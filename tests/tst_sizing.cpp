#include "videowidget.h"

#include <QtTest>

using Mode = VideoWidget::ShaderMode;

class TstSizing : public QObject {
    Q_OBJECT
private slots:
    void roundToEvenAlwaysGivesAnEvenSize_data()
    {
        QTest::addColumn<float>("value");
        for (float v : {0.0f, 1.0f, 2.0f, 719.6f, 720.0f, 721.0f, 721.4f, 959.5f, 1079.9f})
            QTest::newRow(qPrintable(QString::number(double(v)))) << v;
    }
    void roundToEvenAlwaysGivesAnEvenSize()
    {
        QFETCH(float, value);
        const int r = VideoWidget::roundToEven(value);
        QVERIFY(r >= 2);
        QCOMPARE(r % 2, 0);
        if (value >= 2.0f)
            QVERIFY(std::abs(float(r) - value) <= 1.0f);
    }

    void fsrModesAreIdentified()
    {
        for (Mode m : {Mode::None, Mode::Bilinear, Mode::Hq4x, Mode::Xbrz, Mode::CrtGeom,
                       Mode::CrtRoyale})
            QVERIFY(!VideoWidget::isFsrMode(int(m)));
        for (Mode m : {Mode::FsrUltraQuality, Mode::FsrQuality, Mode::FsrBalanced,
                       Mode::FsrPerformance, Mode::FsrSharpenOnly})
            QVERIFY(VideoWidget::isFsrMode(int(m)));
    }

    void fsrPresetRatios()
    {
        QCOMPARE(VideoWidget::fsrPresetRatio(int(Mode::FsrUltraQuality)), 1.3f);
        QCOMPARE(VideoWidget::fsrPresetRatio(int(Mode::FsrQuality)), 1.5f);
        QCOMPARE(VideoWidget::fsrPresetRatio(int(Mode::FsrBalanced)), 1.7f);
        QCOMPARE(VideoWidget::fsrPresetRatio(int(Mode::FsrPerformance)), 2.0f);
        QCOMPARE(VideoWidget::fsrPresetRatio(int(Mode::FsrSharpenOnly)), 1.0f);
    }

    void pipelineOutputSizePerMode()
    {
        const QSize f(640, 480);
        QCOMPARE(VideoWidget::pipelineOutputSize(f, int(Mode::None), 1), f);
        QCOMPARE(VideoWidget::pipelineOutputSize(f, int(Mode::Xbrz), 1), f);
        QCOMPARE(VideoWidget::pipelineOutputSize(f, int(Mode::Hq4x), 1), QSize(2560, 1920));
        QCOMPARE(VideoWidget::pipelineOutputSize(f, int(Mode::FsrQuality), 1), QSize(960, 720));
        QCOMPARE(VideoWidget::pipelineOutputSize(f, int(Mode::FsrSharpenOnly), 1), f);
    }

    void pipelineOutputSizeAppliesTheDownscaleFirst()
    {
        const QSize f(1280, 720);
        QCOMPARE(VideoWidget::pipelineOutputSize(f, int(Mode::None), 2), QSize(640, 360));
        QCOMPARE(VideoWidget::pipelineOutputSize(f, int(Mode::None), 4), QSize(320, 180));
        QCOMPARE(VideoWidget::pipelineOutputSize(f, int(Mode::Hq4x), 4), QSize(1280, 720));
        QCOMPARE(VideoWidget::pipelineOutputSize(f, int(Mode::FsrPerformance), 2),
                 QSize(1280, 720));
    }

    void fsrUpscaleIsCappedByTheTarget()
    {
        const QSize src(640, 480);
        QCOMPARE(VideoWidget::fsrUpscaleSize(src, QSize(4000, 3000), int(Mode::FsrQuality)),
                 QSize(960, 720));
        QCOMPARE(VideoWidget::fsrUpscaleSize(src, QSize(800, 600), int(Mode::FsrQuality)),
                 QSize(800, 600));
        // A target smaller than the source caps the result.
        QCOMPARE(VideoWidget::fsrUpscaleSize(src, QSize(100, 100), int(Mode::FsrQuality)),
                 QSize(100, 100));
        QCOMPARE(VideoWidget::fsrUpscaleSize(src, QSize(4000, 3000), int(Mode::FsrSharpenOnly)),
                 src);
    }
};

QTEST_GUILESS_MAIN(TstSizing)
#include "tst_sizing.moc"
