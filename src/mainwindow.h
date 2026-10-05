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
    void checkDevice();
    void onFormatChanged(int index);
    void onResolutionChanged(int index);
    void updateStatus();
    void toggleFullscreen();
    void fitToPipeline();

private:
    void buildUi();
    // Looks the dongle up and (re)fills the format/resolution lists, keeping the
    // user's preferred selection. Returns false while no usable device exists.
    bool connectDevice();
    void populateResolutions(const QSize &preferred);
    void loadSettings();
    void saveSettings();
    // Whole pipeline (video + audio) vs video only: a format/resolution change
    // restarts just the video so the audio keeps playing without a dropout.
    void startPipeline();
    void stopPipeline();
    bool startVideo();
    void stopVideo();
    void startAudio();
    void stopAudio();

    QString m_devicePath;
    // The user's last choice, kept across unplug/replug and sessions even if the
    // current device does not offer it.
    int m_prefFormat = int(device::PixelFormat::Mjpg);
    QSize m_prefSize;
    bool m_captureFailing = false; // log a persistent start failure only once
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
    QTimer m_deviceTimer;
    uint64_t m_lastCaptured = 0;
    uint64_t m_lastDecoded = 0;
    uint64_t m_lastDecodeNs = 0;
};