#include "mainwindow.h"

#include "decoder.h"
#include "device.h"
#include "videowidget.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>
#include <QVulkanInstance>
#include <QWidget>

#include <chrono>
#include <cmath>

MainWindow::MainWindow(QVulkanInstance *vulkan, QWidget *parent)
    : QMainWindow(parent)
    , m_vulkanInstance(vulkan)
{
    buildUi();
    findDevices();
    loadSettings();
    startPipeline();

    m_statusTimer.setInterval(500);
    connect(&m_statusTimer, &QTimer::timeout, this, &MainWindow::updateStatus);
    m_statusTimer.start();
}

MainWindow::~MainWindow()
{
    stopPipeline();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveSettings();
    QMainWindow::closeEvent(event);
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("QShadow — ShadowCast Viewer"));

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *toolbar = new QToolBar(this);
    toolbar->setMovable(false);

    m_aspectCheck = new QCheckBox(QStringLiteral("Preserve aspect"), toolbar);
    m_aspectCheck->setChecked(true);
    toolbar->addWidget(m_aspectCheck);

    toolbar->addSeparator();
    toolbar->addWidget(new QLabel(QStringLiteral(" Format: "), toolbar));
    m_formatCombo = new QComboBox(toolbar);
    m_formatCombo->setToolTip(QStringLiteral(
        "MJPG: compressed, up to 60 fps but with JPEG artifacts.\n"
        "YUYV: uncompressed and artifact-free, but limited by USB 2.0 "
        "bandwidth to low resolutions / frame rates."));
    toolbar->addWidget(m_formatCombo);

    toolbar->addSeparator();
    toolbar->addWidget(new QLabel(QStringLiteral(" Resolution: "), toolbar));
    m_resCombo = new QComboBox(toolbar);
    toolbar->addWidget(m_resCombo);

    toolbar->addSeparator();
    toolbar->addWidget(new QLabel(QStringLiteral(" Shader: "), toolbar));
    m_shaderCombo = new QComboBox(toolbar);
    m_shaderCombo->addItem(QStringLiteral("None"),
                           int(VideoWidget::ShaderMode::None));
    m_shaderCombo->addItem(QStringLiteral("Bilinear smooth"),
                           int(VideoWidget::ShaderMode::Bilinear));
    m_shaderCombo->addItem(QStringLiteral("HQ4X"),
                           int(VideoWidget::ShaderMode::Hq4x));
    m_shaderCombo->addItem(QStringLiteral("xBRZ"),
                           int(VideoWidget::ShaderMode::Xbrz));
    m_shaderCombo->addItem(QStringLiteral("CRT-Geom"),
                           int(VideoWidget::ShaderMode::CrtGeom));
    m_shaderCombo->addItem(QStringLiteral("CRT-Royale"),
                           int(VideoWidget::ShaderMode::CrtRoyale));
    m_shaderCombo->addItem(QStringLiteral("FSR 1.0 Ultra Quality"),
                           int(VideoWidget::ShaderMode::FsrUltraQuality));
    m_shaderCombo->addItem(QStringLiteral("FSR 1.0 Quality"),
                           int(VideoWidget::ShaderMode::FsrQuality));
    m_shaderCombo->addItem(QStringLiteral("FSR 1.0 Balanced"),
                           int(VideoWidget::ShaderMode::FsrBalanced));
    m_shaderCombo->addItem(QStringLiteral("FSR 1.0 Performance"),
                           int(VideoWidget::ShaderMode::FsrPerformance));
    m_shaderCombo->addItem(QStringLiteral("FSR 1.0 Sharpen only"),
                           int(VideoWidget::ShaderMode::FsrSharpenOnly));
    toolbar->addWidget(m_shaderCombo);

    toolbar->addSeparator();
    toolbar->addWidget(new QLabel(QStringLiteral(" Downscale: "), toolbar));
    m_downscaleCombo = new QComboBox(toolbar);
    m_downscaleCombo->addItem(QStringLiteral("Off"), 1);
    m_downscaleCombo->addItem(QStringLiteral("×2"), 2);
    m_downscaleCombo->addItem(QStringLiteral("×4"), 4);
    m_downscaleCombo->setToolTip(
        QStringLiteral("Nearest-neighbour downscale of the capture, matching "
                       "the source's own pre-upscale, before the video shader"));
    toolbar->addWidget(m_downscaleCombo);

    toolbar->addSeparator();
    toolbar->addWidget(new QLabel(QStringLiteral(" Volume: "), toolbar));
    m_volumeSlider = new QSlider(Qt::Horizontal, toolbar);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setValue(100);
    m_volumeSlider->setFixedWidth(120);
    m_volumeSlider->setToolTip(QStringLiteral("Output volume"));
    toolbar->addWidget(m_volumeSlider);

    toolbar->addSeparator();
    m_fitAction = new QAction(QStringLiteral("Fit"), toolbar);
    m_fitAction->setToolTip(QStringLiteral(
        "Resize the window so every pipeline-output pixel maps 1:1 to a "
        "screen pixel for the current shader and downscale"));
    toolbar->addAction(m_fitAction);
    m_fullscreenAction = new QAction(QStringLiteral("Fullscreen"), toolbar);
    m_fullscreenAction->setCheckable(true);
    m_fullscreenAction->setShortcut(QKeySequence(Qt::Key_F11));
    toolbar->addAction(m_fullscreenAction);
    m_fullscreenAction->setPriority(QAction::LowPriority);
    layout->addWidget(toolbar);

    m_video = new VideoWidget();
    if (m_vulkanInstance)
        m_video->setVulkanInstance(m_vulkanInstance);
    QWidget *videoContainer = QWidget::createWindowContainer(m_video, central);
    videoContainer->setFocusPolicy(Qt::StrongFocus);
    layout->addWidget(videoContainer, 1);
    setCentralWidget(central);

    m_status = new QLabel(this);
    m_statsLabel = new QLabel(this);
    statusBar()->addWidget(m_status, 1);
    statusBar()->addPermanentWidget(m_statsLabel);

    connect(m_aspectCheck, &QCheckBox::toggled, m_video, &VideoWidget::setPreserveAspect);
    connect(m_volumeSlider, &QSlider::valueChanged, this, [this](int v) {
        m_audioSink.setVolume(qreal(v) / 100.0);
    });
    connect(m_formatCombo, &QComboBox::activated, this, &MainWindow::onFormatChanged);
    connect(m_resCombo, &QComboBox::activated, this, &MainWindow::onResolutionChanged);
    connect(m_shaderCombo, qOverload<int>(&QComboBox::activated), this,
            [this](int index) {
                m_video->setShaderMode(VideoWidget::ShaderMode(
                    m_shaderCombo->itemData(index).toInt()));
            });
    connect(m_downscaleCombo, qOverload<int>(&QComboBox::activated), this,
            [this](int index) {
                m_video->setDownscale(m_downscaleCombo->itemData(index).toInt());
            });
    connect(m_fullscreenAction, &QAction::triggered, this, &MainWindow::toggleFullscreen);
    connect(m_fitAction, &QAction::triggered, this, &MainWindow::fitToPipeline);

    // Esc leaves fullscreen.
    auto *escShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    escShortcut->setContext(Qt::ApplicationShortcut);
    connect(escShortcut, &QShortcut::activated, this, [this] {
        if (isFullScreen())
            toggleFullscreen();
    });
}

void MainWindow::toggleFullscreen()
{
    if (isFullScreen())
        showNormal();
    else
        showFullScreen();
    QSignalBlocker blocker(m_fullscreenAction);
    m_fullscreenAction->setChecked(isFullScreen());
}

void MainWindow::fitToPipeline()
{
    // The pipeline works on the decoded frame, not the negotiated capture
    // size, so trust the last frame the decoder produced.
    const QSize frame(m_stats.frameW.load(), m_stats.frameH.load());
    if (frame.isEmpty() || frame.width() < 1 || frame.height() < 1)
        return;

    const int mode = m_shaderCombo->currentData().toInt();
    const int downscale = m_downscaleCombo->currentData().toInt();
    const QSize out = VideoWidget::pipelineOutputSize(frame, mode, downscale);
    if (out.isEmpty() || out.width() < 1 || out.height() < 1)
        return;

    if (isFullScreen() || isMaximized())
        showNormal();

    // The swapchain is sized in device pixels; convert back to widget (logical)
    // pixels using the current DPR so the surface is exactly `out` pixels.
    const qreal dpr = m_video->devicePixelRatio();
    const QSize widgetTarget(std::ceil(out.width() / dpr), std::ceil(out.height() / dpr));
    const QSize chrome = size() - m_video->size();
    resize(widgetTarget + chrome);
    m_video->requestUpdate();
}

void MainWindow::findDevices()
{
    m_devicePath = device::findShadowCastDevice();
    if (m_devicePath.isEmpty()) {
        showDeviceError(QStringLiteral("No ShadowCast capture dongle found (298f:1996)."));
        return;
    }

    for (auto format : {device::PixelFormat::Mjpg, device::PixelFormat::Yuyv}) {
        const QList<QSize> sizes = device::enumerateSizes(m_devicePath, format);
        if (sizes.isEmpty())
            continue;
        m_sizes.insert(int(format), sizes);
        m_formatCombo->addItem(format == device::PixelFormat::Mjpg
                                   ? QStringLiteral("MJPG") : QStringLiteral("YUYV"),
                               int(format));
    }
    if (m_sizes.isEmpty()) {
        showDeviceError(QStringLiteral("ShadowCast offers no MJPG or YUYV capture sizes."));
        m_devicePath.clear();
        return;
    }
    populateResolutions(QSize());

    m_audioCard = findShadowCastAudioCard();
}

void MainWindow::populateResolutions(const QSize &preferred)
{
    const auto format = device::PixelFormat(m_formatCombo->currentData().toInt());
    const QSignalBlocker blocker(m_resCombo);
    m_resCombo->clear();
    for (const QSize &s : m_sizes.value(int(format))) {
        const int fps = device::maxFpsForSize(m_devicePath, s, format);
        m_resCombo->addItem(
            QStringLiteral("%1x%2 @ %3 fps").arg(s.width()).arg(s.height()).arg(fps), s);
    }
    const int idx = m_resCombo->findData(preferred);
    if (idx >= 0)
        m_resCombo->setCurrentIndex(idx);
}

void MainWindow::loadSettings()
{
    QSettings settings;

    restoreGeometry(settings.value(QStringLiteral("geometry")).toByteArray());

    if (settings.contains(QStringLiteral("aspectPreserve")))
        m_aspectCheck->setChecked(settings.value(QStringLiteral("aspectPreserve")).toBool());

    if (settings.contains(QStringLiteral("volume")))
        m_volumeSlider->setValue(settings.value(QStringLiteral("volume")).toInt());

    if (settings.contains(QStringLiteral("shader"))) {
        const int shader = settings.value(QStringLiteral("shader")).toInt();
        const int comboIndex = m_shaderCombo->findData(shader);
        if (comboIndex >= 0)
            m_shaderCombo->setCurrentIndex(comboIndex);
    }
    m_video->setShaderMode(VideoWidget::ShaderMode(
        m_shaderCombo->currentData().toInt()));

    if (settings.contains(QStringLiteral("downscale"))) {
        const int downscale = settings.value(QStringLiteral("downscale")).toInt();
        const int comboIndex = m_downscaleCombo->findData(downscale);
        if (comboIndex >= 0)
            m_downscaleCombo->setCurrentIndex(comboIndex);
    }
    m_video->setDownscale(m_downscaleCombo->currentData().toInt());

    // The slider's setValue() above already applied volume via valueChanged;
    // also hand it to a not-yet-open sink so it takes effect on start.
    m_audioSink.setVolume(qreal(m_volumeSlider->value()) / 100.0);

    if (settings.contains(QStringLiteral("pixelFormat"))) {
        const int idx = m_formatCombo->findData(settings.value(QStringLiteral("pixelFormat")).toInt());
        if (idx >= 0)
            m_formatCombo->setCurrentIndex(idx);
    }
    const QSize saved(settings.value(QStringLiteral("videoWidth")).toInt(),
                      settings.value(QStringLiteral("videoHeight")).toInt());
    populateResolutions(saved);
    m_video->setPreserveAspect(m_aspectCheck->isChecked());
}

void MainWindow::saveSettings()
{
    QSettings settings;

    settings.setValue(QStringLiteral("geometry"), saveGeometry());
    settings.setValue(QStringLiteral("aspectPreserve"), m_aspectCheck->isChecked());
    settings.setValue(QStringLiteral("volume"), m_volumeSlider->value());
    settings.setValue(QStringLiteral("shader"), m_shaderCombo->currentData().toInt());
    settings.setValue(QStringLiteral("downscale"), m_downscaleCombo->currentData().toInt());

    const QSize res = m_resCombo->currentData().toSize();
    settings.setValue(QStringLiteral("pixelFormat"), m_formatCombo->currentData().toInt());
    if (res.isValid()) {
        settings.setValue(QStringLiteral("videoWidth"), res.width());
        settings.setValue(QStringLiteral("videoHeight"), res.height());
    }
    settings.sync();
}

void MainWindow::startPipeline()
{
    if (m_devicePath.isEmpty())
        return;

    const QSize target = m_resCombo->currentData().toSize();
    if (target.isEmpty())
        return;

    stopPipeline();
    m_ring.clear();

    const auto format = device::PixelFormat(m_formatCombo->currentData().toInt());
    m_capture = new Capture(m_devicePath, target, format, &m_stats);
    VideoWidget *video = m_video;
    m_decoder = new Decoder(&m_queue, &m_stats,
                            [video](const QImage &img) { video->submitFrame(img); });
    m_audio = new AudioCapture(&m_ring, &m_stats);

    QString audioErr;
    if (!m_audioSink.open(&m_ring, &audioErr))
        qWarning() << "Audio output:" << audioErr;

    const bool capOk = m_capture->start(&m_queue);
    m_decoder->start();

    if (!m_audio->start(m_audioCard))
        qWarning() << "Audio capture:" << m_audio->errorMessage();

    if (!capOk)
        qWarning() << "Capture failed to start for" << target;
}

void MainWindow::stopPipeline()
{
    if (m_capture) {
        m_capture->stop();
        m_capture->join();
        delete m_capture;
        m_capture = nullptr;
    }
    if (m_decoder) {
        m_decoder->stop();
        m_decoder->join();
        delete m_decoder;
        m_decoder = nullptr;
    }
    if (m_audio) {
        m_audio->stop();
        m_audio->join();
        delete m_audio;
        m_audio = nullptr;
    }
    m_audioSink.close();
}

void MainWindow::onFormatChanged(int index)
{
    if (index < 0)
        return;
    // Keep the current size if the new format offers it, else its largest.
    populateResolutions(m_resCombo->currentData().toSize());
    startPipeline();
}

void MainWindow::onResolutionChanged(int index)
{
    if (index < 0)
        return;
    startPipeline();
}

void MainWindow::updateStatus()
{
    if (m_devicePath.isEmpty()) {
        m_status->setText(QStringLiteral("No device"));
        return;
    }

    const uint64_t captured = m_stats.captured.load();
    const uint64_t decoded = m_stats.decoded.load();
    const uint64_t drops = m_queue.drops() + m_stats.droppedDecode.load();
    const double fpsCap = (captured - m_lastCaptured) / 0.5;
    const double fpsDec = (decoded - m_lastDecoded) / 0.5;
    m_lastCaptured = captured;
    m_lastDecoded = decoded;

    const int w = m_stats.frameW.load();
    const int h = m_stats.frameH.load();
    const uint64_t decodeNs = m_stats.decodeNs.load();
    const double decodeMs = decoded ? double(decodeNs) / 1e6 / decoded : 0.0;

    QString resText;
    if (m_capture) {
        const QSize neg = m_capture->negotiatedSize();
        if (neg.isValid())
            resText = QStringLiteral("%1x%2").arg(neg.width()).arg(neg.height());
    }

    m_status->setText(QStringLiteral("Device: %1  |  negotiated %2  |  frame %3x%4  |  "
                                     "audio: %5")
                          .arg(m_devicePath, resText)
                          .arg(w).arg(h)
                          .arg(m_audioSink.active() ? QStringLiteral("active")
                                                      : QStringLiteral("off")));

    m_statsLabel->setText(QStringLiteral("cap %1 fps  dec %2 fps  dropped %3  decode %4 ms  "
                                         "aud/underrun %5")
                              .arg(fpsCap, 0, 'f', 1)
                              .arg(fpsDec, 0, 'f', 1)
                              .arg(drops)
                              .arg(decodeMs, 0, 'f', 1)
                              .arg(m_ring.underruns()));
}

void MainWindow::showDeviceError(const QString &message)
{
    QMessageBox::warning(this, QStringLiteral("QShadow"), message);
    m_status->setText(message);
}