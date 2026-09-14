#pragma once

#include <QImage>
#include <QMutex>
#include <QOpenGLWidget>

// Renders the latest decoded frame. The GUI thread only ever blits the newest
// frame; aspect ratio is preserved by letterboxing when enabled.
class VideoWidget : public QOpenGLWidget {
    Q_OBJECT
public:
    explicit VideoWidget(QWidget *parent = nullptr);

    QImage currentFrame() const;

public slots:
    void setFrame(const QImage &frame);
    void setPreserveAspect(bool on);

protected:
    void paintGL() override;

private:
    mutable QMutex m_mutex;
    QImage m_frame;
    bool m_preserveAspect = true;
};