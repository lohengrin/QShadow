#include "videowidget.h"

#include <QOpenGLContext>
#include <QOpenGLShader>
#include <QOpenGLVersionFunctionsFactory>
#include <QVector2D>
#include <QVector3D>

#include <QtMath>

#include <cstddef>
#include <cmath>
#include <utility>

// Port of the maxscale(sinangle, cosangle, coordaspect) block from the
// upstream crt-geom vertex shader (see shaders/ATTRIBUTION.md), computed on
// the CPU once per frame. Returns x/y = average point and z = max scale.

VideoWidget::VideoWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAutoFillBackground(false);
}

QImage VideoWidget::currentFrame() const
{
    QMutexLocker locker(&m_mutex);
    return m_frame;
}

void VideoWidget::setFrame(const QImage &frame)
{
    {
        QMutexLocker locker(&m_mutex);
        if (frame.isNull())
            return;
        m_frame = frame;
    }
    update();
}

void VideoWidget::setPreserveAspect(bool on)
{
    m_preserveAspect = on;
    update();
}

void VideoWidget::setShaderMode(ShaderMode mode)
{
    if (m_shaderMode == mode)
        return;
    m_shaderMode = mode;
    update();
}

void VideoWidget::initializeGL()
{
    m_gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(context());
    if (!m_gl)
        qFatal("QOpenGLFunctions_3_3_Core unavailable");

    m_gl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    m_vao.create();
    m_vao.bind();
    m_vbo.create();
    m_vbo.bind();
    m_vbo.setUsagePattern(QOpenGLBuffer::DynamicDraw);
    m_gl->glEnableVertexAttribArray(0);
    m_gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                                reinterpret_cast<void *>(offsetof(Vertex, x)));
    m_gl->glEnableVertexAttribArray(1);
    m_gl->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                                reinterpret_cast<void *>(offsetof(Vertex, u)));
    m_vbo.release();
    m_vao.release();

    m_gl->glGenTextures(2, m_textures.data());
}

void VideoWidget::resizeGL(int, int)
{
}

void VideoWidget::paintGL()
{
    QMutexLocker locker(&m_mutex);
    m_gl->glClear(GL_COLOR_BUFFER_BIT);
    if (m_frame.isNull())
        return;

    const QSizeF src(m_frame.width(), m_frame.height());
    QRectF target(0, 0, width(), height());
    if (m_preserveAspect) {
        const QSizeF scaled = src.scaled(size(), Qt::KeepAspectRatio);
        target = QRectF((width() - scaled.width()) / 2.0,
                        (height() - scaled.height()) / 2.0,
                        scaled.width(), scaled.height());
    }
    locker.unlock();

    uploadFrame(m_frame);
    updateQuad(target);
    drawFrame(target);
}

QOpenGLShaderProgram *VideoWidget::programFor(ShaderMode mode)
{
    const int index = static_cast<int>(mode);
    if (m_programReady[index])
        return &m_programs[index];

    static const char *fragSources[] = {
        ":/shaders/passthrough.frag", // None
        ":/shaders/passthrough.frag", // Bilinear
        ":/shaders/hq4x.frag",        // Hq4x
        ":/shaders/xbrz.frag",        // Xbrz
        ":/shaders/crt-geom.frag",    // CrtGeom
        ":/shaders/crt-royale.frag",  // CrtRoyale
    };

    QOpenGLShaderProgram *program = &m_programs[index];
    if (!program->addShaderFromSourceFile(QOpenGLShader::Vertex,
                                          ":/shaders/passthrough.vert")) {
        qWarning() << "Vertex shader:" << program->log();
        return nullptr;
    }
    if (!program->addShaderFromSourceFile(QOpenGLShader::Fragment, fragSources[index])) {
        qWarning() << "Fragment shader:" << program->log();
        return nullptr;
    }
    if (!program->link()) {
        qWarning() << "Shader link:" << program->log();
        return nullptr;
    }
    m_programReady[index] = true;
    return program;
}

void VideoWidget::uploadFrame(const QImage &frame)
{
    m_gl->glActiveTexture(GL_TEXTURE0);
    m_gl->glBindTexture(GL_TEXTURE_2D, m_textures[0]);

    const GLsizei w = frame.width();
    const GLsizei h = frame.height();
    const bool resize = w != m_frameSize.width() || h != m_frameSize.height();
    if (resize)
        m_gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.bytesPerLine() / 3);

    if (resize) {
        m_gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_RGB,
                           GL_UNSIGNED_BYTE, frame.constBits());
        m_frameSize = QSize(w, h);
    } else {
        m_gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.bytesPerLine() / 3);
        m_gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGB,
                              GL_UNSIGNED_BYTE, frame.constBits());
    }
    m_gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

void VideoWidget::uploadLut()
{
    QImage lut(QStringLiteral(":/shaders/hq4x.png"));
    if (lut.isNull()) {
        qWarning() << "hq4x LUT missing from resources";
        return;
    }
    lut = lut.convertToFormat(QImage::Format_RGBA8888);

    m_gl->glActiveTexture(GL_TEXTURE1);
    m_gl->glBindTexture(GL_TEXTURE_2D, m_textures[1]);
    m_gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, lut.bytesPerLine() / 4);
    m_gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, lut.width(), lut.height(), 0,
                       GL_RGBA, GL_UNSIGNED_BYTE, lut.constBits());
    m_gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    m_lutReady = true;
}

void VideoWidget::updateQuad(const QRectF &target)
{
    const GLfloat halfW = width() / 2.0f;
    const GLfloat halfH = height() / 2.0f;
    const GLfloat l = GLfloat(target.left()) / halfW - 1.0f;
    const GLfloat r = GLfloat(target.right()) / halfW - 1.0f;
    const GLfloat t = 1.0f - GLfloat(target.top()) / halfH;
    const GLfloat b = 1.0f - GLfloat(target.bottom()) / halfH;

    // y axis points up in NDC; texture data starts at the top scanline of the
    // image (v = 0), so the image top maps to the upper screen edge.
    const Vertex verts[4] = {
        {l, t, 0.0f, 0.0f},
        {r, t, 1.0f, 0.0f},
        {l, b, 0.0f, 1.0f},
        {r, b, 1.0f, 1.0f},
    };

    m_vbo.bind();
    m_vbo.allocate(verts, sizeof(verts));
    m_vbo.release();
}

void VideoWidget::drawFrame(const QRectF &target)
{
    QOpenGLShaderProgram *program = programFor(m_shaderMode);
    if (!program)
        return;
    program->bind();

    m_gl->glActiveTexture(GL_TEXTURE0);
    m_gl->glBindTexture(GL_TEXTURE_2D, m_textures[0]);
    const bool smooth = m_shaderMode != ShaderMode::None;
    const GLint filter = smooth ? GL_LINEAR : GL_NEAREST;
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    program->setUniformValue("uTex", 0);
    program->setUniformValue("uTexSize",
                             QVector2D(float(m_frameSize.width()),
                                       float(m_frameSize.height())));

    if (m_shaderMode == ShaderMode::Hq4x) {
        if (!m_lutReady)
            uploadLut();
        m_gl->glActiveTexture(GL_TEXTURE1);
        m_gl->glBindTexture(GL_TEXTURE_2D, m_textures[1]);
        program->setUniformValue("uLut", 1);
    }

    program->setUniformValue("uOutSize",
                             QVector2D(float(target.width()), float(target.height())));

    if (m_shaderMode == ShaderMode::CrtGeom) {
        m_gl->glActiveTexture(GL_TEXTURE0);

        // CPU port of the crt-geom vertex shader maxscale() block, evaluated
        // at the baked default parameters (aspect normalized to y = 1.0,
        // zero tilt => sin/cos angle constants 0.001/1.001).
        const float aspect = target.width() / target.height();
        const float d = 1.6f;
        const float R = 2.0f;
        const float cosA = 1.001f;
        const float sinA = 0.001f;

        const auto intersect = [&](float x, float y) {
            const float xy = x * x + y * y;
            const float A = xy + d * d;
            const float B = 2.0f * (R * (x * sinA + y * sinA - d * cosA * cosA) - d * d);
            const float C = d * d + 2.0f * R * d * cosA * cosA;
            return (-B - std::sqrt(B * B - 4.0f * A * C)) / (2.0f * A);
        };
        const auto bkwtrans = [&](float px, float py) {
            const float c = intersect(px, py);
            float ux = c * px + R * sinA;
            float uy = c * py + R * sinA;
            ux /= R;
            uy /= R;
            const float t0 = sinA / cosA;
            const float p0x = ux / cosA;
            const float p0y = uy / cosA;
            const float A = t0 * t0 + t0 * t0 + 1.0f;
            const float B = -2.0f * (p0x * t0 + p0y * t0);
            const float C = p0x * p0x + p0y * p0y - 1.0f;
            const float a = (-B + std::sqrt(B * B - 4.0f * A * C)) / (2.0f * A);
            ux = (ux - a * sinA) / cosA;
            uy = (uy - a * sinA) / cosA;
            const float r = std::max(std::abs(R * std::acos(a)), 1e-5f);
            const float s = std::sin(r / R) / r;
            return std::pair<float, float>(ux * s, uy * s);
        };
        const auto fwtrans = [&](float ux, float uy) {
            const float r = std::max(std::sqrt(ux * ux + uy * uy), 1e-5f);
            const float sr = std::sin(r / R) / r;
            ux *= sr;
            uy *= sr;
            const float x = 1.0f - std::cos(r / R);
            const float D = d / R + x * cosA * cosA + ux * sinA + uy * sinA;
            return std::pair<float, float>(d * (ux * cosA - x * sinA) / D,
                                           d * (uy * cosA - x * sinA) / D);
        };

        const auto c = bkwtrans(-R * sinA / (1.0f + R / d * cosA * cosA),
                                -R * sinA / (1.0f + R / d * cosA * cosA));
        const float a = 0.5f * aspect;
        const auto lo = fwtrans(-a, c.second);
        const auto hi = fwtrans(a, c.second);
        const auto lo2 = fwtrans(c.first, -0.5f);
        const auto hi2 = fwtrans(c.first, 0.5f);

        // lo/hi in the upstream are divided by aspect before averaging.
        const float loX = lo.first / aspect;
        const float hiX = hi.first / aspect;
        const float loY = lo2.second;
        const float hiY = hi2.second;
        const float stretchX = (hiX + loX) * aspect * 0.5f;
        const float stretchY = (hiY + loY) * 0.5f;
        const float stretchZ = std::max(hiX - loX, hiY - loY);

        program->setUniformValue("uInSize",
                                 QVector2D(float(m_frameSize.width()),
                                           float(m_frameSize.height())));
        program->setUniformValue("uAspect", QVector2D(aspect, 1.0f));
        program->setUniformValue("uStretch",
                                 QVector3D(stretchX, stretchY, stretchZ));
        // mod_factor = uv.x * TextureSize.x * OutputSize.x / InputSize.x,
        // which for a 1:1 source texture reduces to uv.x * OutputSize.x.
        program->setUniformValue("uModScale", float(target.width()));
    }

    m_vao.bind();
    m_gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_vao.release();

    program->release();
}