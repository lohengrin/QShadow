#include "wasapi_audio.h"

#include "com_common.h"

#include <QDebug>

#include <initguid.h>
#include <audioclient.h>
#include <avrt.h>
#include <devicetopology.h>
#include <mmdeviceapi.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

// REFERENCE_TIME units (100 ns) in 20 ms: the buffer asked for when the engine
// period cannot be tuned.
constexpr REFERENCE_TIME kFallbackBuffer = 200'000;

QString coString(LPWSTR s)
{
    QString out = s ? QString::fromWCharArray(s) : QString();
    CoTaskMemFree(s);
    return out;
}

bool isFloatFormat(const WAVEFORMATEX *fmt)
{
    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        return true;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(fmt);
        return ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    return false;
}

bool isPcm16Format(const WAVEFORMATEX *fmt)
{
    if (fmt->wBitsPerSample != 16)
        return false;
    if (fmt->wFormatTag == WAVE_FORMAT_PCM)
        return true;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(fmt);
        return ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM;
    }
    return false;
}

// True if the endpoint's topology leads to the ShadowCast USB device.
bool endpointIsShadowCast(IMMDevice *endpoint)
{
    ComPtr<IDeviceTopology> topology;
    if (SUCCEEDED(endpoint->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void **>(topology.GetAddressOf())))) {
        UINT count = 0;
        topology->GetConnectorCount(&count);
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IConnector> connector;
            if (FAILED(topology->GetConnector(i, &connector)))
                continue;
            LPWSTR id = nullptr;
            if (SUCCEEDED(connector->GetDeviceIdConnectedTo(&id))
                && wincom::isShadowCastPath(coString(id)))
                return true;
        }
    }
    return false;
}

QString endpointName(IMMDevice *endpoint)
{
    ComPtr<IPropertyStore> props;
    QString name;
    if (SUCCEEDED(endpoint->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
            name = QString::fromWCharArray(v.pwszVal);
        PropVariantClear(&v);
    }
    return name;
}

} // namespace

namespace wasapi {

QString findCaptureEndpoint()
{
    wincom::ComThread com;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator))))
        return {};
    ComPtr<IMMDeviceCollection> endpoints;
    if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &endpoints)))
        return {};

    UINT count = 0;
    endpoints->GetCount(&count);
    QString byName;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> endpoint;
        if (FAILED(endpoints->Item(i, &endpoint)))
            continue;
        LPWSTR rawId = nullptr;
        if (FAILED(endpoint->GetId(&rawId)))
            continue;
        const QString id = coString(rawId);
        // Identify it by the USB IDs; fall back to the name only if no endpoint
        // resolves to the dongle that way.
        if (endpointIsShadowCast(endpoint.Get()))
            return id;
        if (byName.isEmpty()
            && endpointName(endpoint.Get()).contains(QStringLiteral("ShadowCast"),
                                                     Qt::CaseInsensitive))
            byName = id;
    }
    return byName;
}

} // namespace wasapi

WasapiAudioInput::WasapiAudioInput(const QString &endpointId, AudioRing *ring, Stats *stats)
    : m_endpointId(endpointId), m_ring(ring), m_stats(stats)
{
}

WasapiAudioInput::~WasapiAudioInput()
{
    stop();
    join();
}

bool WasapiAudioInput::start()
{
    if (m_endpointId.isEmpty()) {
        m_error = QStringLiteral("ShadowCast audio endpoint not found");
        return false;
    }
    m_stop.store(false);
    m_stats->audioUnderruns.store(0);
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    // All device work happens on the capture thread (its own COM apartment);
    // wait here for it to report whether the stream came up.
    std::promise<bool> ready;
    std::future<bool> result = ready.get_future();
    m_thread = std::thread([this, &ready] { run(&ready); });
    if (!result.get()) {
        join();
        return false;
    }
    return true;
}

void WasapiAudioInput::stop()
{
    m_stop.store(true);
    if (m_stopEvent)
        SetEvent(HANDLE(m_stopEvent));
}

void WasapiAudioInput::join()
{
    if (m_thread.joinable())
        m_thread.join();
    if (m_stopEvent) {
        CloseHandle(HANDLE(m_stopEvent));
        m_stopEvent = nullptr;
    }
}

void WasapiAudioInput::run(std::promise<bool> *ready)
{
    wincom::ComThread com;
    bool reported = false;
    auto fail = [&](const QString &msg) {
        m_error = msg;
        if (!reported)
            ready->set_value(false);
        reported = true;
    };

    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> endpoint;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))
        || FAILED(enumerator->GetDevice(reinterpret_cast<LPCWSTR>(m_endpointId.utf16()),
                                        &endpoint))) {
        fail(QStringLiteral("ShadowCast audio endpoint is gone"));
        return;
    }

    ComPtr<IAudioClient> client;
    HRESULT hr = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void **>(client.GetAddressOf()));
    if (FAILED(hr)) {
        fail(QStringLiteral("IAudioClient activation failed: %1").arg(wincom::hresultText(hr)));
        return;
    }

    // The engine's mix format is what comes out without extra conversion. Use it
    // when the pipeline can take it (48 kHz multiples, float or S16); otherwise
    // let the engine convert to 48 kHz stereo S16.
    WAVEFORMATEX *mix = nullptr;
    hr = client->GetMixFormat(&mix);
    if (FAILED(hr)) {
        fail(QStringLiteral("GetMixFormat failed: %1").arg(wincom::hresultText(hr)));
        return;
    }
    WAVEFORMATEX custom = {};
    const WAVEFORMATEX *fmt = mix;
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    bool native = mix->nSamplesPerSec % 48000 == 0
                  && (isFloatFormat(mix) ? mix->wBitsPerSample == 32 : isPcm16Format(mix));
    if (!native) {
        custom.wFormatTag = WAVE_FORMAT_PCM;
        custom.nChannels = 2;
        custom.nSamplesPerSec = 48000;
        custom.wBitsPerSample = 16;
        custom.nBlockAlign = 4;
        custom.nAvgBytesPerSec = 48000 * 4;
        fmt = &custom;
        flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    }
    const bool isFloat = native && isFloatFormat(fmt);
    const int channels = fmt->nChannels;
    const int rate = int(fmt->nSamplesPerSec);

    // Smallest shared-mode engine period (IAudioClient3), else the default one.
    bool initialized = false;
    if (native) {
        ComPtr<IAudioClient3> client3;
        if (SUCCEEDED(client.As(&client3))) {
            UINT32 defPeriod = 0, fundamental = 0, minPeriod = 0, maxPeriod = 0;
            if (SUCCEEDED(client3->GetSharedModeEnginePeriod(fmt, &defPeriod, &fundamental,
                                                             &minPeriod, &maxPeriod))
                && SUCCEEDED(client3->InitializeSharedAudioStream(
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minPeriod, fmt, nullptr))) {
                initialized = true;
                qInfo() << "Audio capture:" << rate << "Hz," << channels << "ch, engine period"
                        << double(minPeriod) * 1000.0 / double(rate) << "ms";
            }
        }
    }
    if (!initialized) {
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kFallbackBuffer, 0, fmt, nullptr);
        if (FAILED(hr)) {
            CoTaskMemFree(mix);
            fail(QStringLiteral("IAudioClient::Initialize failed: %1")
                     .arg(wincom::hresultText(hr)));
            return;
        }
    }
    CoTaskMemFree(mix);

    HANDLE sampleEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ComPtr<IAudioCaptureClient> capture;
    if (FAILED(client->SetEventHandle(sampleEvent))
        || FAILED(client->GetService(IID_PPV_ARGS(&capture)))) {
        CloseHandle(sampleEvent);
        fail(QStringLiteral("Failed to set up WASAPI capture"));
        return;
    }

    AudioPipeline pipeline(m_ring, rate, channels);

    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    hr = client->Start();
    if (FAILED(hr)) {
        if (mmcss)
            AvRevertMmThreadCharacteristics(mmcss);
        CloseHandle(sampleEvent);
        fail(QStringLiteral("IAudioClient::Start failed: %1").arg(wincom::hresultText(hr)));
        return;
    }
    ready->set_value(true);
    reported = true;

    std::vector<int16_t> samples;
    const HANDLE waits[2] = {HANDLE(m_stopEvent), sampleEvent};
    while (!m_stop.load()) {
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, 200);
        if (w == WAIT_OBJECT_0)
            break;
        if (w == WAIT_TIMEOUT)
            continue;

        for (;;) {
            UINT32 packet = 0;
            hr = capture->GetNextPacketSize(&packet);
            if (FAILED(hr)) {
                m_error = QStringLiteral("GetNextPacketSize: %1").arg(wincom::hresultText(hr));
                m_stop.store(true);
                break;
            }
            if (packet == 0)
                break;

            BYTE *data = nullptr;
            UINT32 frames = 0;
            DWORD bufFlags = 0;
            hr = capture->GetBuffer(&data, &frames, &bufFlags, nullptr, nullptr);
            if (FAILED(hr)) {
                m_error = QStringLiteral("GetBuffer: %1").arg(wincom::hresultText(hr));
                m_stop.store(true);
                break;
            }

            const size_t n = size_t(frames) * size_t(channels);
            if (bufFlags & AUDCLNT_BUFFERFLAGS_SILENT) {
                samples.assign(n, 0);
                pipeline.process(samples.data(), frames);
            } else if (isFloat) {
                samples.resize(n);
                const auto *in = reinterpret_cast<const float *>(data);
                for (size_t i = 0; i < n; ++i)
                    samples[i] = int16_t(std::lrintf(std::clamp(in[i], -1.0f, 1.0f) * 32767.0f));
                pipeline.process(samples.data(), frames);
            } else {
                pipeline.process(reinterpret_cast<const int16_t *>(data), frames);
            }
            m_driftPpm.store(pipeline.driftPpm(), std::memory_order_relaxed);
            capture->ReleaseBuffer(frames);
        }
    }

    client->Stop();
    if (mmcss)
        AvRevertMmThreadCharacteristics(mmcss);
    CloseHandle(sampleEvent);
}
