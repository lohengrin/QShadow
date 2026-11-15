// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#pragma once

#include "audio.h"
#include "capture.h"
#include "decoder.h"
#include "platform/platform.h"
#include "videowidget.h"

#include <QComboBox>
#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QSize>
#include <QTimer>

#include <memory>

class QCloseEvent;
class QEvent;
class QMenu;
class QToolBar;
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
    // Fullscreen hides the toolbar and status bar however it was entered/left.
    void changeEvent(QEvent *event) override;

private slots:
    void checkDevice();
    void onDeviceChanged(int index);
    void onAudioChanged(int index);
    void onFormatChanged(int index);
    void onSizeChanged(int index);
    void onRateChanged(int index);
    void updateStatus();
    void toggleFullscreen();
    void fitToPipeline();
    void showContextMenu(const QPoint &globalPos);

private:
    void buildUi();
    // Lists the capture devices, selects the preferred one (else the first) and
    // (re)fills the format/resolution lists, keeping the user's preferred
    // selection. Returns false while no usable device exists.
    bool connectDevice();
    // Refills the audio source list (paired / none / every input) and resolves
    // m_audioId from the user's choice.
    void updateAudioSources();
    // Refills the Format, Resolution and Frame rate lists from what the device
    // offers, selecting the entries closest to the user's preferred format, size
    // and rate. Format and Resolution are on the toolbar (Format only when the
    // device offers several); Frame rate is in the Options menu.
    void populateModes();
    void populateSizes();
    void populateRates();
    struct CurrentMode {
        device::PixelFormat format = device::PixelFormat::Mjpg;
        QSize size;
        int fps = 0; // 0: the device's default / highest
    };
    CurrentMode currentMode() const;
    // The options shared by the toolbar's Options button and the right-click menu.
    void populateMenu(QMenu *menu);
    void loadSettings();
    void saveSettings();
    // Whole pipeline (video + audio) vs video only: a format/resolution change
    // restarts just the video so the audio keeps playing without a dropout.
    void startPipeline();
    void stopPipeline();
    bool startVideo();
    void stopVideo();
    void restartVideo();
    void startAudio();
    void stopAudio();

    QString m_devicePath; // video id of the selected device, empty if none usable
    QString m_idleStatus = QStringLiteral("Waiting for a video capture device…"); // why nothing is running, shown while m_devicePath is empty
    QList<platform::DeviceInfo> m_devices;
    QString m_prefDevice; // DeviceInfo::key() of the user's choice, kept across unplug
    // The user's last choice, kept across unplug/replug and sessions even if the
    // current device does not offer it.
    int m_prefFormat = int(device::PixelFormat::Mjpg);
    QSize m_prefSize;
    int m_prefFps = 0; // 0: the highest the mode offers
    bool m_captureFailing = false; // log a persistent start failure only once
    QHash<int, QList<device::Mode>> m_sizes; // keyed by device::PixelFormat
    int m_vid = 0; // USB ids of the selected device (quirks lookup)
    int m_pid = 0;
    QString m_pairedAudioId; // the selected device's own audio interface, may be empty
    QString m_prefAudio = QStringLiteral("auto"); // "auto", "none" or "in:<input name>"
    QList<platform::AudioInputInfo> m_audioInputs;
    QString m_audioId; // backend-defined identifier of the device's audio interface, may be empty

    QVulkanInstance *m_vulkanInstance = nullptr;
    VideoWidget *m_video = nullptr;
    QToolBar *m_toolbar = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_statsLabel = nullptr;
    QComboBox *m_deviceCombo = nullptr;
    QComboBox *m_audioCombo = nullptr; // model of the Audio source menu (never shown)
    QComboBox *m_sizeCombo = nullptr;
    QComboBox *m_formatCombo = nullptr;
    QAction *m_formatAction = nullptr; // the toolbar entry of m_formatCombo (hidden if one format)
    QComboBox *m_rateCombo = nullptr;   // model of the Frame rate menu (never shown)
    QComboBox *m_shaderCombo = nullptr;
    // Downscale and Audio source live in the Options menu: these combos only hold
    // the choices and the current one (never shown).
    QComboBox *m_downscaleCombo = nullptr;
    QAction *m_aspectAction = nullptr;
    QLabel *m_warnLabel = nullptr;
    QSlider *m_volumeSlider = nullptr;
    QAction *m_fullscreenAction = nullptr;
    QAction *m_fitAction = nullptr;

    Stats m_stats;
    // Latest-wins: while the decoder works on one frame at most one more waits.
    FrameQueue m_queue{2};
    AudioRing m_ring;

    std::unique_ptr<CaptureSource> m_capture;
    Decoder *m_decoder = nullptr;
    std::unique_ptr<AudioInput> m_audio;
    AudioSink m_audioSink;

    QTimer m_statusTimer;
    QTimer m_deviceTimer;
    uint64_t m_lastCaptured = 0;
    uint64_t m_lastDecoded = 0;
    uint64_t m_lastDecodeNs = 0;
    uint64_t m_lastDrops = 0;
    uint64_t m_lastUnderruns = 0;
    uint64_t m_lastXruns = 0;
};