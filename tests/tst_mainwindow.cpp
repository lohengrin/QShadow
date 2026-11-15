// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

// Behaviour of the window chrome: fullscreen shows only the video, and the
// right-click menu mirrors the toolbar. Runs on the offscreen platform, which has
// no Vulkan support, so the window is created without a QVulkanInstance (the
// video surface stays inert; the chrome logic does not depend on it).

#include "mainwindow.h"

#include <QAction>
#include <QComboBox>
#include <QLayout>
#include <QMenu>
#include <QSlider>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QtTest>

#include <functional>
#include <memory>

class TstMainWindow : public QObject {
    Q_OBJECT

private slots:
    void fullscreenHidesEverythingButTheVideo()
    {
        MainWindow w(nullptr);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto *toolbar = w.findChild<QToolBar *>();
        QVERIFY(toolbar);
        QVERIFY(toolbar->isVisible());
        QVERIFY(w.statusBar()->isVisible());

        w.showFullScreen();
        QTRY_VERIFY(w.isFullScreen());
        QTRY_VERIFY(!toolbar->isVisible());
        QVERIFY(!w.statusBar()->isVisible());
        QVERIFY(fullscreenAction(w)->isChecked());

        w.showNormal();
        QTRY_VERIFY(!w.isFullScreen());
        QTRY_VERIFY(toolbar->isVisible());
        QVERIFY(w.statusBar()->isVisible());
        QVERIFY(!fullscreenAction(w)->isChecked());
    }

    // Regression: with the chrome hidden the video must not cover every pixel of the
    // fullscreen window, or the compositor treats the window as occluded and the
    // embedded video surface stops rendering (the image freezes on Wayland).
    void fullscreenLeavesAStripOfTheWindowUncovered()
    {
        MainWindow w(nullptr);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QWidget *central = w.centralWidget();
        QCOMPARE(central->layout()->contentsMargins().bottom(), 0);
        QVERIFY(!central->autoFillBackground());

        w.showFullScreen();
        QTRY_VERIFY(w.isFullScreen());
        QTRY_COMPARE(central->layout()->contentsMargins().bottom(), 1);
        QVERIFY(central->autoFillBackground());
        QCOMPARE(central->palette().color(QPalette::Window), QColor(Qt::black));

        w.showNormal();
        QTRY_VERIFY(!w.isFullScreen());
        QTRY_COMPARE(central->layout()->contentsMargins().bottom(), 0);
        QVERIFY(!central->autoFillBackground());
    }

    void f11AndEscapeToggleFullscreenEvenWithTheToolbarHidden()
    {
        MainWindow w(nullptr);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));

        QTest::keyClick(&w, Qt::Key_F11);
        QTRY_VERIFY(w.isFullScreen());
        QVERIFY(!w.findChild<QToolBar *>()->isVisible());
        QTest::keyClick(&w, Qt::Key_Escape); // leaves fullscreen
        QTRY_VERIFY(!w.isFullScreen());
        QTest::keyClick(&w, Qt::Key_F11);
        QTRY_VERIFY(w.isFullScreen());
        QTest::keyClick(&w, Qt::Key_F11); // F11 works while the toolbar is hidden
        QTRY_VERIFY(!w.isFullScreen());
    }

    void contextMenuMirrorsTheToolbar()
    {
        MainWindow w(nullptr);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));

        QStringList top;
        QStringList shaders;
        bool fitEnabled = false;
        withPopupMenu(this, [&](QMenu *menu) {
            for (QAction *a : menu->actions()) {
                top << a->text();
                if (a->text() == QLatin1String("Shader") && a->menu()) {
                    for (QAction *s : a->menu()->actions())
                        shaders << s->text();
                }
                if (a->text() == QLatin1String("Fit window to video"))
                    fitEnabled = a->isEnabled();
            }
        });
        QMetaObject::invokeMethod(&w, "showContextMenu", Q_ARG(QPoint, QPoint(50, 50)));

        for (const char *expected : {"Preserve aspect", "Format", "Resolution", "Shader",
                                     "Downscale", "Volume", "Fit window to video"})
            QVERIFY2(top.contains(QLatin1String(expected)), expected);
        QVERIFY(top.filter(QStringLiteral("Fullscreen")).size() == 1);
        QVERIFY(fitEnabled); // windowed
        // The Shader submenu lists exactly what the toolbar's combo box does.
        QComboBox *shader = nullptr;
        for (QComboBox *c : w.findChildren<QComboBox *>()) {
            if (c->findText(QStringLiteral("CRT-Geom")) >= 0)
                shader = c;
        }
        QVERIFY(shader);
        QCOMPARE(shaders.size(), shader->count());
        QCOMPARE(shaders.value(0), shader->itemText(0));
    }

    void choosingAMenuEntryActsLikeThePickingItInTheToolbar()
    {
        MainWindow w(nullptr);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QComboBox *shader = nullptr;
        for (QComboBox *c : w.findChildren<QComboBox *>()) {
            if (c->findText(QStringLiteral("CRT-Geom")) >= 0)
                shader = c;
        }
        QVERIFY(shader);
        const int target = shader->findText(QStringLiteral("CRT-Geom"));
        QVERIFY(shader->currentIndex() != target);

        withPopupMenu(this, [&](QMenu *menu) {
            for (QAction *a : menu->actions()) {
                if (a->text() == QLatin1String("Shader") && a->menu()) {
                    for (QAction *s : a->menu()->actions()) {
                        if (s->text() == QLatin1String("CRT-Geom"))
                            s->trigger();
                    }
                }
            }
        });
        QMetaObject::invokeMethod(&w, "showContextMenu", Q_ARG(QPoint, QPoint(50, 50)));
        QCOMPARE(shader->currentIndex(), target);
    }

    void fitIsDisabledInFullscreenMenu()
    {
        MainWindow w(nullptr);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.showFullScreen();
        QTRY_VERIFY(w.isFullScreen());

        bool fitEnabled = true;
        bool fullChecked = false;
        withPopupMenu(this, [&](QMenu *menu) {
            for (QAction *a : menu->actions()) {
                if (a->text() == QLatin1String("Fit window to video"))
                    fitEnabled = a->isEnabled();
                if (a->text().startsWith(QLatin1String("Fullscreen")))
                    fullChecked = a->isChecked();
            }
        });
        QMetaObject::invokeMethod(&w, "showContextMenu", Q_ARG(QPoint, QPoint(50, 50)));
        QVERIFY(!fitEnabled);
        QVERIFY(fullChecked);
    }

private:
    // showContextMenu() blocks in QMenu::exec(), so the menu has to be inspected and
    // closed from a timer. Poll until it is up (how fast a popup becomes the active
    // one varies by platform and CI machine) instead of checking once after a fixed
    // delay, which left exec() blocked forever whenever the menu was not ready yet.
    static void withPopupMenu(QObject *context, std::function<void(QMenu *)> inspect)
    {
        auto *timer = new QTimer(context);
        auto attempts = std::make_shared<int>(0);
        QObject::connect(timer, &QTimer::timeout, context, [timer, attempts, inspect] {
            QMenu *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) {
                for (QWidget *w : QApplication::topLevelWidgets()) {
                    auto *m = qobject_cast<QMenu *>(w);
                    if (m && m->isVisible()) {
                        menu = m;
                        break;
                    }
                }
            }
            if (menu) {
                inspect(menu);
                menu->close();
            } else if (++*attempts < 100) { // ~5 s
                return;
            }
            timer->stop();
            timer->deleteLater();
        });
        timer->start(50);
    }

    static QAction *fullscreenAction(MainWindow &w)
    {
        for (QAction *a : w.actions()) {
            if (a->text() == QLatin1String("Fullscreen"))
                return a;
        }
        return nullptr;
    }

};

QTEST_MAIN(TstMainWindow)
#include "tst_mainwindow.moc"
