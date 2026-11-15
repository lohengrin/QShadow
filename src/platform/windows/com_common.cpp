// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#include "com_common.h"

#include "device.h"

#include <mfapi.h>

namespace wincom {

MfRuntime::MfRuntime()
{
    m_ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET));
}

MfRuntime::~MfRuntime()
{
    if (m_ok)
        MFShutdown();
}

ComThread::ComThread()
{
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    m_uninit = SUCCEEDED(hr); // RPC_E_CHANGED_MODE: leave the existing mode alone
}

ComThread::~ComThread()
{
    if (m_uninit)
        CoUninitialize();
}

QString hresultText(HRESULT hr)
{
    return QStringLiteral("0x%1").arg(quint32(hr), 8, 16, QLatin1Char('0'));
}

bool isShadowCastPath(const QString &id)
{
    const QString needle = QStringLiteral("vid_%1&pid_%2")
                               .arg(device::kUsbVendorId, 4, 16, QLatin1Char('0'))
                               .arg(device::kUsbProductId, 4, 16, QLatin1Char('0'));
    return id.contains(needle, Qt::CaseInsensitive);
}

} // namespace wincom
