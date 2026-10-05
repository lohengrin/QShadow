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

// Parses the header and starts decompression; returns the output size.
bool beginJpeg(jpeg_decompress_struct &cinfo, JpegError &err, const Frame &frame, int *w,
               int *h)
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
    cinfo.out_color_space = JCS_EXT_RGBA;
    jpeg_start_decompress(&cinfo);
    *w = int(cinfo.output_width);
    *h = int(cinfo.output_height);
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

} // namespace

Decoder::Decoder(FrameQueue *queue, Stats *stats, FrameSink sink)
    : m_queue(queue), m_stats(stats), m_sink(std::move(sink))
{
}

// Returns a pool image of the right size/format that nobody else holds. The
// reference stays valid for the caller; bits()/scanLine() on it never detach
// because it is only handed out while unshared.
QImage &Decoder::acquireImage(int w, int h)
{
    for (QImage &img : m_pool) {
        if (!img.isNull() && img.isDetached() && img.width() == w && img.height() == h
            && img.format() == QImage::Format_RGBA8888)
            return img;
    }
    // Pool exhausted (consumer still holds the others) or size changed:
    // replace the next slot round-robin. Consumers keep their own reference.
    QImage &slot = m_pool[m_poolNext];
    m_poolNext = (m_poolNext + 1) % m_pool.size();
    slot = QImage(w, h, QImage::Format_RGBA8888);
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

// YUYV (4:2:2, BT.601 limited range, as reported by the driver) -> RGBA8888.
// Integer fixed-point (8 fractional bits); chroma is shared by pixel pairs.
void Decoder::convertYuyv(const Frame &frame, QImage &dst)
{
    const int w = frame.width;
    const int h = frame.height;
    const auto clamp8 = [](int v) { return uint32_t(v < 0 ? 0 : (v > 255 ? 255 : v)); };
    const auto *base = reinterpret_cast<const unsigned char *>(frame.data.constData());
    for (int y = 0; y < h; ++y) {
        const unsigned char *s = base + size_t(y) * size_t(frame.stride);
        uint32_t *d = reinterpret_cast<uint32_t *>(dst.scanLine(y));
        for (int x = 0; x < w; x += 2, s += 4) {
            const int u = s[1] - 128;
            const int v = s[3] - 128;
            const int rc = 409 * v + 128;
            const int gc = -100 * u - 208 * v + 128;
            const int bc = 516 * u + 128;
            for (int k = 0; k < 2; ++k) {
                const int c = 298 * (s[k * 2] - 16);
                // Memory order R,G,B,A == RGBA8888 on little-endian.
                d[x + k] = clamp8((c + rc) >> 8) | (clamp8((c + gc) >> 8) << 8)
                           | (clamp8((c + bc) >> 8) << 16) | 0xff000000u;
            }
        }
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

        if (frame.format == device::PixelFormat::Yuyv) {
            QImage &img = acquireImage(frame.width, frame.height);
            convertYuyv(frame, img);
            m_stats->frameW.store(frame.width);
            m_stats->frameH.store(frame.height);
            m_stats->decoded.fetch_add(1);
            const auto t1 = std::chrono::steady_clock::now();
            m_stats->decodeNs.fetch_add(uint64_t(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
            if (m_sink)
                m_sink(img, frame.stamp);
            continue;
        }

        int w = 0;
        int h = 0;
        bool ok = beginJpeg(cinfo, err, frame, &w, &h);
        if (ok) {
            QImage &img = acquireImage(w, h);
            ok = readJpeg(cinfo, err, img.bits(), img.bytesPerLine());
            if (ok) {
                m_stats->frameW.store(w);
                m_stats->frameH.store(h);
                m_stats->decoded.fetch_add(1);
                const auto t1 = std::chrono::steady_clock::now();
                m_stats->decodeNs.fetch_add(uint64_t(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
                if (m_sink)
                    m_sink(img, frame.stamp);
            }
        }
        if (!ok)
            m_stats->droppedDecode.fetch_add(1);
    }

    jpeg_destroy_decompress(&cinfo);
}