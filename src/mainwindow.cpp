// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#include "mainwindow.h"

#include "devicequirks.h"

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
#include <QAbstractItemView>
#include <QIcon>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVulkanInstance>
#include <QWidgetAction>
#include <QWidget>

#include <algorithm>
#include <cstdlib>

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
    setWindowTitle(QStringLiteral("QShadow"));

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *toolbar = new QToolBar(this);
    m_toolbar = toolbar;
    toolbar->setMovable(false);

    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);

    // Everything changed while watching stays on the toolbar; the settings that are
    // set once (aspect, downscale, audio source) live in the Options menu. No
    // text labels: the tooltips say what each control is.
    m_aspectAction = new QAction(QStringLiteral("Preserve aspect"), this);
    m_aspectAction->setCheckable(true);
    m_aspectAction->setChecked(true);

    const auto fixedWidth = [](QComboBox *combo, int chars) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(chars);
    };

    m_deviceCombo = new QComboBox(toolbar);
    fixedWidth(m_deviceCombo, 14);
    m_deviceCombo->setToolTip(QStringLiteral("Video capture device"));
    toolbar->addWidget(m_deviceCombo);

    m_sizeCombo = new QComboBox(toolbar);
    fixedWidth(m_sizeCombo, 18);
    m_sizeCombo->setToolTip(QStringLiteral(
        "Resolution (and the frame rate it will run at; change the rate in the "
        "Options menu)"));
    toolbar->addWidget(m_sizeCombo);

    m_formatCombo = new QComboBox(toolbar);
    fixedWidth(m_formatCombo, 5);
    m_formatCombo->setToolTip(QStringLiteral(
        "Pixel format. MJPG: compressed, high rates but with JPEG artifacts.\n"
        "Uncompressed formats (YUYV, NV12, ...): artifact-free, but limited by "
        "USB bandwidth to lower resolutions / frame rates."));
    m_formatAction = toolbar->addWidget(m_formatCombo);
    m_formatAction->setVisible(false); // only when the device offers several

    m_shaderCombo = new QComboBox(toolbar);
    fixedWidth(m_shaderCombo, 14);
    m_shaderCombo->setToolTip(QStringLiteral("Video shader"));
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
    m_shaderCombo->view()->setMinimumWidth(m_shaderCombo->view()->sizeHintForColumn(0) + 24);
    toolbar->addWidget(m_shaderCombo);

    // Models of the Options menu entries (see populateMenu); never shown.
    m_downscaleCombo = new QComboBox(this);
    m_downscaleCombo->addItem(QStringLiteral("Off"), 1);
    m_downscaleCombo->addItem(QStringLiteral("×2"), 2);
    m_downscaleCombo->addItem(QStringLiteral("×4"), 4);
    m_downscaleCombo->hide();
    m_audioCombo = new QComboBox(this);
    m_audioCombo->hide();
    m_rateCombo = new QComboBox(this);
    m_rateCombo->hide();

    toolbar->addSeparator();
    auto *volumeIcon = new QLabel(toolbar);
    const QIcon speaker = QIcon::fromTheme(QStringLiteral("audio-volume-high"));
    if (speaker.isNull())
        volumeIcon->setText(QStringLiteral("Vol"));
    else
        volumeIcon->setPixmap(speaker.pixmap(16, 16));
    volumeIcon->setToolTip(QStringLiteral("Output volume"));
    toolbar->addWidget(volumeIcon);
    m_volumeSlider = new QSlider(Qt::Horizontal, toolbar);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setValue(100);
    m_volumeSlider->setFixedWidth(80);
    m_volumeSlider->setToolTip(QStringLiteral("Output volume"));
    toolbar->addWidget(m_volumeSlider);

    toolbar->addSeparator();
    m_fitAction = new QAction(QIcon::fromTheme(QStringLiteral("zoom-fit-best")),
                              QStringLiteral("Fit"), toolbar);
    m_fitAction->setToolTip(QStringLiteral(
        "Resize the window so every pipeline-output pixel maps 1:1 to a "
        "screen pixel for the current shader and downscale"));
    toolbar->addAction(m_fitAction);
    m_fullscreenAction = new QAction(QIcon::fromTheme(QStringLiteral("view-fullscreen")),
                                     QStringLiteral("Fullscreen"), toolbar);
    m_fullscreenAction->setToolTip(QStringLiteral("Fullscreen (F11)"));
    m_fullscreenAction->setCheckable(true);
    m_fullscreenAction->setShortcut(QKeySequence(Qt::Key_F11));
    toolbar->addAction(m_fullscreenAction);
    m_fullscreenAction->setPriority(QAction::LowPriority);
    // Also owned by the window itself so F11 keeps working with the toolbar hidden.
    addAction(m_fullscreenAction);

    auto *options = new QToolButton(toolbar);
    options->setIcon(QIcon::fromTheme(QStringLiteral("preferences-system")));
    if (options->icon().isNull())
        options->setText(QStringLiteral("Options"));
    options->setToolTip(QStringLiteral("Options: aspect ratio, audio source, downscale, ..."));
    options->setPopupMode(QToolButton::InstantPopup);
    auto *optionsMenu = new QMenu(options);
    connect(optionsMenu, &QMenu::aboutToShow, this, [this, optionsMenu] {
        optionsMenu->clear();
        populateMenu(optionsMenu);
    });
    options->setMenu(optionsMenu);
    toolbar->addWidget(options);
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
    // The status text may be long (device names); it must never widen the window.
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_warnLabel = new QLabel(this);
    m_warnLabel->setStyleSheet(QStringLiteral("color: #c0392b; font-weight: bold;"));
    statusBar()->addWidget(m_status, 1);
    statusBar()->addPermanentWidget(m_warnLabel);
    statusBar()->addPermanentWidget(m_statsLabel);

    connect(m_aspectAction, &QAction::toggled, m_video, &VideoWidget::setPreserveAspect);
    connect(m_volumeSlider, &QSlider::valueChanged, this, [this](int v) {
        m_audioSink.setVolume(qreal(v) / 100.0);
    });
    connect(m_deviceCombo, &QComboBox::activated, this, &MainWindow::onDeviceChanged);
    connect(m_audioCombo, &QComboBox::activated, this, &MainWindow::onAudioChanged);
    connect(m_sizeCombo, &QComboBox::activated, this, &MainWindow::onSizeChanged);
    connect(m_formatCombo, &QComboBox::activated, this, &MainWindow::onFormatChanged);
    connect(m_rateCombo, &QComboBox::activated, this, &MainWindow::onRateChanged);
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

// The menu behind the toolbar's Options button and the right-click menu: every
// option. It is built from the controls' own models when opened, so everything
// always agrees and shares the same handlers.
void MainWindow::populateMenu(QMenu *menu)
{
    menu->addAction(m_aspectAction);

    // One radio-style submenu per combo box; choosing an entry acts exactly
    // like picking it in the combo.
    const auto comboMenu = [&](const QString &title, QComboBox *combo) {
        QMenu *sub = menu->addMenu(title);
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
    comboMenu(QStringLiteral("Device"), m_deviceCombo);
    comboMenu(QStringLiteral("Resolution"), m_sizeCombo);
    if (m_formatCombo->count() > 1)
        comboMenu(QStringLiteral("Format"), m_formatCombo);
    if (m_rateCombo->count() > 1)
        comboMenu(QStringLiteral("Frame rate"), m_rateCombo);
    comboMenu(QStringLiteral("Audio source"), m_audioCombo);
    comboMenu(QStringLiteral("Shader"), m_shaderCombo);
    comboMenu(QStringLiteral("Downscale"), m_downscaleCombo);

    QMenu *volume = menu->addMenu(QStringLiteral("Volume"));
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

    menu->addSeparator();
    QAction *fit = menu->addAction(QStringLiteral("Fit window to video"));
    fit->setEnabled(!isFullScreen());
    connect(fit, &QAction::triggered, this, &MainWindow::fitToPipeline);
    QAction *full = menu->addAction(QStringLiteral("Fullscreen\tF11"));
    full->setCheckable(true);
    full->setChecked(isFullScreen());
    connect(full, &QAction::triggered, this, &MainWindow::toggleFullscreen);
}

void MainWindow::showContextMenu(const QPoint &globalPos)
{
    QMenu menu(this);
    populateMenu(&menu);
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
    const QList<platform::DeviceInfo> devices = platform::listDevices();

    // Refill the device list only when it changed, so an open drop-down is not
    // yanked away by the periodic retry.
    auto sameDevices = [&] {
        if (devices.size() != m_devices.size())
            return false;
        for (qsizetype i = 0; i < devices.size(); ++i) {
            if (devices[i].key() != m_devices[i].key() || devices[i].videoId != m_devices[i].videoId
                || devices[i].audioId != m_devices[i].audioId)
                return false;
        }
        return true;
    };
    if (!sameDevices() || m_deviceCombo->count() != devices.size()) {
        m_devices = devices;
        const QSignalBlocker blocker(m_deviceCombo);
        m_deviceCombo->clear();
        for (const platform::DeviceInfo &d : std::as_const(m_devices)) {
            m_deviceCombo->addItem(d.name, d.key());
            m_deviceCombo->setItemData(m_deviceCombo->count() - 1, d.name, Qt::ToolTipRole);
        }
        // Compact box, full names in the list.
        m_deviceCombo->view()->setMinimumWidth(m_deviceCombo->view()->sizeHintForColumn(0) + 24);
    }

    auto fail = [&](const QString &why) {
        m_devicePath.clear();
        m_pairedAudioId.clear();
        m_audioId.clear();
        m_sizes.clear();
        m_idleStatus = why;
        updateAudioSources(); // the audio list does not depend on a video device
        return false;
    };
    if (m_devices.isEmpty())
        return fail(QStringLiteral("Waiting for a video capture device…"));

    // The user's device if present, else the first one (without forgetting the
    // choice, so the preferred device is picked again once it comes back).
    qsizetype pick = 0;
    for (qsizetype i = 0; i < m_devices.size(); ++i) {
        if (m_devices[i].key() == m_prefDevice) {
            pick = i;
            break;
        }
    }
    {
        const QSignalBlocker blocker(m_deviceCombo);
        m_deviceCombo->setCurrentIndex(int(pick));
    }
    const platform::DeviceInfo &dev = m_devices[pick];
    const QString path = dev.videoId;

    QHash<int, QList<device::Mode>> sizes;
    for (auto format : device::kAllFormats) {
        const QList<device::Mode> list = platform::enumerateModes(path, format);
        if (!list.isEmpty())
            sizes.insert(int(format), list);
    }
    if (sizes.isEmpty())
        return fail(QStringLiteral("%1: no supported video format").arg(dev.name));

    // Only touch the toolbar when something changed, so an open drop-down is
    // not yanked away by the periodic retry.
    if (path != m_devicePath || sizes != m_sizes || m_sizeCombo->count() == 0) {
        m_devicePath = path;
        m_sizes = sizes;
        populateModes();
    }
    m_pairedAudioId = dev.audioId;
    updateAudioSources();
    m_vid = dev.vid;
    m_pid = dev.pid;
    return true;
}

void MainWindow::updateAudioSources()
{
    const QList<platform::AudioInputInfo> inputs = platform::listAudioInputs();
    bool same = inputs.size() == m_audioInputs.size() && m_audioCombo->count() == inputs.size() + 2;
    for (qsizetype i = 0; same && i < inputs.size(); ++i)
        same = inputs[i].name == m_audioInputs[i].name && inputs[i].id == m_audioInputs[i].id;
    if (!same) {
        m_audioInputs = inputs;
        const QSignalBlocker blocker(m_audioCombo);
        m_audioCombo->clear();
        m_audioCombo->addItem(QStringLiteral("Device audio"), QStringLiteral("auto"));
        m_audioCombo->addItem(QStringLiteral("None"), QStringLiteral("none"));
        for (const platform::AudioInputInfo &in : std::as_const(m_audioInputs))
            m_audioCombo->addItem(in.name, QStringLiteral("in:") + in.name);
    }
    // A saved input that is gone falls back to the device's audio without
    // forgetting the choice.
    QString effective = m_prefAudio;
    int idx = m_audioCombo->findData(effective);
    if (idx < 0) {
        effective = QStringLiteral("auto");
        idx = 0;
    }
    {
        const QSignalBlocker blocker(m_audioCombo);
        m_audioCombo->setCurrentIndex(idx);
    }
    if (effective == QLatin1String("none")) {
        m_audioId.clear();
    } else if (effective == QLatin1String("auto")) {
        m_audioId = m_pairedAudioId;
    } else {
        m_audioId.clear();
        for (const platform::AudioInputInfo &in : std::as_const(m_audioInputs)) {
            if (QLatin1String("in:") + in.name == effective)
                m_audioId = in.id;
        }
    }
}

namespace {

// The rate a mode will run at: the one nearest to the preferred rate (0: the
// highest, which is listed first).
int pickRate(const QList<int> &rates, int preferred)
{
    if (rates.isEmpty())
        return 0;
    int best = rates.first();
    if (preferred > 0) {
        for (int r : rates) {
            if (std::abs(r - preferred) < std::abs(best - preferred))
                best = r;
        }
    }
    return best;
}

QList<int> ratesOf(const device::Mode &mode)
{
    return mode.rates.isEmpty() && mode.fps > 0 ? QList<int>{mode.fps} : mode.rates;
}

} // namespace

void MainWindow::populateModes()
{
    {
        const QSignalBlocker blocker(m_formatCombo);
        m_formatCombo->clear();
        for (auto format : device::kAllFormats) {
            if (m_sizes.contains(int(format)))
                m_formatCombo->addItem(QLatin1String(device::formatName(format)), int(format));
        }
        const int idx = m_formatCombo->findData(m_prefFormat);
        if (idx >= 0)
            m_formatCombo->setCurrentIndex(idx);
    }
    // A format box is only worth the toolbar space when there is a choice.
    m_formatAction->setVisible(m_formatCombo->count() > 1);
    populateSizes();
}

void MainWindow::populateSizes()
{
    const auto format = device::PixelFormat(m_formatCombo->currentData().toInt());
    {
        const QSignalBlocker blocker(m_sizeCombo);
        m_sizeCombo->clear();
        for (const device::Mode &mode : m_sizes.value(int(format))) {
            QString text = QStringLiteral("%1×%2").arg(mode.size.width()).arg(mode.size.height());
            // The rate this size will run at if picked.
            if (const int rate = pickRate(ratesOf(mode), m_prefFps))
                text += QStringLiteral(" · %1 fps").arg(rate);
            m_sizeCombo->addItem(text, mode.size);
        }
        const int idx = m_sizeCombo->findData(m_prefSize);
        if (idx >= 0)
            m_sizeCombo->setCurrentIndex(idx);
        // The list shows the full labels even though the box is compact.
        m_sizeCombo->view()->setMinimumWidth(m_sizeCombo->view()->sizeHintForColumn(0) + 24);
    }
    populateRates();
}

void MainWindow::populateRates()
{
    const auto format = device::PixelFormat(m_formatCombo->currentData().toInt());
    const QSize size = m_sizeCombo->currentData().toSize();
    QList<int> rates;
    for (const device::Mode &mode : m_sizes.value(int(format))) {
        if (mode.size == size) {
            rates = ratesOf(mode);
            break;
        }
    }
    const QSignalBlocker blocker(m_rateCombo);
    m_rateCombo->clear();
    for (int r : std::as_const(rates))
        m_rateCombo->addItem(QStringLiteral("%1 fps").arg(r), r);
    if (rates.isEmpty())
        m_rateCombo->addItem(QStringLiteral("Auto"), 0);
    else
        m_rateCombo->setCurrentIndex(int(rates.indexOf(pickRate(rates, m_prefFps))));
}

MainWindow::CurrentMode MainWindow::currentMode() const
{
    CurrentMode mode;
    if (m_formatCombo->count() == 0 || m_sizeCombo->count() == 0)
        return mode;
    mode.format = device::PixelFormat(m_formatCombo->currentData().toInt());
    mode.size = m_sizeCombo->currentData().toSize();
    mode.fps = m_rateCombo->currentData().toInt();
    return mode;
}

void MainWindow::loadSettings()
{
    QSettings settings;

    restoreGeometry(settings.value(QStringLiteral("geometry")).toByteArray());

    if (settings.contains(QStringLiteral("aspectPreserve")))
        m_aspectAction->setChecked(settings.value(QStringLiteral("aspectPreserve")).toBool());

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

    m_prefDevice = settings.value(QStringLiteral("deviceKey")).toString();
    m_prefAudio = settings.value(QStringLiteral("audioSource"), QStringLiteral("auto")).toString();
    m_prefFormat = settings.value(QStringLiteral("pixelFormat"),
                                  int(device::PixelFormat::Mjpg)).toInt();
    m_prefFps = settings.value(QStringLiteral("videoFps"), 0).toInt();
    m_prefSize = QSize(settings.value(QStringLiteral("videoWidth")).toInt(),
                       settings.value(QStringLiteral("videoHeight")).toInt());
    m_video->setPreserveAspect(m_aspectAction->isChecked());
}

void MainWindow::saveSettings()
{
    QSettings settings;

    settings.setValue(QStringLiteral("geometry"), saveGeometry());
    settings.setValue(QStringLiteral("aspectPreserve"), m_aspectAction->isChecked());
    settings.setValue(QStringLiteral("volume"), m_volumeSlider->value());
    settings.setValue(QStringLiteral("shader"), m_shaderCombo->currentData().toInt());
    settings.setValue(QStringLiteral("downscale"), m_downscaleCombo->currentData().toInt());

    settings.setValue(QStringLiteral("deviceKey"), m_prefDevice);
    settings.setValue(QStringLiteral("audioSource"), m_prefAudio);
    settings.setValue(QStringLiteral("pixelFormat"), m_prefFormat);
    settings.setValue(QStringLiteral("videoFps"), m_prefFps);
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

    const CurrentMode mode = currentMode();
    const QSize target = mode.size;
    if (target.isEmpty())
        return false;

    m_queue.clear();
    const device::PixelFormat format = mode.format;
    m_capture = platform::createCapture(m_devicePath, target, format, mode.fps, &m_stats);
    VideoWidget *video = m_video;
    m_decoder = new Decoder(&m_queue, &m_stats,
                            [video](const VideoFramePtr &frame, std::chrono::steady_clock::time_point stamp) {
                                video->submitFrame(frame, stamp);
                            });
    m_decoder->setGpuYuvCheck([video] { return video->gpuYuvSupported(); });
    m_decoder->setColorOverride(quirks::colorOverride(m_vid, m_pid, format));

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
    if (m_audioId.isEmpty())
        return; // no audio interface paired with this device: video only
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

void MainWindow::onDeviceChanged(int index)
{
    if (index < 0 || index >= m_devices.size())
        return;
    m_prefDevice = m_devices[index].key();
    // A different device: its formats and sizes replace the lists wholesale.
    stopPipeline();
    m_devicePath.clear();
    if (connectDevice())
        startPipeline();
}

void MainWindow::onAudioChanged(int index)
{
    if (index < 0)
        return;
    m_prefAudio = m_audioCombo->itemData(index).toString();
    updateAudioSources();
    // Only the audio restarts; the video keeps running.
    stopAudio();
    if (m_capture && m_capture->isRunning())
        startAudio();
}

void MainWindow::onFormatChanged(int index)
{
    if (index < 0)
        return;
    m_prefFormat = m_formatCombo->currentData().toInt();
    // Keep the current size if the new format offers it, else its largest.
    m_prefSize = m_sizeCombo->currentData().toSize();
    populateSizes();
    m_prefSize = m_sizeCombo->currentData().toSize();
    m_prefFps = m_rateCombo->currentData().toInt();
    restartVideo();
}

void MainWindow::onSizeChanged(int index)
{
    if (index < 0)
        return;
    m_prefSize = m_sizeCombo->currentData().toSize();
    populateRates(); // keeps the rate if the new size offers it
    m_prefFps = m_rateCombo->currentData().toInt();
    restartVideo();
}

void MainWindow::onRateChanged(int index)
{
    if (index < 0)
        return;
    m_prefFps = m_rateCombo->currentData().toInt();
    // The size list shows the rate each size will run at.
    m_prefSize = m_sizeCombo->currentData().toSize();
    populateSizes();
    restartVideo();
}

void MainWindow::restartVideo()
{
    // Only the video needs to restart; the audio keeps playing.
    stopVideo();
    startVideo();
}

void MainWindow::updateStatus()
{
    if (m_devicePath.isEmpty()) {
        m_status->setText(m_idleStatus);
        m_statsLabel->clear();
        m_warnLabel->clear();
        return;
    }
    if (!m_capture || !m_capture->isRunning()) {
        m_status->setText(QStringLiteral("Capture stopped — reconnecting…"));
        m_statsLabel->clear();
        m_warnLabel->clear();
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

    const uint64_t underruns = m_ring.underruns();
    const uint64_t xruns = m_audio ? m_audio->captureXruns() : 0;
    const uint64_t newDrops = drops - std::min(drops, m_lastDrops);
    const uint64_t newUnderruns = underruns - std::min(underruns, m_lastUnderruns);
    const uint64_t newXruns = xruns - std::min(xruns, m_lastXruns);
    m_lastDrops = drops;
    m_lastUnderruns = underruns;
    m_lastXruns = xruns;

    const int w = m_stats.frameW.load();
    const int h = m_stats.frameH.load();

    QString resText;
    const QSize neg = m_capture->negotiatedSize();
    if (neg.isValid())
        resText = QStringLiteral("%1x%2").arg(neg.width()).arg(neg.height());

    const bool audioActive = m_audioSink.active();
    const double latencyMs = m_video->latencyUs() / 1000.0;
    const double audioMs = double(m_ring.depthBytes()) / double(AudioRing::kBytesPerMs);
    const int driftPpm = m_audio ? m_audio->driftPpm() : 0;

    // Left: what is being shown. Right: how it is doing. Everything else is in
    // the tooltip, and trouble shows up as a red warning only while it happens.
    const CurrentMode mode = currentMode();
    m_status->setText(QStringLiteral("%1  ·  %2x%3 %4  ·  audio %5")
                          .arg(m_deviceCombo->currentText())
                          .arg(w).arg(h)
                          .arg(QLatin1String(device::formatName(mode.format)),
                               audioActive ? QStringLiteral("on") : QStringLiteral("off")));
    m_status->setToolTip(QStringLiteral("%1\nnegotiated %2, frame %3x%4%5")
                             .arg(m_devicePath, resText).arg(w).arg(h)
                             .arg(m_stats.frameYuv.load() ? QStringLiteral(", GPU YUV conversion")
                                                          : QString()));

    QString right = QStringLiteral("%1 fps  ·  dec %2 ms  ·  lat %3 ms")
                        .arg(fpsCap, 0, 'f', 0)
                        .arg(decodeMs, 0, 'f', 1)
                        .arg(latencyMs, 0, 'f', 0);
    if (audioActive)
        right += QStringLiteral("  ·  audio %1 ms").arg(audioMs, 0, 'f', 0);
    m_statsLabel->setText(right);
    m_statsLabel->setToolTip(
        QStringLiteral("capture %1 fps, decode %2 fps, %3 frames dropped in total\n"
                       "decode %4 ms, capture-to-render latency %5 ms\n"
                       "audio buffer %6 ms, drift correction %7 ppm\n"
                       "audio underruns %8, capture xruns %9\n\n"
                       "latency: from the first USB packet of a frame to the frame being "
                       "submitted for display (includes the USB transfer time of the frame). "
                       "It excludes the device's own capture/encode delay and the display's "
                       "scanout.")
            .arg(fpsCap, 0, 'f', 1).arg(fpsDec, 0, 'f', 1).arg(drops)
            .arg(decodeMs, 0, 'f', 1).arg(latencyMs, 0, 'f', 1)
            .arg(audioMs, 0, 'f', 0).arg(driftPpm)
            .arg(underruns).arg(xruns));

    QStringList warnings;
    if (noFrames)
        warnings << QStringLiteral("NO FRAMES");
    if (newDrops)
        warnings << QStringLiteral("dropped %1").arg(newDrops);
    if (newUnderruns)
        warnings << QStringLiteral("audio underrun");
    if (newXruns)
        warnings << QStringLiteral("audio overrun");
    m_warnLabel->setText(warnings.isEmpty() ? QString()
                                            : QStringLiteral("⚠ ") + warnings.join(QStringLiteral(", ")));

    // The scripted-check output keeps its complete, stable format.
    static const bool printStats = !qEnvironmentVariableIsEmpty("QSHADOW_STATS");
    if (printStats) {
        const QString legacyStatus =
            QStringLiteral("Device: %1  |  negotiated %2  |  frame %3x%4  |  audio: %5%6%7")
                .arg(m_devicePath, resText)
                .arg(w).arg(h)
                .arg(audioActive ? QStringLiteral("active") : QStringLiteral("off"))
                .arg(noFrames ? QStringLiteral("  |  NO FRAMES RECEIVED") : QString())
                .arg(m_stats.frameYuv.load() ? QStringLiteral("  |  GPU YUV") : QString());
        const QString legacyStats =
            QStringLiteral("cap %1 fps  dec %2 fps  dropped %3  decode %4 ms  "
                           "latency %5 ms  audio buf %6 ms (%7 ppm)  underruns %8  capture xruns %9")
                .arg(fpsCap, 0, 'f', 1).arg(fpsDec, 0, 'f', 1).arg(drops)
                .arg(decodeMs, 0, 'f', 1).arg(latencyMs, 0, 'f', 1)
                .arg(audioMs, 0, 'f', 0).arg(driftPpm)
                .arg(underruns).arg(xruns);
        qInfo().noquote() << legacyStatus << "||" << legacyStats;
    }
}
