// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#include "com_common.h"

#include <QRegularExpression>

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

bool parseVidPid(const QString &id, int *vid, int *pid)
{
    static const QRegularExpression re(QStringLiteral("vid_([0-9a-f]{4})&pid_([0-9a-f]{4})"),
                                       QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(id);
    if (!m.hasMatch())
        return false;
    *vid = m.captured(1).toInt(nullptr, 16);
    *pid = m.captured(2).toInt(nullptr, 16);
    return true;
}

} // namespace wincom
