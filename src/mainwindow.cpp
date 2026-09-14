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
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidget>

#include <chrono>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
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
    toolbar->addWidget(new QLabel(QStringLiteral(" Resolution: "), toolbar));
    m_resCombo = new QComboBox(toolbar);
    toolbar->addWidget(m_resCombo);

    toolbar->addSeparator();
    m_fullscreenAction = new QAction(QStringLiteral("Fullscreen"), toolbar);
    m_fullscreenAction->setCheckable(true);
    m_fullscreenAction->setShortcut(QKeySequence(Qt::Key_F11));
    toolbar->addAction(m_fullscreenAction);
    m_fullscreenAction->setPriority(QAction::LowPriority);
    layout->addWidget(toolbar);

    m_video = new VideoWidget(central);
    layout->addWidget(m_video, 1);
    setCentralWidget(central);

    m_status = new QLabel(this);
    m_statsLabel = new QLabel(this);
    statusBar()->addWidget(m_status, 1);
    statusBar()->addPermanentWidget(m_statsLabel);

    connect(m_aspectCheck, &QCheckBox::toggled, m_video, &VideoWidget::setPreserveAspect);
    connect(m_resCombo, &QComboBox::activated, this, &MainWindow::onResolutionChanged);
    connect(m_fullscreenAction, &QAction::triggered, this, &MainWindow::toggleFullscreen);

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

void MainWindow::findDevices()
{
    m_devicePath = device::findShadowCastDevice();
    if (m_devicePath.isEmpty()) {
        showDeviceError(QStringLiteral("No ShadowCast capture dongle found (298f:1996)."));
        return;
    }

    m_sizes = device::enumerateMjpgSizes(m_devicePath);
    if (m_sizes.isEmpty()) {
        showDeviceError(QStringLiteral("ShadowCast offers no MJPG capture sizes."));
        m_devicePath.clear();
        return;
    }

    for (const QSize &s : m_sizes) {
        const int fps = device::maxFpsForSize(m_devicePath, s);
        m_resCombo->addItem(QStringLiteral("%1x%2 @ %3 fps").arg(s.width()).arg(s.height()).arg(fps),
                            s);
    }

    m_audioCard = findShadowCastAudioCard();
}

void MainWindow::loadSettings()
{
    QSettings settings;

    restoreGeometry(settings.value(QStringLiteral("geometry")).toByteArray());

    if (settings.contains(QStringLiteral("aspectPreserve")))
        m_aspectCheck->setChecked(settings.value(QStringLiteral("aspectPreserve")).toBool());

    const QSize saved(settings.value(QStringLiteral("videoWidth")).toInt(),
                      settings.value(QStringLiteral("videoHeight")).toInt());
    if (saved.isValid()) {
        for (int i = 0; i < m_resCombo->count(); ++i) {
            if (m_resCombo->itemData(i).toSize() == saved) {
                m_resCombo->setCurrentIndex(i);
                break;
            }
        }
    }
    m_video->setPreserveAspect(m_aspectCheck->isChecked());
}

void MainWindow::saveSettings()
{
    QSettings settings;

    settings.setValue(QStringLiteral("geometry"), saveGeometry());
    settings.setValue(QStringLiteral("aspectPreserve"), m_aspectCheck->isChecked());

    const QSize res = m_resCombo->currentData().toSize();
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

    m_capture = new Capture(m_devicePath, target, &m_stats);
    m_decoder = new Decoder(&m_queue, &m_stats, m_video, "setFrame");
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