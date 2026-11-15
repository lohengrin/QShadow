// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#include "audio.h"

#include <QAudio>
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>

namespace {

constexpr int kOutputRate = 48000;

} // namespace

bool AudioSink::open(AudioRing *ring, QString *error)
{
    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull()) {
        if (error)
            *error = QStringLiteral("No default audio output device");
        return false;
    }

    QAudioFormat format;
    format.setSampleRate(kOutputRate);
    format.setChannelCount(2);
    format.setSampleFormat(QAudioFormat::Int16);

    bool floatOutput = false;
    if (!device.isFormatSupported(format)) {
        floatOutput = true;
        format.setSampleFormat(QAudioFormat::Float);
        if (!device.isFormatSupported(format)) {
            if (error)
                *error = QStringLiteral("Host output rejects 48 kHz stereo S16/F32");
            return false;
        }
    }

    m_host = new HostAudioSink(ring, floatOutput);
    if (!m_host->open(QIODevice::ReadOnly)) {
        delete m_host;
        m_host = nullptr;
        if (error)
            *error = QStringLiteral("Failed to open pull device");
        return false;
    }

    m_sink = new QAudioSink(device, format);
    m_sink->setBufferSize(int(double(kOutputRate) * 2 * (floatOutput ? 4 : 2) * 0.04)); // ~40 ms
    m_sink->setVolume(m_volume);
    m_sink->start(m_host);

    const QAudio::State st = m_sink->state();
    m_active = (st == QAudio::ActiveState || st == QAudio::IdleState);
    if (!m_active) {
        if (error)
            *error = QStringLiteral("Audio output failed to start (error %1)")
                         .arg(int(m_sink->error()));
        close();
        return false;
    }
    return true;
}

void AudioSink::setVolume(qreal volume)
{
    m_volume = qBound<qreal>(0.0, volume, 1.0);
    if (m_sink)
        m_sink->setVolume(m_volume);
}

void AudioSink::close()
{
    if (m_sink) {
        if (m_sink->state() != QAudio::StoppedState)
            m_sink->stop();
        delete m_sink;
        m_sink = nullptr;
    }
    if (m_host) {
        m_host->close();
        delete m_host;
        m_host = nullptr;
    }
    m_active = false;
}
