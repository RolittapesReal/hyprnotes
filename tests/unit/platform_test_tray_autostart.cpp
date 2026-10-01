#include "hn/platform/autostart.h"
#include "hn/platform/tray_controller.h"
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <unistd.h>
using namespace hn::platform;

class TrayAutostartTest : public QObject {
  Q_OBJECT
private slots:
  void trayMenuAndActivation() {
    TrayController t{QIcon()};
    QSignalSpy n(&t, &TrayController::newNoteRequested), s(&t, &TrayController::showOrganizerRequested), q(&t, &TrayController::quitRequested);
    int real = 0; for (auto* a : t.menu()->actions()) real += !a->isSeparator();
    QCOMPARE(real, 3);
    QCOMPARE(t.newNoteAction()->text(), QString("New Note"));
    QCOMPARE(t.showOrganizerAction()->text(), QString("Show Organizer"));
    QCOMPARE(t.quitAction()->text(), QString("Quit"));
    t.newNoteAction()->trigger(); t.showOrganizerAction()->trigger(); t.quitAction()->trigger();
    QCOMPARE(n.size(), 1); QCOMPARE(s.size(), 1); QCOMPARE(q.size(), 1);
  }
  void closePolicyFollowsEnabledAndAvailable() {
    TrayController t{QIcon()};
    QVERIFY(t.closeOrganizerPolicy() == TrayController::ClosePolicy::CloseNormally);   // disabled
    QSignalSpy av(&t, &TrayController::availabilityChanged);
    t.setEnabled(true);
    // offscreen has no StatusNotifier host: tray not usable => organizer must really close
    const bool usable = QSystemTrayIcon::isSystemTrayAvailable();
    QCOMPARE(t.available(), usable);
    QCOMPARE(t.closeOrganizerPolicy() == TrayController::ClosePolicy::HideToTray, usable);
    QCOMPARE(av.size(), usable ? 1 : 0);
    t.setEnabled(false);
    QVERIFY(t.closeOrganizerPolicy() == TrayController::ClosePolicy::CloseNormally);
  }
  void disablingWhileHiddenShowsOrganizerFirst() {
    TrayController t{QIcon()};
    t.setEnabled(true);
    bool enabledAtEmit = false, emitted = false;
    connect(&t, &TrayController::showOrganizerRequested, [&] { emitted = true; enabledAtEmit = t.enabled(); });
    t.setEnabled(false, /*organizerHidden*/ true);
    QVERIFY(emitted); QVERIFY(enabledAtEmit);    // emitted before the icon was removed
    emitted = false; t.setEnabled(true); t.setEnabled(false, false);
    QVERIFY(!emitted);
  }
  void autostartLifecycle() {
    QTemporaryDir d; Autostart a(d.filePath("cfg/autostart"));
    QCOMPARE(a.state(), Autostart::State::Off);                 // default off
    QVERIFY(a.disable().ok);
    auto r = a.enable(); QVERIFY(r.ok); QCOMPARE(a.state(), Autostart::State::EnabledManaged);
    QFile f(a.filePath()); QVERIFY(f.open(QIODevice::ReadOnly)); const QByteArray c = f.readAll();
    QVERIFY(c.contains("Exec=hyprnotes --background\n")); QVERIFY(c.contains("X-Hyprnotes-Managed=true"));
    QVERIFY(a.enable().ok);                                     // idempotent
    QVERIFY(a.disable().ok); QCOMPARE(a.state(), Autostart::State::Off); QVERIFY(!QFile::exists(a.filePath()));
  }
  void externalEntryPreserved() {
    QTemporaryDir d; Autostart a(d.path());
    QFile f(a.filePath()); QVERIFY(f.open(QIODevice::WriteOnly));
    const QByteArray mine = "[Desktop Entry]\nType=Application\nExec=hyprnotes --new-note\n"; f.write(mine); f.close();
    QCOMPARE(a.state(), Autostart::State::ExternallyManaged);
    auto r = a.enable(); QVERIFY(!r.ok); QCOMPARE(r.state, Autostart::State::ExternallyManaged); QVERIFY(r.message.contains("externally"));
    r = a.disable(); QVERIFY(!r.ok);
    QVERIFY(f.open(QIODevice::ReadOnly)); QCOMPARE(f.readAll(), mine);   // untouched
  }
  void unwritableDirReportsFailure() {
    if (::geteuid() == 0) QSKIP("root ignores permissions");
    QTemporaryDir d; QVERIFY(QFile::setPermissions(d.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    Autostart a(d.path() + "/autostart");
    auto r = a.enable(); QVERIFY(!r.ok); QVERIFY(!r.message.isEmpty()); QCOMPARE(a.state(), Autostart::State::Off);
    QFile::setPermissions(d.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  }
  void defaultDirHonoursXdg() {
    qputenv("XDG_CONFIG_HOME", "/x/cfg");
    QCOMPARE(Autostart::defaultDir(), QString("/x/cfg/autostart"));
    QCOMPARE(Autostart().filePath(), QString("/x/cfg/autostart/hyprnotes.desktop"));
  }
};
QTEST_MAIN(TrayAutostartTest)
#include "platform_test_tray_autostart.moc"
