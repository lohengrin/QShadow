#include "capture.h"
#include "decoder.h"

#include <QBuffer>
#include <QImage>
#include <QPainter>
#include <QtTest>

#include <algorithm>
#include <condition_variable>
#include <cmath>
#include <mutex>
#include <set>

namespace {

// Runs a Decoder on one or more frames and collects what it delivers.
class Harness {
public:
    explicit Harness(bool gpuYuv) : m_decoder(&m_queue, &m_stats, [this](const VideoFramePtr &f,
                                                                       std::chrono::steady_clock::time_point) {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_delivered;
        m_last = f;
        m_pointers.push_back(f.get());
        if (m_keep)
            m_kept.push_back(f);
        m_cv.notify_all();
    })
    {
        m_decoder.setGpuYuvCheck([gpuYuv] { return gpuYuv; });
        m_decoder.start();
    }
    ~Harness()
    {
        m_decoder.stop();
        m_decoder.join();
    }

    // Pushes a frame and waits until the decoder has either delivered or dropped it.
    bool feed(Frame frame)
    {
        const uint64_t before = m_stats.decoded.load() + m_stats.droppedDecode.load();
        frame.stamp = std::chrono::steady_clock::now();
        m_queue.push(std::move(frame));
        for (int i = 0; i < 2000; ++i) {
            if (m_stats.decoded.load() + m_stats.droppedDecode.load() > before)
                return true;
            QThread::msleep(2);
        }
        return false;
    }

    VideoFramePtr last()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_last;
    }
    void dropLast()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_last.reset();
    }
    std::vector<const void *> pointers()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_pointers;
    }
    int delivered()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_delivered;
    }
    Stats &stats() { return m_stats; }
    void keepFrames(bool keep) { m_keep = keep; }

private:
    Stats m_stats;
    FrameQueue m_queue{2};
    std::mutex m_mutex;
    std::condition_variable m_cv;
    VideoFramePtr m_last;
    std::vector<const void *> m_pointers;
    std::vector<VideoFramePtr> m_kept;
    int m_delivered = 0;
    bool m_keep = false;
    Decoder m_decoder;
};

Frame jpegFrame(const QByteArray &jpeg)
{
    Frame f;
    f.data = jpeg;
    f.format = device::PixelFormat::Mjpg;
    return f;
}

Frame yuyvFrame(int w, int h, int stride, const QByteArray &data)
{
    Frame f;
    f.data = data;
    f.format = device::PixelFormat::Yuyv;
    f.width = w;
    f.height = h;
    f.stride = stride;
    return f;
}

QImage testImage(int w, int h)
{
    QImage img(w, h, QImage::Format_RGB888);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x)
            img.setPixel(x, y, qRgb(int(127 + 127 * std::sin(x / 5.0)),
                                    int(127 + 127 * std::sin(y / 7.0 + x / 11.0)),
                                    255 * x / std::max(1, w)));
    }
    QPainter p(&img);
    p.fillRect(w / 4, h / 4, w / 3, h / 3, QColor(220, 30, 30));
    p.fillRect(w / 2, h / 2, w / 4, h / 4, QColor(20, 40, 230));
    return img;
}

QByteArray toJpeg(const QImage &img)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, "JPEG", 90);
    return bytes;
}

// CPU model of shaders/yuv2rgb.comp: Y fetched exactly, chroma bilinear (clamp to
// edge) at the pixel centre scaled by the true subsampling factor.
void emulateYuvShader(const VideoFrame &f, int x, int y, int rgb[3])
{
    const auto fetch = [&](int pl, int px, int py) {
        px = std::clamp(px, 0, f.planes[pl].width - 1);
        py = std::clamp(py, 0, f.planes[pl].height - 1);
        return float(uint8_t(f.planes[pl].data[py * f.planes[pl].stride + px])) / 255.0f;
    };
    const int w = f.planes[0].width;
    const int h = f.planes[0].height;
    const int cw = f.planes[1].width;
    const int ch = f.planes[1].height;
    const int fx = std::max(1, (w + cw / 2) / cw);
    const int fy = std::max(1, (h + ch / 2) / ch);
    const auto bilinear = [&](int pl) {
        const float u = (float(x) + 0.5f) / float(fx * cw);
        const float v = (float(y) + 0.5f) / float(fy * ch);
        const float sx = u * float(cw) - 0.5f;
        const float sy = v * float(ch) - 0.5f;
        const int x0 = int(std::floor(sx));
        const int y0 = int(std::floor(sy));
        const float tx = sx - float(x0);
        const float ty = sy - float(y0);
        return (fetch(pl, x0, y0) * (1 - tx) + fetch(pl, x0 + 1, y0) * tx) * (1 - ty)
               + (fetch(pl, x0, y0 + 1) * (1 - tx) + fetch(pl, x0 + 1, y0 + 1) * tx) * ty;
    };
    const float Y = fetch(0, x, y);
    const float cb = bilinear(1) - 128.0f / 255.0f;
    const float cr = bilinear(2) - 128.0f / 255.0f;
    const float c[3] = {Y + 1.402f * cr, Y - 0.344136f * cb - 0.714136f * cr, Y + 1.772f * cb};
    for (int i = 0; i < 3; ++i)
        rgb[i] = int(std::lround(std::clamp(c[i], 0.0f, 1.0f) * 255.0f));
}

} // namespace

class TstPipeline : public QObject {
    Q_OBJECT
private slots:
    // ---- FrameQueue ----
    void queueDropsTheOldestWhenFull()
    {
        FrameQueue q(2);
        for (int i = 1; i <= 3; ++i) {
            Frame f;
            f.width = i;
            q.push(std::move(f));
        }
        QCOMPARE(q.drops(), uint64_t(1));
        Frame out;
        QVERIFY(q.pop(&out, false, 10));
        QCOMPARE(out.width, 2); // frame 1 was dropped
        QVERIFY(q.pop(&out, false, 10));
        QCOMPARE(out.width, 3);
        QVERIFY(!q.pop(&out, false, 5)); // empty: times out
    }

    void queueClearEmptiesIt()
    {
        FrameQueue q(3);
        q.push(Frame());
        q.push(Frame());
        q.clear();
        Frame out;
        QVERIFY(!q.pop(&out, false, 5));
    }

    // ---- MJPG ----
    void jpegDecodesToRgbaOnTheCpu()
    {
        Harness h(/*gpuYuv=*/false);
        const QImage src = testImage(64, 48);
        QVERIFY(h.feed(jpegFrame(toJpeg(src))));
        const VideoFramePtr f = h.last();
        QVERIFY(f);
        QCOMPARE(f->kind, VideoFrame::Kind::Rgba);
        QCOMPARE(f->size(), QSize(64, 48));
        QCOMPARE(h.stats().frameYuv.load(), false);
        // JPEG is lossy but close to the source.
        long diff = 0;
        for (int y = 0; y < 48; ++y) {
            for (int x = 0; x < 64; ++x)
                diff += qAbs(qRed(f->rgba.pixel(x, y)) - qRed(src.pixel(x, y)));
        }
        QVERIFY(double(diff) / (64 * 48) < 8.0);
    }

    void planarYuvMatchesTheCpuConversion_data()
    {
        QTest::addColumn<int>("w");
        QTest::addColumn<int>("h");
        QTest::newRow("even") << 64 << 48;
        QTest::newRow("odd width and height") << 63 << 47;
        QTest::newRow("wide") << 200 << 30;
    }
    void planarYuvMatchesTheCpuConversion()
    {
        QFETCH(int, w);
        QFETCH(int, h);
        const QByteArray jpeg = toJpeg(testImage(w, h));

        Harness cpu(false);
        QVERIFY(cpu.feed(jpegFrame(jpeg)));
        Harness gpu(true);
        QVERIFY(gpu.feed(jpegFrame(jpeg)));
        const VideoFramePtr ref = cpu.last();
        const VideoFramePtr yuv = gpu.last();
        QVERIFY(ref && yuv);
        QCOMPARE(yuv->kind, VideoFrame::Kind::Yuv);
        QCOMPARE(gpu.stats().frameYuv.load(), true);
        QCOMPARE(yuv->planes[0].width, w);
        QCOMPARE(yuv->planes[0].height, h);
        // Chroma is subsampled by at most 2 per axis and rounds up.
        for (int i = 1; i < 3; ++i) {
            QVERIFY(yuv->planes[i].width == w || yuv->planes[i].width == (w + 1) / 2);
            QVERIFY(yuv->planes[i].height == h || yuv->planes[i].height == (h + 1) / 2);
            QVERIFY(yuv->planes[i].stride >= yuv->planes[i].width);
        }

        int maxDiff = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int rgb[3];
                emulateYuvShader(*yuv, x, y, rgb);
                const QRgb p = ref->rgba.pixel(x, y);
                maxDiff = std::max({maxDiff, qAbs(rgb[0] - qRed(p)), qAbs(rgb[1] - qGreen(p)),
                                    qAbs(rgb[2] - qBlue(p))});
            }
        }
        QVERIFY2(maxDiff <= 3, qPrintable(QString::number(maxDiff)));
    }

    void corruptJpegIsDroppedAndTheDecoderRecovers()
    {
        Harness h(false);
        QByteArray good = toJpeg(testImage(32, 32));
        QVERIFY(h.feed(jpegFrame(QByteArray("this is not a jpeg at all"))));
        QVERIFY(h.feed(jpegFrame(good.left(good.size() / 2)))); // truncated
        QVERIFY(h.stats().droppedDecode.load() >= 1);
        QVERIFY(h.feed(jpegFrame(good)));
        QVERIFY(h.last());
        QCOMPARE(h.last()->size(), QSize(32, 32));
    }

    void framesAreRecycledOnceReleased()
    {
        Harness h(false);
        const QByteArray jpeg = toJpeg(testImage(32, 32));
        for (int i = 0; i < 12; ++i) {
            QVERIFY(h.feed(jpegFrame(jpeg)));
            h.dropLast(); // the consumer lets go
        }
        // With every frame released the pool hands the same storage back.
        const auto ptrs = h.pointers();
        QCOMPARE(int(ptrs.size()), 12);
        QVERIFY(std::set<const void *>(ptrs.begin(), ptrs.end()).size() <= 2);
    }

    void heldFramesAreNeverOverwritten()
    {
        Harness h(false);
        h.keepFrames(true);
        const QByteArray jpeg = toJpeg(testImage(32, 32));
        for (int i = 0; i < 6; ++i)
            QVERIFY(h.feed(jpegFrame(jpeg)));
        // Everything is still held, so each delivery needed distinct storage.
        const auto ptrs = h.pointers();
        QCOMPARE(std::set<const void *>(ptrs.begin(), ptrs.end()).size(), ptrs.size());
    }

    // ---- YUYV ----
    void yuyvConvertsToRgbaOnTheCpu()
    {
        Harness h(false);
        // 4 pixels: black, white, red, mid-grey (BT.601 limited range).
        const uint8_t px[] = {16, 128, 16, 128,   // black, black
                              235, 128, 235, 128, // white, white
                              81, 90, 81, 240,    // red (Cb=90, Cr=240)
                              126, 128, 126, 128};
        QVERIFY(h.feed(yuyvFrame(8, 1, 16, QByteArray(reinterpret_cast<const char *>(px), 16))));
        const VideoFramePtr f = h.last();
        QVERIFY(f);
        QCOMPARE(f->kind, VideoFrame::Kind::Rgba);
        QCOMPARE(f->size(), QSize(8, 1));
        QVERIFY(qRed(f->rgba.pixel(0, 0)) <= 1 && qGreen(f->rgba.pixel(0, 0)) <= 1);
        QVERIFY(qRed(f->rgba.pixel(2, 0)) >= 254 && qBlue(f->rgba.pixel(3, 0)) >= 254);
        QVERIFY(qRed(f->rgba.pixel(4, 0)) > 240);
        QVERIFY(qGreen(f->rgba.pixel(4, 0)) < 16 && qBlue(f->rgba.pixel(4, 0)) < 16);
        const int grey = qRed(f->rgba.pixel(6, 0));
        QVERIFY(grey > 120 && grey < 135);
        QCOMPARE(qAlpha(f->rgba.pixel(0, 0)), 255);
    }

    void yuyvIsPassedThroughWithoutACopyWhenTheGpuCanConvert()
    {
        Harness h(true);
        QByteArray raw(16 * 4, 'x');
        const char *original = raw.constData();
        QVERIFY(h.feed(yuyvFrame(8, 4, 16, raw)));
        const VideoFramePtr f = h.last();
        QVERIFY(f);
        QCOMPARE(f->kind, VideoFrame::Kind::Yuyv);
        QCOMPARE(f->planes[0].width, 8);
        QCOMPARE(f->planes[0].height, 4);
        QCOMPARE(f->planes[0].stride, 16);
        QCOMPARE(f->planes[0].data.constData(), original); // shared, not copied
        QCOMPARE(h.stats().frameYuv.load(), true);
    }

    void yuyvFallsBackToTheCpuWhenTheGpuCannotTakeIt()
    {
        Harness h(true);
        // Odd width: a texel (two pixels) would straddle the row end.
        QVERIFY(h.feed(yuyvFrame(7, 2, 16, QByteArray(32, 'x'))));
        QCOMPARE(h.last()->kind, VideoFrame::Kind::Rgba);
        // Stride not a multiple of 4 bytes.
        QVERIFY(h.feed(yuyvFrame(6, 2, 14, QByteArray(28, 'x'))));
        QCOMPARE(h.last()->kind, VideoFrame::Kind::Rgba);
    }
};

QTEST_GUILESS_MAIN(TstPipeline)
#include "tst_pipeline.moc"
