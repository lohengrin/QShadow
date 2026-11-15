// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#include "mf_capture.h"

#include "mf_device.h"

#include <mfapi.h>
#include <mferror.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>

// The reader's sample callback. Media Foundation may keep it alive past the
// capture object, so it reaches back through an owner pointer that join()
// clears under the lock before the capture is destroyed.
class MfReaderCallback : public IMFSourceReaderCallback {
public:
    explicit MfReaderCallback(MfCapture *owner) : m_owner(owner) {}

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFSourceReaderCallback)) {
            *ppv = static_cast<IMFSourceReaderCallback *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++m_refs; }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG n = --m_refs;
        if (n == 0)
            delete this;
        return n;
    }

    // IMFSourceReaderCallback
    STDMETHODIMP OnReadSample(HRESULT status, DWORD, DWORD streamFlags, LONGLONG sampleTime,
                              IMFSample *sample) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_owner)
            return S_OK;
        if (!m_owner->onSample(status, streamFlags, sampleTime, sample))
            return S_OK;
        const HRESULT hr = m_owner->m_reader->ReadSample(
            wincom::kFirstVideoStream, 0, nullptr, nullptr, nullptr, nullptr);
        if (FAILED(hr))
            m_owner->m_running.store(false);
        return S_OK;
    }
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent *) override { return S_OK; }
    STDMETHODIMP OnFlush(DWORD) override
    {
        {
            std::lock_guard<std::mutex> lock(m_flushMutex);
            m_flushed = true;
        }
        m_flushCv.notify_all();
        return S_OK;
    }

    // Optionally waits for a Flush() to complete, then detaches from the owner
    // (waiting out any callback still running).
    void detach(bool waitForFlush)
    {
        if (waitForFlush) {
            std::unique_lock<std::mutex> lock(m_flushMutex);
            m_flushCv.wait_for(lock, std::chrono::seconds(2), [this] { return m_flushed; });
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_owner = nullptr;
    }

private:
    std::atomic<ULONG> m_refs{1};
    std::mutex m_mutex; // held while the owner is used
    MfCapture *m_owner;
    std::mutex m_flushMutex;
    std::condition_variable m_flushCv;
    bool m_flushed = false;
};

namespace {

// No real JPEG frame is smaller than this.
constexpr DWORD kMinJpegBytes = 64;

// Capture time of a sample as a steady_clock point. Media Foundation stamps
// capture samples on the system clock (MFGetSystemTime); anything implausible
// falls back to "now".
std::chrono::steady_clock::time_point sampleStamp(LONGLONG sampleTime)
{
    const auto now = std::chrono::steady_clock::now();
    if (sampleTime <= 0)
        return now;
    const LONGLONG age = MFGetSystemTime() - sampleTime; // 100 ns units
    if (age < 0 || age > 10'000'000)
        return now;
    return now - std::chrono::nanoseconds(age * 100);
}

} // namespace

MfCapture::MfCapture(const QString &symbolicLink, const QSize &targetSize,
                     device::PixelFormat format, Stats *stats)
    : m_link(symbolicLink), m_targetSize(targetSize), m_format(format), m_stats(stats)
{
}

MfCapture::~MfCapture()
{
    stop();
    join();
}

QDebug MfCapture::warn()
{
    if (m_quiet) {
        m_discard.clear();
        return QDebug(&m_discard);
    }
    return qWarning();
}

// Picks the native type of the requested format and size with the highest rate.
bool MfCapture::selectType(IMFMediaType **chosen)
{
    const GUID wanted = mfdev::subtypeFor(m_format);
    double bestFps = -1.0;
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> type;
        if (FAILED(m_reader->GetNativeMediaType(wincom::kFirstVideoStream, i, &type)))
            break;
        GUID subtype = {};
        UINT32 w = 0, h = 0, num = 0, den = 0;
        if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != wanted
            || FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h))
            || int(w) != m_targetSize.width() || int(h) != m_targetSize.height())
            continue;
        MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &num, &den);
        const double fps = den ? double(num) / double(den) : 0.0;
        if (fps > bestFps) {
            if (*chosen)
                (*chosen)->Release();
            bestFps = fps;
            m_fps = int(fps + 0.5);
            m_negotiated = QSize(int(w), int(h));
            *chosen = type.Detach();
        }
    }
    return bestFps >= 0.0;
}

bool MfCapture::start(FrameQueue *out)
{
    m_queue = out;
    if (!m_mf.ok()) {
        warn() << "Capture: Media Foundation failed to start";
        return false;
    }

    HRESULT hr = mfdev::createSource(m_link, &m_source);
    if (FAILED(hr)) {
        warn() << "Capture: failed to open" << m_link << wincom::hresultText(hr);
        return false;
    }

    // Async reader: samples arrive on a Media Foundation thread as soon as the
    // device delivers them. Converters stay off so MJPG/YUY2 pass through
    // untouched (the decoder and the GPU shaders do the rest).
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 4);
    m_callback.Attach(new MfReaderCallback(this));
    attrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, m_callback.Get());
    attrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
    attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    attrs->SetUINT32(MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, TRUE);
    hr = MFCreateSourceReaderFromMediaSource(m_source.Get(), attrs.Get(), &m_reader);
    if (FAILED(hr)) {
        warn() << "Capture: MFCreateSourceReader failed:" << wincom::hresultText(hr);
        join();
        return false;
    }

    ComPtr<IMFMediaType> type;
    if (!selectType(type.GetAddressOf())) {
        warn() << "Capture: the device offers no such format/size" << m_targetSize;
        join();
        return false;
    }
    hr = m_reader->SetCurrentMediaType(wincom::kFirstVideoStream, nullptr, type.Get());
    if (FAILED(hr)) {
        warn() << "Capture: SetCurrentMediaType failed:" << wincom::hresultText(hr);
        join();
        return false;
    }

    if (m_format == device::PixelFormat::Yuyv) {
        UINT32 s = 0;
        int stride = 0;
        if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &s)))
            stride = std::abs(int(s));
        m_stride = std::max(stride, m_negotiated.width() * 2);
    } else {
        m_stride = 0;
    }

    // The largest frame a pooled buffer will hold.
    UINT32 imageSize = 0;
    if (FAILED(MFCalculateImageSize(mfdev::subtypeFor(m_format), UINT32(m_negotiated.width()),
                                    UINT32(m_negotiated.height()), &imageSize))
        || imageSize == 0)
        imageSize = UINT32(m_negotiated.width() * m_negotiated.height() * 2);
    m_pool.reserve(imageSize);

    m_running = true;
    hr = m_reader->ReadSample(wincom::kFirstVideoStream, 0, nullptr, nullptr,
                              nullptr, nullptr);
    if (FAILED(hr)) {
        m_running = false;
        warn() << "Capture: ReadSample failed:" << wincom::hresultText(hr);
        join();
        return false;
    }

    qInfo() << "Capture:" << m_negotiated.width() << "x" << m_negotiated.height() << "@" << m_fps;
    return true;
}

bool MfCapture::onSample(HRESULT status, DWORD streamFlags, LONGLONG sampleTime, IMFSample *sample)
{
    if (m_stop.load()) {
        m_running.store(false);
        return false;
    }
    if (FAILED(status) || (streamFlags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
        // Unplugged, device lost or stream ended.
        m_running.store(false);
        return false;
    }
    if (!sample)
        return true; // stream tick / gap: just ask again

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
        return true;
    BYTE *data = nullptr;
    DWORD length = 0;
    if (FAILED(buffer->Lock(&data, nullptr, &length)))
        return true;

    const size_t rawSize = size_t(m_stride) * size_t(m_negotiated.height());
    // A short YUYV frame (dropped USB packets) can't be converted: skip it.
    // The dongle also sends empty JPEGs (just SOI+EOI, 4 bytes) between real
    // frames; they carry no image and would only queue ahead of the next frame.
    const bool complete = m_format == device::PixelFormat::Mjpg ? length > kMinJpegBytes
                                                                : length >= rawSize;
    if (complete) {
        Frame frame;
        frame.stamp = sampleStamp(sampleTime);
        frame.format = m_format;
        frame.width = m_negotiated.width();
        frame.height = m_negotiated.height();
        frame.stride = m_stride;
        frame.data = m_pool.take(reinterpret_cast<const char *>(data),
                                 m_format == device::PixelFormat::Yuyv ? rawSize : length);
        m_stats->captured.fetch_add(1);
        m_queue->push(std::move(frame));
    }
    buffer->Unlock();
    return true;
}

void MfCapture::stop()
{
    m_stop.store(true);
}

void MfCapture::join()
{
    if (m_reader) {
        // Cancel pending reads and wait until the last callback has returned.
        const bool flushing = SUCCEEDED(m_reader->Flush(wincom::kFirstVideoStream));
        if (m_callback)
            m_callback->detach(flushing);
        m_reader.Reset();
    } else if (m_callback) {
        m_callback->detach(false);
    }
    m_callback.Reset();
    if (m_source) {
        m_source->Shutdown();
        m_source.Reset();
    }
    m_running.store(false);
}
