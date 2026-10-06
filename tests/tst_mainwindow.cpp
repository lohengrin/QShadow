// Behaviour of the window chrome: fullscreen shows only the video, and the
// right-click menu mirrors the toolbar. Runs on the offscreen platform, which has
// no Vulkan support, so the window is created without a QVulkanInstance (the
// video surface stays inert; the chrome logic does not depend on it).

#include "mainwindow.h"

#include <QAction>
#include <QComboBox>
#include <QMenu>
#include <QSlider>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QtTest>

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
        QTimer::singleShot(200, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (menu) {
                for (QAction *a : menu->actions()) {
                    top << a->text();
                    if (a->text() == QLatin1String("Shader") && a->menu()) {
                        for (QAction *s : a->menu()->actions())
                            shaders << s->text();
                    }
                    if (a->text() == QLatin1String("Fit window to video"))
                        fitEnabled = a->isEnabled();
                }
                menu->close();
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

        QTimer::singleShot(200, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu)
                return;
            for (QAction *a : menu->actions()) {
                if (a->text() == QLatin1String("Shader") && a->menu()) {
                    for (QAction *s : a->menu()->actions()) {
                        if (s->text() == QLatin1String("CRT-Geom"))
                            s->trigger();
                    }
                }
            }
            menu->close();
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
        QTimer::singleShot(200, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu)
                return;
            for (QAction *a : menu->actions()) {
                if (a->text() == QLatin1String("Fit window to video"))
                    fitEnabled = a->isEnabled();
                if (a->text().startsWith(QLatin1String("Fullscreen")))
                    fullChecked = a->isChecked();
            }
            menu->close();
        });
        QMetaObject::invokeMethod(&w, "showContextMenu", Q_ARG(QPoint, QPoint(50, 50)));
        QVERIFY(!fitEnabled);
        QVERIFY(fullChecked);
    }

private:
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
