#include "mainwindow.h"

#include <QApplication>
#include <QMessageBox>

int main(int argc, char *argv[])
{
    // Wayland-only target; prefer the wayland QPA plugin when a Wayland display
    // is available so we never touch the X11 stack.
    if (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "wayland");

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("qshadow"));
    QApplication::setOrganizationName(QStringLiteral("QShadow"));

    MainWindow window;
    window.resize(1280, 720);
    window.show();

    return app.exec();
}