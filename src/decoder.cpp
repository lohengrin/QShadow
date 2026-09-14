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
        const unsigned char *src =
            reinterpret_cast<const unsigned char *>(frame.jpeg.constData());
        const unsigned long srcLen = static_cast<unsigned long>(frame.jpeg.size());

        volatile bool ok = false;
        if (setjmp(err.jb)) {
            // A corrupt frame landed us here; skip it and move on.
            jpeg_abort_decompress(&cinfo);
            m_stats->droppedDecode.fetch_add(1);
            continue;
        }

        jpeg_mem_src(&cinfo, src, srcLen);
        if (jpeg_read_header(&cinfo, TRUE) == JPEG_HEADER_OK) {
            cinfo.out_color_space = JCS_RGB;
            jpeg_start_decompress(&cinfo);

            const int w = int(cinfo.output_width);
            const int h = int(cinfo.output_height);
            if (m_frame.isNull() || m_frame.width() != w || m_frame.height() != h
                || m_frame.format() != QImage::Format_RGB888)
                m_frame = QImage(w, h, QImage::Format_RGB888);

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