#pragma once
#include "controller.h"
#include "editor_bridge.h"
#include "organizer_window.h"
#include "sticky_window.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <memory>

namespace apptest {
using namespace hn::app;

struct Lib {
    QTemporaryDir dir;
    QString notes = dir.filePath("notes"), state = dir.filePath("state"), cache = dir.filePath("cache"), cfg = dir.filePath("cfg/config.json");
    Lib() { QDir().mkpath(notes); }
    void write(const QString &rel, const QByteArray &bytes) {
        const QString p = notes + "/" + rel;
        QDir().mkpath(QFileInfo(p).absolutePath());
        QFile f(p);
        QVERIFY2(f.open(QIODevice::WriteOnly | QIODevice::Truncate), qPrintable(p));
        f.write(bytes);
    }
    QByteArray read(const QString &rel) const {
        QFile f(notes + "/" + rel);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<missing>");
    }
    ControllerOptions opts() const {
        ControllerOptions o;
        o.notesDir = notes; o.stateDir = state; o.cacheDir = cache; o.configPath = cfg;
        o.modsDir = dir.filePath("mods"); o.modsEnabledPath = dir.filePath("cfg/mods-enabled.json"); o.pluginsDir = dir.filePath("plugins");
        o.useTray = false; o.useHyprland = false;
        o.pluginHooks.consent = [](const ConsentRequest &) { return false; };   // never block a test on the modal consent dialog
        o.confirm = [](const QString &, const QString &) { return true; };
        o.recoveryPrompt = [](const QList<hn::core::RecoveryEntry> &) { return RecoveryChoice::Later; };
        return o;
    }
};

inline void typeText(NoteSession *s, const QString &t) { QTest::keyClicks(s->editor()->activeEdit(), t); }

// D15 invariant: every live session is shown in exactly one window (a sticky, or the organizer's editor).
inline bool sessionsMatchWindows(AppController &c) {
    QStringList shown;
    for (auto *w : c.stickies()) shown << w->session()->rel();
    if (c.organizer() && c.organizer()->currentSession()) shown << c.organizer()->currentSession()->rel();
    shown.sort();
    QStringList live = c.openNotes();
    live.sort();
    return shown == live;
}

} // namespace apptest
