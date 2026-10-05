#pragma once

#include <QByteArray>
#include <QImage>
#include <QSize>

#include <memory>

// A decoded frame on its way from the decoder thread to the renderer: either
// ready-to-sample RGBA, or YUV left for the GPU to convert (the JPEG's planar
// Y/Cb/Cr, or the capture's raw packed YUYV). Frames are pooled by the decoder and
// recycled once every other shared_ptr holder (widget, renderer) has let go.
struct VideoFrame {
    enum class Kind { Rgba, Yuv, Yuyv };

    // One 8-bit plane; rows are `stride` bytes apart, `width` x `height` are
    // the real sample counts (stride and the allocation include padding).
    struct Plane {
        QByteArray data;
        int width = 0;
        int height = 0;
        int stride = 0;
    };

    Kind kind = Kind::Rgba;
    QImage rgba;     // Kind::Rgba (RGBA8888, tightly packed)
    Plane planes[3]; // Kind::Yuv: Y, Cb, Cr (full-range BT.601 / JFIF)
                     // Kind::Yuyv: planes[0] only, packed 4:2:2, full-range BT.601 (like the JPEGs);
                     //   `data` is the capture buffer itself (shared, not copied)

    // Luma / pixel size of the frame.
    QSize size() const
    {
        return kind == Kind::Rgba ? rgba.size() : QSize(planes[0].width, planes[0].height);
    }
};

using VideoFramePtr = std::shared_ptr<VideoFrame>;
