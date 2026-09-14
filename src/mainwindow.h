#pragma once

#include "audio.h"
#include "capture.h"
#include "decoder.h"
#include "videowidget.h"

#include <QComboBox>
#include <QList>
#include <QMainWindow>
#include <QSize>
#include <QTimer>

class QCheckBox;
class QLabel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void onResolutionChanged(int index);
    void updateStatus();

private:
    void buildUi();
    void findDevices();
    void startPipeline();
    void stopPipeline();
    void showDeviceError(const QString &message);

    QString m_devicePath;
    QList<QSize> m_sizes;
    int m_audioCard = -1;

    VideoWidget *m_video = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_statsLabel = nullptr;
    QComboBox *m_resCombo = nullptr;
    QCheckBox *m_aspectCheck = nullptr;

    Stats m_stats;
    FrameQueue m_queue{3};
    AudioRing m_ring;

    Capture *m_capture = nullptr;
    Decoder *m_decoder = nullptr;
    AudioCapture *m_audio = nullptr;
    AudioSink m_audioSink;

    QTimer m_statusTimer;
    uint64_t m_lastCaptured = 0;
    uint64_t m_lastDecoded = 0;
};