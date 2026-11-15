// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

// Relating a camera's interfaces through the USB device they share, against a
// fake sysfs tree (Linux backend).

#include "platform/linux/sysfs_usb.h"

#include <QtTest>

namespace {

void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

} // namespace

class TstSysfs : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        const QString root = m_dir.path();
        // Two USB devices, each with a video and a sound interface; a PCI sound card.
        for (const auto &[usb, vid, pid] : {std::tuple{"usb1/1-1", "298f", "1996"},
                                            std::tuple{"usb1/1-2", "046d", "0825"}}) {
            const QString dev = root + "/devices/pci0/" + usb;
            writeFile(dev + "/idVendor", QByteArray(vid) + "\n");
            writeFile(dev + "/idProduct", QByteArray(pid) + "\n");
        }
        QDir().mkpath(root + "/devices/pci0/usb1/1-1/1-1:1.0/video4linux/video0");
        QDir().mkpath(root + "/devices/pci0/usb1/1-1/1-1:1.1/sound/card3");
        QDir().mkpath(root + "/devices/pci0/usb1/1-2/1-2:1.2/sound/card5");
        QDir().mkpath(root + "/devices/pci0/hda/sound/card0");
        QDir().mkpath(root + "/class/sound");
        QVERIFY(QFile::link(root + "/devices/pci0/usb1/1-1/1-1:1.1/sound/card3",
                            root + "/class/sound/card3"));
        QVERIFY(QFile::link(root + "/devices/pci0/usb1/1-2/1-2:1.2/sound/card5",
                            root + "/class/sound/card5"));
        QVERIFY(QFile::link(root + "/devices/pci0/hda/sound/card0", root + "/class/sound/card0"));
        // Not a bare cardN node: must be ignored.
        QVERIFY(QFile::link(root + "/devices/pci0/hda/sound/card0", root + "/class/sound/cardX"));
    }

    void usbDeviceDirIsTheAncestorWithIds()
    {
        const QString root = m_dir.path();
        const QString video = root + "/devices/pci0/usb1/1-1/1-1:1.0/video4linux/video0";
        QCOMPARE(sysfs_usb::usbDeviceDir(video), root + "/devices/pci0/usb1/1-1");
        QCOMPARE(sysfs_usb::usbDeviceDir(root + "/devices/pci0/hda/sound/card0"), QString());
    }

    void readsTheUsbIds()
    {
        int vid = 0, pid = 0;
        QVERIFY(sysfs_usb::readUsbIds(m_dir.path() + "/devices/pci0/usb1/1-1", &vid, &pid));
        QCOMPARE(vid, 0x298f);
        QCOMPARE(pid, 0x1996);
        QVERIFY(!sysfs_usb::readUsbIds(m_dir.path() + "/devices/pci0", &vid, &pid));
    }

    void pairsTheSoundCardOfTheSameUsbDevice()
    {
        const QString root = m_dir.path();
        QCOMPARE(sysfs_usb::findSoundCardOnUsbDevice(root + "/devices/pci0/usb1/1-1", root), 3);
        QCOMPARE(sysfs_usb::findSoundCardOnUsbDevice(root + "/devices/pci0/usb1/1-2", root), 5);
    }

    void noSoundCardForADeviceWithoutOneOrWithoutADirectory()
    {
        const QString root = m_dir.path();
        QDir().mkpath(root + "/devices/pci0/usb1/1-3");
        QCOMPARE(sysfs_usb::findSoundCardOnUsbDevice(root + "/devices/pci0/usb1/1-3", root), -1);
        QCOMPARE(sysfs_usb::findSoundCardOnUsbDevice(QString(), root), -1);
    }

private:
    QTemporaryDir m_dir;
};

QTEST_GUILESS_MAIN(TstSysfs)
#include "tst_sysfs.moc"
