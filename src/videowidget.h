#pragma once

#include <QImage>
#include <QMutex>
#include <QOpenGLBuffer>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QRectF>
#include <QSize>

#include <array>

// Renders the latest decoded frame as a full-screen textured quad through a
// selectable shader program. The GUI thread only ever blits the newest frame;
// aspect ratio is preserved by letterboxing when enabled.
class VideoWidget : public QOpenGLWidget {
    Q_OBJECT
public:
    enum class ShaderMode {
        None = 0,
        Bilinear,
        Hq4x,
        Xbrz,
        CrtGeom,
        CrtRoyale,
    };

    explicit VideoWidget(QWidget *parent = nullptr);

    QImage currentFrame() const;

public slots:
    void setFrame(const QImage &frame);
    void setPreserveAspect(bool on);
    void setShaderMode(ShaderMode mode);
    void setDownscale(int factor);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    struct Vertex {
        float x, y, u, v;
    };

    QOpenGLShaderProgram *programFor(ShaderMode mode);
    bool downscaleProgramReady();
    void uploadFrame(const QImage &frame);
    void uploadLut();
    void updateQuad(const QRectF &target);
    void runDownscale(const QSize &targetSize);
    void drawFrame(GLuint texture, const QSize &texSize, const QRectF &target);

    mutable QMutex m_mutex;
    QImage m_frame;
    bool m_preserveAspect = true;
    ShaderMode m_shaderMode = ShaderMode::None;
    int m_downscale = 1; // nearest-neighbour decimation of the capture (1/2/4)

    QOpenGLFunctions_3_3_Core *m_gl = nullptr;
    QOpenGLVertexArrayObject m_vao;
    QOpenGLBuffer m_vbo{QOpenGLBuffer::VertexBuffer};
    std::array<GLuint, 2> m_textures{0, 0}; // [0]=frame, [1]=hq4x LUT
    QSize m_frameSize;                      // allocated texture dimensions
    QOpenGLFramebufferObject *m_fbo = nullptr;
    QSize m_fboSize;
    QOpenGLShaderProgram m_downscaleProgram;
    bool m_downscaleReady = false;
    std::array<QOpenGLShaderProgram, 6> m_programs;
    bool m_programReady[6] = {};
    bool m_lutReady = false;
};