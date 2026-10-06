#pragma once

// Small COM / Media Foundation helpers shared by the Windows backend.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <wrl/client.h>

#include <QString>

using Microsoft::WRL::ComPtr;

namespace wincom {

// The negative MF_SOURCE_READER_FIRST_VIDEO_STREAM as the DWORD the API takes.
inline constexpr DWORD kFirstVideoStream = 0xFFFFFFFCu;

// Media Foundation is process-wide and reference counted; hold one of these for
// as long as any MF object created through it is alive.
class MfRuntime {
public:
    MfRuntime();
    ~MfRuntime();
    MfRuntime(const MfRuntime &) = delete;
    MfRuntime &operator=(const MfRuntime &) = delete;
    bool ok() const { return m_ok; }

private:
    bool m_ok = false;
};

// Initializes COM for the calling thread (multithreaded apartment) for its
// lifetime. Harmless if the thread already initialized COM in another mode.
class ComThread {
public:
    ComThread();
    ~ComThread();
    ComThread(const ComThread &) = delete;
    ComThread &operator=(const ComThread &) = delete;

private:
    bool m_uninit = false;
};

QString hresultText(HRESULT hr);

// True if `id` contains the ShadowCast's USB vendor/product ID in the form
// Windows uses in device paths ("vid_298f&pid_1996"), case-insensitively.
bool isShadowCastPath(const QString &id);

} // namespace wincom
