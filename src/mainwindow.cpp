// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#include "mainwindow.h"

#include "decoder.h"
#include "device.h"
#include "platform/platform.h"
#include "videowidget.h"

#include <QActionGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>
#include <QVulkanInstance>
#include <QWidgetAction>
#include <QWidget>

#include <chrono>
#include <cmath>

MainWindow::MainWindow(QVulkanInstance *vulkan, QWidget *parent)
    : QMainWindow(parent)
    , m_vulkanInstance(vulkan)
{
    buildUi();
    loadSettings();
    connectDevice();
    startPipeline();

    m_statusTimer.setInterval(500);
    connect(&m_statusTimer, &QTimer::timeout, this, &MainWindow::updateStatus);
    m_statusTimer.start();

    // Hotplug: notice an unplugged/dead capture, or a dongle that appears later,
    // and (re)start the pipeline.
    m_deviceTimer.setInterval(1000);
    connect(&m_deviceTimer, &QTimer::timeout, this, &MainWindow::checkDevice);
    m_deviceTimer.start();
}

MainWindow::~MainWindow()
{
    m_statusTimer.stop();
    m_deviceTimer.stop();
    stopPipeline();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // No hotplug retry (which would start a new pipeline and audio sink) while
    // the application is shutting down.
    m_statusTimer.stop();
    m_deviceTimer.stop();
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
    m_toolbar = toolbar;
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
    // Also owned by the window itself so F11 keeps working with the toolbar hidden.
    addAction(m_fullscreenAction);
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
    m_statsLabel->setToolTip(QStringLiteral(
        "latency: from the first USB packet of a frame to the frame being "
        "submitted for display (includes the USB transfer time of the frame).\n"
        "It excludes the dongle's own capture/encode delay and the display's "
        "scanout."));
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
    connect(m_video, &VideoWidget::contextMenuRequested, this, &MainWindow::showContextMenu);

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
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() != QEvent::WindowStateChange || !m_toolbar)
        return;
    // Real fullscreen: nothing but the video.
    const bool full = isFullScreen();
    m_toolbar->setVisible(!full);
    statusBar()->setVisible(!full);

    // Never let the video cover the whole window. On Wayland (seen with GNOME's
    // compositor) a video surface that covers every pixel of the fullscreen
    // window makes the window count as fully occluded: the embedded
    // QVulkanWindow loses its exposure, Qt stops rendering and the picture
    // freezes. Resizing the video is fine, only total coverage is not, so keep
    // one pixel row of the window's own (black) background uncovered.
    QWidget *central = centralWidget();
    central->layout()->setContentsMargins(0, 0, 0, full ? 1 : 0);
    if (full) {
        QPalette black;
        black.setColor(QPalette::Window, Qt::black);
        central->setPalette(black);
    } else {
        central->setPalette(QPalette());
    }
    central->setAutoFillBackground(full);
    QSignalBlocker blocker(m_fullscreenAction);
    m_fullscreenAction->setChecked(full);
}

// Right-click menu with every toolbar option. It is built from the toolbar's
// own controls when opened, so both always agree and share the same handlers.
void MainWindow::showContextMenu(const QPoint &globalPos)
{
    QMenu menu(this);

    auto *aspect = menu.addAction(QStringLiteral("Preserve aspect"));
    aspect->setCheckable(true);
    aspect->setChecked(m_aspectCheck->isChecked());
    connect(aspect, &QAction::triggered, m_aspectCheck, &QCheckBox::setChecked);

    // One radio-style submenu per combo box; choosing an entry acts exactly
    // like picking it in the toolbar.
    const auto comboMenu = [&](const QString &title, QComboBox *combo) {
        QMenu *sub = menu.addMenu(title);
        sub->setEnabled(combo->count() > 0);
        auto *group = new QActionGroup(sub);
        for (int i = 0; i < combo->count(); ++i) {
            QAction *a = sub->addAction(combo->itemText(i));
            a->setCheckable(true);
            a->setChecked(i == combo->currentIndex());
            group->addAction(a);
            connect(a, &QAction::triggered, this, [combo, i] {
                combo->setCurrentIndex(i);
                emit combo->activated(i);
            });
        }
    };
    comboMenu(QStringLiteral("Format"), m_formatCombo);
    comboMenu(QStringLiteral("Resolution"), m_resCombo);
    comboMenu(QStringLiteral("Shader"), m_shaderCombo);
    comboMenu(QStringLiteral("Downscale"), m_downscaleCombo);

    QMenu *volume = menu.addMenu(QStringLiteral("Volume"));
    auto *holder = new QWidget(volume);
    auto *row = new QHBoxLayout(holder);
    auto *slider = new QSlider(Qt::Horizontal, holder);
    slider->setRange(m_volumeSlider->minimum(), m_volumeSlider->maximum());
    slider->setValue(m_volumeSlider->value());
    slider->setFixedWidth(160);
    auto *value = new QLabel(QString::number(slider->value()), holder);
    value->setMinimumWidth(28);
    row->addWidget(slider);
    row->addWidget(value);
    connect(slider, &QSlider::valueChanged, m_volumeSlider, &QSlider::setValue);
    connect(slider, &QSlider::valueChanged, value,
            [value](int v) { value->setText(QString::number(v)); });
    auto *sliderAction = new QWidgetAction(volume);
    sliderAction->setDefaultWidget(holder);
    volume->addAction(sliderAction);

    menu.addSeparator();
    QAction *fit = menu.addAction(QStringLiteral("Fit window to video"));
    fit->setEnabled(!isFullScreen());
    connect(fit, &QAction::triggered, this, &MainWindow::fitToPipeline);
    QAction *full = menu.addAction(QStringLiteral("Fullscreen	F11"));
    full->setCheckable(true);
    full->setChecked(isFullScreen());
    connect(full, &QAction::triggered, this, &MainWindow::toggleFullscreen);

    menu.exec(globalPos);
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

bool MainWindow::connectDevice()
{
    const platform::DeviceIds ids = platform::findDevice();
    const QString path = ids.video;
    if (path.isEmpty()) {
        m_devicePath.clear();
        m_sizes.clear();
        return false;
    }

    QHash<int, QList<device::Mode>> sizes;
    for (auto format : {device::PixelFormat::Mjpg, device::PixelFormat::Yuyv}) {
        const QList<device::Mode> list = platform::enumerateModes(path, format);
        if (!list.isEmpty())
            sizes.insert(int(format), list);
    }
    if (sizes.isEmpty()) {
        m_devicePath.clear();
        m_sizes.clear();
        return false;
    }

    // Only touch the toolbar when something changed, so an open drop-down is
    // not yanked away by the periodic retry.
    if (path != m_devicePath || sizes != m_sizes || m_formatCombo->count() == 0) {
        m_devicePath = path;
        m_sizes = sizes;
        {
            const QSignalBlocker blocker(m_formatCombo);
            m_formatCombo->clear();
            for (auto format : {device::PixelFormat::Mjpg, device::PixelFormat::Yuyv}) {
                if (m_sizes.contains(int(format)))
                    m_formatCombo->addItem(format == device::PixelFormat::Mjpg
                                               ? QStringLiteral("MJPG")
                                               : QStringLiteral("YUYV"),
                                           int(format));
            }
            const int idx = m_formatCombo->findData(m_prefFormat);
            if (idx >= 0)
                m_formatCombo->setCurrentIndex(idx);
        }
        populateResolutions(m_prefSize);
    }
    m_audioId = ids.audio;
    return true;
}

void MainWindow::populateResolutions(const QSize &preferred)
{
    const auto format = device::PixelFormat(m_formatCombo->currentData().toInt());
    const QSignalBlocker blocker(m_resCombo);
    m_resCombo->clear();
    for (const device::Mode &mode : m_sizes.value(int(format))) {
        m_resCombo->addItem(QStringLiteral("%1x%2 @ %3 fps")
                                .arg(mode.size.width()).arg(mode.size.height()).arg(mode.fps),
                            mode.size);
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

    m_prefFormat = settings.value(QStringLiteral("pixelFormat"),
                                  int(device::PixelFormat::Mjpg)).toInt();
    m_prefSize = QSize(settings.value(QStringLiteral("videoWidth")).toInt(),
                       settings.value(QStringLiteral("videoHeight")).toInt());
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

    settings.setValue(QStringLiteral("pixelFormat"), m_prefFormat);
    if (m_prefSize.isValid()) {
        settings.setValue(QStringLiteral("videoWidth"), m_prefSize.width());
        settings.setValue(QStringLiteral("videoHeight"), m_prefSize.height());
    }
    settings.sync();
}

void MainWindow::startPipeline()
{
    stopPipeline();
    // Audio only follows a successfully started capture: when the device is busy
    // or just vanished, retrying every second must not churn the audio sink.
    if (startVideo())
        startAudio();
}

void MainWindow::stopPipeline()
{
    stopVideo();
    stopAudio();
}

bool MainWindow::startVideo()
{
    if (m_devicePath.isEmpty())
        return false;

    const QSize target = m_resCombo->currentData().toSize();
    if (target.isEmpty())
        return false;

    m_queue.clear();
    const auto format = device::PixelFormat(m_formatCombo->currentData().toInt());
    m_capture = platform::createCapture(m_devicePath, target, format, &m_stats);
    VideoWidget *video = m_video;
    m_decoder = new Decoder(&m_queue, &m_stats,
                            [video](const VideoFramePtr &frame, std::chrono::steady_clock::time_point stamp) {
                                video->submitFrame(frame, stamp);
                            });
    m_decoder->setGpuYuvCheck([video] { return video->gpuYuvSupported(); });

    m_capture->setQuiet(m_captureFailing);
    const bool capOk = m_capture->start(&m_queue);
    m_decoder->start();
    if (!capOk && !m_captureFailing)
        qWarning() << "Capture failed to start for" << target << "- retrying every second";
    m_captureFailing = !capOk;
    return capOk;
}

void MainWindow::stopVideo()
{
    if (m_capture) {
        m_capture->stop();
        m_capture->join();
        m_capture.reset();
    }
    if (m_decoder) {
        m_decoder->stop();
        m_decoder->join();
        delete m_decoder;
        m_decoder = nullptr;
    }
}

void MainWindow::startAudio()
{
    m_ring.clear();
    m_audio = platform::createAudioInput(m_audioId, &m_ring, &m_stats);

    QString audioErr;
    if (!m_audioSink.open(&m_ring, &audioErr))
        qWarning() << "Audio output:" << audioErr;
    if (!m_audio->start())
        qWarning() << "Audio capture:" << m_audio->errorMessage();
}

void MainWindow::stopAudio()
{
    if (m_audio) {
        m_audio->stop();
        m_audio->join();
        m_audio.reset();
    }
    m_audioSink.close();
}

void MainWindow::checkDevice()
{
    if (m_capture && m_capture->isRunning())
        return;

    // Never started, failed, or the capture thread died (unplug / stream
    // error): rebuild everything, since the audio card may have gone too.
    stopPipeline();
    if (connectDevice())
        startPipeline();
}

void MainWindow::onFormatChanged(int index)
{
    if (index < 0)
        return;
    m_prefFormat = m_formatCombo->currentData().toInt();
    // Keep the current size if the new format offers it, else its largest.
    populateResolutions(m_resCombo->currentData().toSize());
    m_prefSize = m_resCombo->currentData().toSize();
    stopVideo();
    startVideo();
}

void MainWindow::onResolutionChanged(int index)
{
    if (index < 0)
        return;
    m_prefSize = m_resCombo->currentData().toSize();
    // Only the video needs to restart; the audio keeps playing.
    stopVideo();
    startVideo();
}

void MainWindow::updateStatus()
{
    if (m_devicePath.isEmpty()) {
        m_status->setText(QStringLiteral("Waiting for the ShadowCast dongle (298f:1996)…"));
        m_statsLabel->clear();
        return;
    }
    if (!m_capture || !m_capture->isRunning()) {
        m_status->setText(QStringLiteral("Capture stopped — reconnecting…"));
        m_statsLabel->clear();
        return;
    }

    // Rates and decode time are over the last status interval, so a spike or a
    // recovery shows up immediately instead of being averaged away.
    const uint64_t captured = m_stats.captured.load();
    const uint64_t decoded = m_stats.decoded.load();
    const uint64_t decodeNs = m_stats.decodeNs.load();
    const uint64_t drops = m_queue.drops() + m_stats.droppedDecode.load();
    const double interval = m_statusTimer.interval() / 1000.0;
    const double fpsCap = double(captured - m_lastCaptured) / interval;
    const double fpsDec = double(decoded - m_lastDecoded) / interval;
    const uint64_t decodedNow = decoded - m_lastDecoded;
    const double decodeMs =
        decodedNow ? double(decodeNs - m_lastDecodeNs) / 1e6 / double(decodedNow) : 0.0;
    const bool noFrames = captured == m_lastCaptured;
    m_lastCaptured = captured;
    m_lastDecoded = decoded;
    m_lastDecodeNs = decodeNs;

    const int w = m_stats.frameW.load();
    const int h = m_stats.frameH.load();

    QString resText;
    const QSize neg = m_capture->negotiatedSize();
    if (neg.isValid())
        resText = QStringLiteral("%1x%2").arg(neg.width()).arg(neg.height());

    m_status->setText(QStringLiteral("Device: %1  |  negotiated %2  |  frame %3x%4  |  "
                                     "audio: %5%6%7")
                          .arg(m_devicePath, resText)
                          .arg(w).arg(h)
                          .arg(m_audioSink.active() ? QStringLiteral("active")
                                                      : QStringLiteral("off"))
                          .arg(noFrames ? QStringLiteral("  |  NO FRAMES RECEIVED")
                                        : QString())
                          .arg(m_stats.frameYuv.load() ? QStringLiteral("  |  GPU YUV")
                                                       : QString()));

    m_statsLabel->setText(QStringLiteral("cap %1 fps  dec %2 fps  dropped %3  decode %4 ms  "
                                         "latency %5 ms  audio buf %6 ms (%7 ppm)  underruns %8  capture xruns %9")
                              .arg(fpsCap, 0, 'f', 1)
                              .arg(fpsDec, 0, 'f', 1)
                              .arg(drops)
                              .arg(decodeMs, 0, 'f', 1)
                              .arg(m_video->latencyUs() / 1000.0, 0, 'f', 1)
                              .arg(double(m_ring.depthBytes()) / double(AudioRing::kBytesPerMs),
                                   0, 'f', 0)
                              .arg(m_audio ? m_audio->driftPpm() : 0)
                              .arg(m_ring.underruns())
                              .arg(m_audio ? m_audio->captureXruns() : 0));

    static const bool printStats = !qEnvironmentVariableIsEmpty("QSHADOW_STATS");
    if (printStats)
        qInfo().noquote() << m_status->text() << "||" << m_statsLabel->text();
}
