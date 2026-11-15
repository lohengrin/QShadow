// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#pragma once

#include "com_common.h"
#include "platform/platform.h"

#include <QDebug>

#include <mfidl.h>
#include <mfreadwrite.h>

#include <atomic>

class MfReaderCallback;

// Media Foundation capture through an asynchronous source reader. The reader
// hands MJPG JPEGs or packed YUY2 straight through (converters disabled), and
// its callback copies each sample into a pooled buffer and pushes it into the
// FrameQueue from Media Foundation's own thread, never blocking on the decoder.
class MfCapture : public CaptureSource {
public:
    MfCapture(const QString &symbolicLink, const QSize &targetSize,
              device::PixelFormat format, int fps, Stats *stats);
    ~MfCapture() override;

    bool start(FrameQueue *out) override;
    void stop() override;
    void join() override;

    QSize negotiatedSize() const override { return m_negotiated; }
    int negotiatedFps() const override { return m_fps; }
    bool isRunning() const override { return m_running.load(); }
    void setQuiet(bool quiet) override { m_quiet = quiet; }

private:
    friend class MfReaderCallback;
    // Runs on a Media Foundation thread. Returns false once capture must end.
    bool onSample(HRESULT status, DWORD streamFlags, LONGLONG sampleTime, IMFSample *sample);
    bool selectType(IMFMediaType **chosen);
    QDebug warn();

    QString m_link;
    QSize m_targetSize;
    device::PixelFormat m_format;
    int m_requestedFps; // 0: the highest the device offers
    device::ColorInfo m_color;
    Stats *m_stats;
    FrameQueue *m_queue = nullptr;
    bool m_quiet = false;
    QString m_discard;

    wincom::MfRuntime m_mf;
    ComPtr<IMFMediaSource> m_source;
    ComPtr<IMFSourceReader> m_reader;
    ComPtr<MfReaderCallback> m_callback;

    int m_stride = 0;
    QSize m_negotiated;
    int m_fps = 0;
    BufferPool m_pool;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};
};
