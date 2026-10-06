#include "mainwindow.h"

#include <QApplication>
#include <QByteArrayList>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QVulkanInstance>

#ifdef Q_OS_WIN
#include <cstdio>
#include <windows.h>
#endif

int main(int argc, char *argv[])
{
#ifdef Q_OS_LINUX
    // Linux is Wayland-only; prefer the wayland QPA plugin when a Wayland display
    // is available so we never touch the X11 stack.
    if (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "wayland");
#endif
#ifdef Q_OS_WIN
    // A GUI-subsystem executable has no console; reattach the parent's (unless
    // stderr is already redirected) so that QSHADOW_STATS and warnings show up
    // when started from a terminal.
    const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if ((!err || err == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE *unused = nullptr;
        freopen_s(&unused, "CONOUT$", "w", stderr);
        freopen_s(&unused, "CONOUT$", "w", stdout);
    }
    // Qt's default handler only reaches the debugger for a GUI-subsystem exe.
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &context, const QString &msg) {
        const QByteArray line = qFormatLogMessage(type, context, msg).toLocal8Bit();
        std::fputs(line.constData(), stderr);
        std::fputc('\n', stderr);
        std::fflush(stderr);
    });
#endif

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("qshadow"));
    QApplication::setOrganizationName(QStringLiteral("QShadow"));

    QVulkanInstance instance;
    if (!qEnvironmentVariableIsEmpty("QSHADOW_VULKAN_VALIDATION")) {
        for (const auto &layer : instance.supportedLayers()) {
            if (layer.name == QByteArrayLiteral("VK_LAYER_KHRONOS_validation")) {
                instance.setLayers({QByteArrayLiteral("VK_LAYER_KHRONOS_validation")});
                break;
            }
        }
    }
    if (!instance.create()) {
        QMessageBox::critical(nullptr, QStringLiteral("QShadow"),
                              QStringLiteral("Failed to initialize Vulkan. Ensure a "
                                             "working Vulkan driver is installed."));
        return 1;
    }

    MainWindow window(&instance);
    window.resize(1280, 720);
    window.show();

    return app.exec();
}