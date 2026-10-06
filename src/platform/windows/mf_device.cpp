#include "mf_device.h"

#include "com_common.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>

namespace mfdev {

namespace {

struct ActivateList {
    IMFActivate **items = nullptr;
    UINT32 count = 0;
    ~ActivateList()
    {
        for (UINT32 i = 0; i < count; ++i)
            items[i]->Release();
        CoTaskMemFree(items);
    }
};

QString stringAttr(IMFActivate *activate, const GUID &key)
{
    WCHAR *value = nullptr;
    UINT32 len = 0;
    QString out;
    if (SUCCEEDED(activate->GetAllocatedString(key, &value, &len))) {
        out = QString::fromWCharArray(value, int(len));
        CoTaskMemFree(value);
    }
    return out;
}

} // namespace

GUID subtypeFor(device::PixelFormat format)
{
    return format == device::PixelFormat::Yuyv ? MFVideoFormat_YUY2 : MFVideoFormat_MJPG;
}

QString findVideo()
{
    wincom::MfRuntime mf;
    if (!mf.ok())
        return {};

    ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(&attrs, 1))
        || FAILED(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                 MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)))
        return {};

    ActivateList list;
    if (FAILED(MFEnumDeviceSources(attrs.Get(), &list.items, &list.count)))
        return {};

    for (UINT32 i = 0; i < list.count; ++i) {
        const QString link = stringAttr(
            list.items[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
        if (wincom::isShadowCastPath(link))
            return link;
    }
    return {};
}

HRESULT createSource(const QString &symbolicLink, IMFMediaSource **source)
{
    ComPtr<IMFAttributes> attrs;
    HRESULT hr = MFCreateAttributes(&attrs, 2);
    if (FAILED(hr))
        return hr;
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    attrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                     reinterpret_cast<LPCWSTR>(symbolicLink.utf16()));
    return MFCreateDeviceSource(attrs.Get(), source);
}

QList<device::Mode> enumerateModes(const QString &videoId, device::PixelFormat format)
{
    QList<device::Mode> modes;
    wincom::MfRuntime mf;
    if (!mf.ok())
        return modes;

    ComPtr<IMFMediaSource> source;
    if (FAILED(createSource(videoId, &source)))
        return modes;

    ComPtr<IMFAttributes> readerAttrs;
    MFCreateAttributes(&readerAttrs, 1);
    if (readerAttrs)
        readerAttrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromMediaSource(source.Get(), readerAttrs.Get(), &reader))) {
        source->Shutdown();
        return modes;
    }

    const GUID wanted = subtypeFor(format);
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> type;
        const HRESULT hr = reader->GetNativeMediaType(wincom::kFirstVideoStream, i, &type);
        if (hr == MF_E_NO_MORE_TYPES || FAILED(hr))
            break;

        GUID subtype = {};
        UINT32 w = 0, h = 0, num = 0, den = 0;
        if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != wanted
            || FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h)))
            continue;
        MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &num, &den);
        const int fps = den ? int(double(num) / double(den) + 0.5) : 0;

        const QSize sz = QSize(int(w), int(h));
        auto it = std::find_if(modes.begin(), modes.end(),
                               [&](const device::Mode &m) { return m.size == sz; });
        if (it == modes.end())
            modes.append(device::Mode{sz, fps});
        else
            it->fps = std::max(it->fps, fps);
    }

    reader.Reset();
    source->Shutdown();

    std::sort(modes.begin(), modes.end(), [](const device::Mode &a, const device::Mode &b) {
        return a.size.width() * a.size.height() > b.size.width() * b.size.height();
    });
    return modes;
}

} // namespace mfdev
