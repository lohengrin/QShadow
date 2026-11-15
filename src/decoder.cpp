// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#include "decoder.h"

#include <jpeglib.h>
#include <csetjmp>
#include <chrono>

namespace {

struct JpegError {
    jpeg_error_mgr pub;
    jmp_buf jb;
};

void onJpegError(j_common_ptr cinfo)
{
    JpegError *err = reinterpret_cast<JpegError *>(cinfo->err);
    longjmp(err->jb, 1);
}

// libjpeg reports errors by longjmp. Each setjmp scope lives in its own small
// function holding only trivial locals, so nothing with a destructor is ever
// skipped and no variable is left indeterminate by the jump.

// Parses the JPEG header.
bool headerJpeg(jpeg_decompress_struct &cinfo, JpegError &err, const Frame &frame)
{
    if (setjmp(err.jb)) {
        jpeg_abort_decompress(&cinfo);
        return false;
    }
    jpeg_mem_src(&cinfo, reinterpret_cast<const unsigned char *>(frame.data.constData()),
                 static_cast<unsigned long>(frame.data.size()));
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_abort_decompress(&cinfo);
        return false;
    }
    return true;
}

// Plain 3-component YCbCr whose luma is full size and whose chroma planes are
// 1x or 2x subsampled (4:4:4, 4:2:2, 4:2:0, 4:4:0): the cases the GPU shader
// and the raw-data path handle.
bool isPlanarYuv(const jpeg_decompress_struct &c)
{
    if (c.jpeg_color_space != JCS_YCbCr || c.num_components != 3)
        return false;
    const int mh = c.max_h_samp_factor;
    const int mv = c.max_v_samp_factor;
    if (mh > 2 || mv > 2)
        return false;
    const jpeg_component_info *ci = c.comp_info;
    if (ci[0].h_samp_factor != mh || ci[0].v_samp_factor != mv)
        return false;
    for (int i = 1; i < 3; ++i) {
        if (ci[i].h_samp_factor < 1 || mh % ci[i].h_samp_factor != 0
            || ci[i].v_samp_factor < 1 || mv % ci[i].v_samp_factor != 0)
            return false;
    }
    return true;
}

// Starts decompression either to RGBA, or (raw) to the JPEG's own planes with
// no colour conversion and no chroma upsampling.
bool startJpeg(jpeg_decompress_struct &cinfo, JpegError &err, bool raw)
{
    if (setjmp(err.jb)) {
        jpeg_abort_decompress(&cinfo);
        return false;
    }
    if (raw) {
        cinfo.out_color_space = JCS_YCbCr;
        cinfo.raw_data_out = TRUE;
        cinfo.do_fancy_upsampling = FALSE;
    } else {
        cinfo.out_color_space = JCS_EXT_RGBA;
        cinfo.raw_data_out = FALSE;
        cinfo.do_fancy_upsampling = TRUE;
    }
    jpeg_start_decompress(&cinfo);
    return true;
}

// Reads all scanlines into a tightly packed RGBA8888 buffer (one pixel per
// uint32, so every row is 4-byte aligned and the Vulkan upload can use a
// single contiguous copy with bufferRowLength == width).
bool readJpeg(jpeg_decompress_struct &cinfo, JpegError &err, unsigned char *dst,
              qsizetype stride)
{
    if (setjmp(err.jb)) {
        jpeg_abort_decompress(&cinfo);
        return false;
    }
    JSAMPROW row = dst;
    while (cinfo.output_scanline < cinfo.output_height) {
        jpeg_read_scanlines(&cinfo, &row, 1);
        row += stride;
    }
    jpeg_finish_decompress(&cinfo);
    return true;
}

// Reads the raw component planes, one iMCU row (8 * max_v_samp_factor luma
// lines) at a time. Each plane buffer must hold whole iMCU rows.
bool readRawJpeg(jpeg_decompress_struct &cinfo, JpegError &err, unsigned char *const planes[3],
                 const int strides[3])
{
    if (setjmp(err.jb)) {
        jpeg_abort_decompress(&cinfo);
        return false;
    }
    const int maxV = cinfo.max_v_samp_factor;
    const int lines = maxV * DCTSIZE;
    JSAMPROW rows[3][2 * DCTSIZE]; // at most 2 * 8 rows per component (maxV <= 2)
    JSAMPARRAY arrays[3] = {rows[0], rows[1], rows[2]};
    while (cinfo.output_scanline < cinfo.output_height) {
        for (int c = 0; c < 3; ++c) {
            const int v = cinfo.comp_info[c].v_samp_factor;
            const int first = int(cinfo.output_scanline) * v / maxV;
            for (int r = 0; r < v * DCTSIZE; ++r)
                rows[c][r] = planes[c] + size_t(first + r) * size_t(strides[c]);
        }
        if (jpeg_read_raw_data(&cinfo, arrays, JDIMENSION(lines)) == 0) {
            jpeg_abort_decompress(&cinfo);
            return false;
        }
    }
    jpeg_finish_decompress(&cinfo);
    return true;
}

YuvLayout yuvLayout(const jpeg_decompress_struct &c)
{
    YuvLayout l{};
    const int lines = c.max_v_samp_factor * DCTSIZE;
    const int imcuRows = (int(c.output_height) + lines - 1) / lines;
    for (int i = 0; i < 3; ++i) {
        const jpeg_component_info &ci = c.comp_info[i];
        l.width[i] = (int(c.output_width) * ci.h_samp_factor + c.max_h_samp_factor - 1)
                     / c.max_h_samp_factor;
        l.height[i] = (int(c.output_height) * ci.v_samp_factor + c.max_v_samp_factor - 1)
                      / c.max_v_samp_factor;
        l.stride[i] = (int(ci.width_in_blocks) * DCTSIZE + 15) & ~15;
        l.rows[i] = imcuRows * ci.v_samp_factor * DCTSIZE;
    }
    return l;
}

} // namespace

Decoder::Decoder(FrameQueue *queue, Stats *stats, FrameSink sink)
    : m_queue(queue), m_stats(stats), m_sink(std::move(sink))
{
}

VideoFramePtr Decoder::acquireRgba(int w, int h)
{
    for (VideoFramePtr &f : m_pool) {
        if (f && f.use_count() == 1 && f->kind == VideoFrame::Kind::Rgba
            && f->rgba.width() == w && f->rgba.height() == h)
            return f;
    }
    // Pool exhausted (consumers still hold the others) or size/kind changed:
    // replace the next slot round-robin. Consumers keep their own reference.
    VideoFramePtr &slot = m_pool[m_poolNext];
    m_poolNext = (m_poolNext + 1) % m_pool.size();
    slot = std::make_shared<VideoFrame>();
    slot->kind = VideoFrame::Kind::Rgba;
    slot->rgba = QImage(w, h, QImage::Format_RGBA8888);
    return slot;
}

VideoFramePtr Decoder::acquireYuv(const YuvLayout &l)
{
    const auto matches = [&](const VideoFrame &f) {
        if (f.kind != VideoFrame::Kind::Yuv)
            return false;
        for (int i = 0; i < 3; ++i) {
            if (f.planes[i].width != l.width[i] || f.planes[i].height != l.height[i]
                || f.planes[i].stride != l.stride[i])
                return false;
        }
        return true;
    };
    for (VideoFramePtr &f : m_pool) {
        if (f && f.use_count() == 1 && matches(*f))
            return f;
    }
    VideoFramePtr &slot = m_pool[m_poolNext];
    m_poolNext = (m_poolNext + 1) % m_pool.size();
    slot = std::make_shared<VideoFrame>();
    slot->kind = VideoFrame::Kind::Yuv;
    for (int i = 0; i < 3; ++i) {
        VideoFrame::Plane &p = slot->planes[i];
        p.width = l.width[i];
        p.height = l.height[i];
        p.stride = l.stride[i];
        // A little slack past the last row: SIMD IDCT stores may touch it.
        p.data = QByteArray(qsizetype(l.stride[i]) * l.rows[i] + 64, Qt::Uninitialized);
    }
    return slot;
}

Decoder::~Decoder()
{
    stop();
    join();
}

void Decoder::start()
{
    m_thread = std::thread([this] { run(); });
}

void Decoder::stop()
{
    m_stop.store(true);
}

void Decoder::join()
{
    if (m_thread.joinable())
        m_thread.join();
}

namespace {

// Fixed-point (12 fractional bits) YCbCr -> RGB for a matrix and range.
struct YuvCoeffs {
    int yScale; // luma gain (1.0, or 255/219 for limited range)
    int yOff;   // luma black level (0 or 16)
    int rv, gu, gv, bu;
};

YuvCoeffs coeffsFor(const device::ColorInfo &c)
{
    constexpr double kOne = 4096.0;
    const double chroma = c.fullRange ? 1.0 : 255.0 / 224.0;
    const bool bt709 = c.matrix == device::ColorMatrix::Bt709;
    const double rv = bt709 ? 1.5748 : 1.402;
    const double gu = bt709 ? -0.187324 : -0.344136;
    const double gv = bt709 ? -0.468124 : -0.714136;
    const double bu = bt709 ? 1.8556 : 1.772;
    YuvCoeffs k;
    k.yScale = int((c.fullRange ? 1.0 : 255.0 / 219.0) * kOne + 0.5);
    k.yOff = c.fullRange ? 0 : 16;
    k.rv = int(rv * chroma * kOne + (rv < 0 ? -0.5 : 0.5));
    k.gu = int(gu * chroma * kOne + (gu < 0 ? -0.5 : 0.5));
    k.gv = int(gv * chroma * kOne + (gv < 0 ? -0.5 : 0.5));
    k.bu = int(bu * chroma * kOne + (bu < 0 ? -0.5 : 0.5));
    return k;
}

inline uint32_t clamp8(int v)
{
    return uint32_t(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// Memory order R,G,B,A == RGBA8888 on little-endian.
inline uint32_t pack(const YuvCoeffs &k, int y, int u, int v)
{
    const int c = (y - k.yOff) * k.yScale + 2048;
    return clamp8((c + k.rv * v) >> 12) | (clamp8((c + k.gu * u + k.gv * v) >> 12) << 8)
           | (clamp8((c + k.bu * u) >> 12) << 16) | 0xff000000u;
}

} // namespace

// Raw frame -> RGBA8888 on the CPU. Chroma is shared by pixel pairs (4:2:2) or
// 2x2 blocks (NV12), nearest. The full-range BT.601 case reproduces the
// ShadowCast's YUYV exactly as the GPU shader does.
void Decoder::convertRaw(const Frame &frame, const device::ColorInfo &color, QImage &dst)
{
    const int w = frame.width;
    const int h = frame.height;
    const auto *base = reinterpret_cast<const unsigned char *>(frame.data.constData());
    const size_t stride = size_t(frame.stride);
    const YuvCoeffs k = coeffsFor(color);

    switch (frame.format) {
    case device::PixelFormat::Yuyv:
    case device::PixelFormat::Uyvy: {
        const bool yuyv = frame.format == device::PixelFormat::Yuyv;
        const int y0 = yuyv ? 0 : 1, u = yuyv ? 1 : 0, y1 = yuyv ? 2 : 3, v = yuyv ? 3 : 2;
        for (int y = 0; y < h; ++y) {
            const unsigned char *s = base + size_t(y) * stride;
            uint32_t *d = reinterpret_cast<uint32_t *>(dst.scanLine(y));
            for (int x = 0; x < w; x += 2, s += 4) {
                const int cb = s[u] - 128;
                const int cr = s[v] - 128;
                d[x] = pack(k, s[y0], cb, cr);
                if (x + 1 < w) // w may be odd: no pixel past the row
                    d[x + 1] = pack(k, s[y1], cb, cr);
            }
        }
        break;
    }
    case device::PixelFormat::Nv12: {
        const unsigned char *chroma = base + stride * size_t(h);
        for (int y = 0; y < h; ++y) {
            const unsigned char *ys = base + size_t(y) * stride;
            const unsigned char *cs = chroma + size_t(y >> 1) * stride;
            uint32_t *d = reinterpret_cast<uint32_t *>(dst.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const unsigned char *c = cs + (x & ~1);
                d[x] = pack(k, ys[x], c[0] - 128, c[1] - 128);
            }
        }
        break;
    }
    case device::PixelFormat::Rgb24:
    case device::PixelFormat::Bgr24: {
        const bool rgb = frame.format == device::PixelFormat::Rgb24;
        for (int y = 0; y < h; ++y) {
            const unsigned char *s = base + size_t(y) * stride;
            uint32_t *d = reinterpret_cast<uint32_t *>(dst.scanLine(y));
            for (int x = 0; x < w; ++x, s += 3) {
                const uint32_t r = rgb ? s[0] : s[2];
                const uint32_t b = rgb ? s[2] : s[0];
                d[x] = r | (uint32_t(s[1]) << 8) | (b << 16) | 0xff000000u;
            }
        }
        break;
    }
    case device::PixelFormat::Bgrx: {
        for (int y = 0; y < h; ++y) {
            const unsigned char *s = base + size_t(y) * stride;
            uint32_t *d = reinterpret_cast<uint32_t *>(dst.scanLine(y));
            for (int x = 0; x < w; ++x, s += 4)
                d[x] = uint32_t(s[2]) | (uint32_t(s[1]) << 8) | (uint32_t(s[0]) << 16)
                       | 0xff000000u;
        }
        break;
    }
    case device::PixelFormat::Mjpg:
        break;
    }
}

void Decoder::run()
{
    jpeg_decompress_struct cinfo;
    JpegError err;
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = onJpegError;
    jpeg_create_decompress(&cinfo);

    Frame frame;
    while (!m_stop.load()) {
        if (!m_queue->pop(&frame, m_stop.load(), 50))
            continue;

        const auto t0 = std::chrono::steady_clock::now();

        VideoFramePtr out;
        bool ok = false;
        int w = 0;
        int h = 0;

        if (frame.format != device::PixelFormat::Mjpg) {
            w = frame.width;
            h = frame.height;
            const device::ColorInfo color = m_colorOverride.value_or(frame.color);
            const bool sane = w > 0 && h > 0 && frame.stride >= device::minStride(frame.format, w)
                              && size_t(frame.data.size())
                                     >= device::rawFrameBytes(frame.format, frame.stride, h);
            if (!sane) {
                ok = false;
            } else if (frame.format == device::PixelFormat::Yuyv && color == device::ColorInfo{}
                       && m_gpuYuv && m_gpuYuv() && (w % 2) == 0 && (frame.stride % 4) == 0) {
                // The GPU converts (full-range BT.601): hand over the capture
                // buffer as it is (a shared reference, no copy). Texels are 4
                // bytes = 2 pixels.
                out = std::make_shared<VideoFrame>();
                out->kind = VideoFrame::Kind::Yuyv;
                out->planes[0].width = w;
                out->planes[0].height = h;
                out->planes[0].stride = frame.stride;
                out->planes[0].data = frame.data;
                ok = true;
            } else {
                out = acquireRgba(w, h);
                convertRaw(frame, color, out->rgba);
                ok = true;
            }
        } else if (headerJpeg(cinfo, err, frame)) {
            const bool raw = m_gpuYuv && m_gpuYuv() && isPlanarYuv(cinfo);
            if (startJpeg(cinfo, err, raw)) {
                w = int(cinfo.output_width);
                h = int(cinfo.output_height);
                if (raw) {
                    const YuvLayout layout = yuvLayout(cinfo);
                    out = acquireYuv(layout);
                    unsigned char *planes[3] = {
                        reinterpret_cast<unsigned char *>(out->planes[0].data.data()),
                        reinterpret_cast<unsigned char *>(out->planes[1].data.data()),
                        reinterpret_cast<unsigned char *>(out->planes[2].data.data())};
                    ok = readRawJpeg(cinfo, err, planes, layout.stride);
                } else {
                    out = acquireRgba(w, h);
                    ok = readJpeg(cinfo, err, out->rgba.bits(), out->rgba.bytesPerLine());
                }
            }
        }

        if (ok) {
            m_stats->frameW.store(w);
            m_stats->frameH.store(h);
            m_stats->frameYuv.store(out->kind != VideoFrame::Kind::Rgba);
            m_stats->decoded.fetch_add(1);
            const auto t1 = std::chrono::steady_clock::now();
            m_stats->decodeNs.fetch_add(uint64_t(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
            if (m_sink)
                m_sink(out, frame.stamp);
        } else {
            m_stats->droppedDecode.fetch_add(1);
        }
    }

    jpeg_destroy_decompress(&cinfo);
}