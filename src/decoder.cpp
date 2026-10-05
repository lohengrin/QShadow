#include "decoder.h"

#include <QMetaObject>
#include <QMetaType>

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

} // namespace

Decoder::Decoder(FrameQueue *queue, Stats *stats, QObject *frameTarget, const char *slot)
    : m_queue(queue), m_stats(stats), m_target(frameTarget), m_slot(slot)
{
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
void Decoder::convertYuyv(const Frame &frame)
{
    const int w = frame.width;
    const int h = frame.height;
    if (m_frame.isNull() || m_frame.width() != w || m_frame.height() != h
        || m_frame.format() != QImage::Format_RGBA8888)
        m_frame = QImage(w, h, QImage::Format_RGBA8888);

    const auto clamp8 = [](int v) { return uint32_t(v < 0 ? 0 : (v > 255 ? 255 : v)); };
    const auto *base = reinterpret_cast<const unsigned char *>(frame.data.constData());
    for (int y = 0; y < h; ++y) {
        const unsigned char *s = base + size_t(y) * size_t(frame.stride);
        uint32_t *d = reinterpret_cast<uint32_t *>(m_frame.scanLine(y));
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
            convertYuyv(frame);
            m_stats->frameW.store(frame.width);
            m_stats->frameH.store(frame.height);
            m_stats->decoded.fetch_add(1);
            const auto t1 = std::chrono::steady_clock::now();
            m_stats->decodeNs.fetch_add(uint64_t(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
            if (m_target)
                QMetaObject::invokeMethod(m_target, m_slot, Qt::QueuedConnection,
                                          Q_ARG(QImage, m_frame.copy()));
            continue;
        }

        const unsigned char *src =
            reinterpret_cast<const unsigned char *>(frame.data.constData());
        const unsigned long srcLen = static_cast<unsigned long>(frame.data.size());

        volatile bool ok = false;
        if (setjmp(err.jb)) {
            // A corrupt frame landed us here; skip it and move on.
            jpeg_abort_decompress(&cinfo);
            m_stats->droppedDecode.fetch_add(1);
            continue;
        }

        jpeg_mem_src(&cinfo, src, srcLen);
        if (jpeg_read_header(&cinfo, TRUE) == JPEG_HEADER_OK) {
            cinfo.out_color_space = JCS_EXT_RGBA;
            jpeg_start_decompress(&cinfo);

            const int w = int(cinfo.output_width);
            const int h = int(cinfo.output_height);
            if (m_frame.isNull() || m_frame.width() != w || m_frame.height() != h
                || m_frame.format() != QImage::Format_RGBA8888)
                m_frame = QImage(w, h, QImage::Format_RGBA8888);

            // RGBA8888 is tightly packed (one pixel per uint32), so every row
            // starts at a 4-byte-aligned offset: the Vulkan upload path can use
            // a single contiguous copy with bufferRowLength == width.
            JSAMPROW row = m_frame.bits();
            while (cinfo.output_scanline < cinfo.output_height) {
                jpeg_read_scanlines(&cinfo, &row, 1);
                row += m_frame.bytesPerLine();
            }
            jpeg_finish_decompress(&cinfo);

            m_stats->frameW.store(w);
            m_stats->frameH.store(h);
            m_stats->decoded.fetch_add(1);
            ok = true;
        } else {
            m_stats->droppedDecode.fetch_add(1);
        }

        const auto t1 = std::chrono::steady_clock::now();
        m_stats->decodeNs.fetch_add(
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));

        if (ok && m_target) {
            // Copy so the queued event owns private pixel data while we reuse
            // m_frame for the next decode (implicit sharing would otherwise
            // alias frames in flight).
            QMetaObject::invokeMethod(m_target, m_slot,
                                      Qt::QueuedConnection, Q_ARG(QImage, m_frame.copy()));
        }
    }

    jpeg_destroy_decompress(&cinfo);
}