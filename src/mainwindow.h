#pragma once

#include "audio.h"
#include "capture.h"
#include "decoder.h"
#include "videowidget.h"

#include <QComboBox>
#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QSize>
#include <QTimer>

class QCheckBox;
class QCloseEvent;
class QLabel;
class QSlider;
class QVulkanInstance;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QVulkanInstance *vulkan = nullptr, QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onFormatChanged(int index);
    void onResolutionChanged(int index);
    void updateStatus();
    void toggleFullscreen();
    void fitToPipeline();

private:
    void buildUi();
    void findDevices();
    void populateResolutions(const QSize &preferred);
    void loadSettings();
    void saveSettings();
    void startPipeline();
    void stopPipeline();
    void showDeviceError(const QString &message);

    QString m_devicePath;
    QHash<int, QList<QSize>> m_sizes; // keyed by device::PixelFormat
    int m_audioCard = -1;

    QVulkanInstance *m_vulkanInstance = nullptr;
    VideoWidget *m_video = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_statsLabel = nullptr;
    QComboBox *m_formatCombo = nullptr;
    QComboBox *m_resCombo = nullptr;
    QComboBox *m_shaderCombo = nullptr;
    QComboBox *m_downscaleCombo = nullptr;
    QCheckBox *m_aspectCheck = nullptr;
    QSlider *m_volumeSlider = nullptr;
    QAction *m_fullscreenAction = nullptr;
    QAction *m_fitAction = nullptr;

    Stats m_stats;
    // Latest-wins: while the decoder works on one frame at most one more waits.
    FrameQueue m_queue{2};
    AudioRing m_ring;

    Capture *m_capture = nullptr;
    Decoder *m_decoder = nullptr;
    AudioCapture *m_audio = nullptr;
    AudioSink m_audioSink;

    QTimer m_statusTimer;
    uint64_t m_lastCaptured = 0;
    uint64_t m_lastDecoded = 0;
};