#include "mainwindow.h"

#include <QApplication>
#include <QByteArrayList>
#include <QMessageBox>
#include <QVulkanInstance>

int main(int argc, char *argv[])
{
#ifdef Q_OS_LINUX
    // Linux is Wayland-only; prefer the wayland QPA plugin when a Wayland display
    // is available so we never touch the X11 stack.
    if (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "wayland");
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