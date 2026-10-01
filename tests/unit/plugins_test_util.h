#pragma once
#include "hn/plugins/runtime.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
#include <archive.h>
#include <archive_entry.h>

namespace hn::plugins::test {

// Isolated data/state dirs via the same env overrides the app uses.
struct TmpEnv {
    QTemporaryDir tmp;
    TmpEnv() {
        qputenv("HN_DATA_DIR", (tmp.path() + "/data").toUtf8());
        qputenv("HN_STATE_DIR", (tmp.path() + "/state").toUtf8());
    }
    QString plugins() const { return tmp.path() + "/data/plugins"; }
    QString state() const { return tmp.path() + "/state"; }
    QString src() const { return tmp.path() + "/src"; }
};

inline QString fixture(const QString &name) { return QStringLiteral(HN_PLUGIN_FIXTURES) + "/" + name; }

inline bool writeFile(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}
inline QByteArray readFile(const QString &path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }

inline QByteArray manifestJson(const QString &id, const QStringList &perms = {}, const QStringList &hosts = {}, const QString &tier = "script", const QString &entry = "main.lua") {
    QJsonObject o{{"id", id}, {"name", "Test " + id}, {"version", "1.0.0"}, {"author", "tests"}, {"description", "test plugin"},
                  {"api", 1}, {"tier", tier}, {"entry", entry}, {"permissions", QJsonArray::fromStringList(perms)}, {"min_app", "0.1.0"}};
    if (!hosts.isEmpty()) o["net_hosts"] = QJsonArray::fromStringList(hosts);
    return QJsonDocument(o).toJson();
}
// Writes <root>/<id>/{plugin.json,main.lua}
inline QString writePlugin(const QString &root, const QString &id, const QStringList &perms, const QByteArray &lua, const QStringList &hosts = {}) {
    const QString dir = root + "/" + id;
    writeFile(dir + "/plugin.json", manifestJson(id, perms, hosts));
    writeFile(dir + "/main.lua", lua);
    return dir;
}
inline bool copyTree(const QString &from, const QString &to) {
    QDir().mkpath(to);
    for (const auto &fi : QDir(from).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
        if (fi.isDir()) { if (!copyTree(fi.filePath(), to + "/" + fi.fileName())) return false; }
        else if (!QFile::copy(fi.filePath(), to + "/" + fi.fileName())) return false;
    }
    return true;
}

// ---- archive builder (for malicious packages) ----
struct AEntry {
    QString name;
    QByteArray data;
    int type = AE_IFREG;  // AE_IFREG, AE_IFDIR, AE_IFLNK, AE_IFCHR
    QString linkTarget = {};
    bool hardlink = false;
};
inline bool makeArchive(const QString &path, const QList<AEntry> &entries, bool zip = true) {
    struct archive *a = archive_write_new();
    if (zip) archive_write_set_format_zip(a); else archive_write_set_format_pax_restricted(a);
    if (archive_write_open_filename(a, path.toLocal8Bit().constData()) != ARCHIVE_OK) { archive_write_free(a); return false; }
    for (const auto &e : entries) {
        archive_entry *x = archive_entry_new();
        archive_entry_set_pathname(x, e.name.toUtf8().constData());
        archive_entry_set_filetype(x, e.type);
        archive_entry_set_perm(x, e.type == AE_IFDIR ? 0755 : 0644);
        archive_entry_set_mtime(x, 1700000000, 0);
        if (e.type == AE_IFLNK) archive_entry_set_symlink(x, e.linkTarget.toUtf8().constData());
        if (e.hardlink) archive_entry_set_hardlink(x, e.linkTarget.toUtf8().constData());
        archive_entry_set_size(x, e.type == AE_IFREG && !e.hardlink ? e.data.size() : 0);
        archive_write_header(a, x);
        if (e.type == AE_IFREG && !e.hardlink && !e.data.isEmpty()) archive_write_data(a, e.data.constData(), size_t(e.data.size()));
        archive_entry_free(x);
    }
    archive_write_close(a);
    archive_write_free(a);
    return true;
}
inline QList<AEntry> goodEntries(const QString &id = "arch", const QByteArray &lua = "hn.command{id='x',title='X',run=function() end}\n") {
    return {{"plugin.json", manifestJson(id, {"ui"}), AE_IFREG, {}, false}, {"main.lua", lua, AE_IFREG, {}, false}};
}

// ---- fake host bridges ----
struct FakeNote : NoteBridge {
    QString t = "hello world", sel = "world", p = "notes/a.md", ti = "A";
    QStringList tg{"x", "y"}, ops;
    int begins = 0, ends = 0;
    QString text() override { return t; }
    QString selection() override { return sel; }
    QString path() override { return p; }
    QString title() override { return ti; }
    QStringList tags() override { return tg; }
    void beginTransaction(const QString &) override { ++begins; }
    void replaceSelection(const QString &s) override { ops << "replace:" + s; t.replace(sel, s); }
    void insert(const QString &s) override { ops << "insert:" + s; t += s; }
    void setText(const QString &s) override { ops << "set:" + s.left(40); t = s; }
    void endTransaction() override { ++ends; }
};
struct FakeLib : LibraryBridge {
    QStringList calls;
    QList<NoteInfo> list(const QString &q) override { calls << "list:" + q; return {{"a.md", "A"}, {"b.md", "B"}}; }
    bool read(const QString &p, QString *t) override { calls << "read:" + p; *t = "body of " + p; return true; }
    QString create(const QString &ti, const QString &) override { calls << "create:" + ti; return "new.md"; }
    bool write(const QString &p, const QString &) override { calls << "write:" + p; return true; }
    bool remove(const QString &p) override { calls << "delete:" + p; return true; }
};
struct FakeUi : UiBridge {
    QStringList notes;
    int prompts = 0;
    void notify(const QString &, const QString &m) override { notes << m; }
    std::optional<QString> prompt(const QString &, const QString &, const QString &, const QString &d) override { ++prompts; return d + "!"; }
    bool confirm(const QString &, const QString &) override { return true; }
    int pick(const QString &, const QString &, const QStringList &) override { return 1; }
};
struct FakeNet : NetBridge {
    QList<HttpRequest> reqs;
    int status = 200;
    QByteArray body = "ok";
    QString error;
    HttpResponse perform(const HttpRequest &r) override { reqs << r; return {status, body, error}; }
};
struct FakeClip : ClipboardBridge {
    QString v = "clip";
    QString get() override { return v; }
    void set(const QString &s) override { v = s; }
};
struct FakeTheme : ThemeBridge {
    QStringList sets;
    bool setToken(const QString &, const QString &t, const QString &v) override { sets << t + "=" + v; return true; }
};

// A manager wired to fakes, with every hn.log line captured.
struct Rig {
    TmpEnv env;
    FakeNote note;
    FakeLib lib;
    FakeUi ui;
    FakeNet net;
    FakeClip clip;
    FakeTheme theme;
    QStringList logs;
    std::unique_ptr<PluginManager> mgr;
    Rig() {
        ManagerConfig c;
        c.bridges = {&lib, &ui, &net, &clip, &theme};
        c.logger = [this](int, const QString &id, const QString &m) { logs << id + ": " + m; };
        mgr = std::make_unique<PluginManager>(c);
        mgr->scan();
    }
    // install + consent (all or given perms) + enable
    bool setup(const QString &srcDir, QStringList consent = {}, bool all = true, QString *err = nullptr) {
        auto r = mgr->install(srcDir, true);
        if (!r.ok) { if (err) *err = r.errors.value(0).message; return false; }
        if (all) consent = mgr->info(r.id).manifest.permissions;
        return mgr->consent(r.id, consent, err) && mgr->enable(r.id, err);
    }
    bool addLua(const QString &id, const QStringList &perms, const QByteArray &lua, const QStringList &hosts = {}, QString *err = nullptr) {
        return setup(writePlugin(env.src(), id, perms, lua, hosts), {}, true, err);
    }
    QStringList logsOf(const QString &id) const {
        QStringList r;
        for (const auto &l : logs) if (l.startsWith(id + ": ")) r << l.mid(id.size() + 2);
        return r;
    }
};

inline qint64 pssKb() {
    QFile f("/proc/self/smaps_rollup");
    if (!f.open(QIODevice::ReadOnly)) return -1;
    for (const auto &l : f.readAll().split('\n'))
        if (l.startsWith("Pss:")) return l.mid(4).trimmed().split(' ').first().toLongLong();
    return -1;
}

}  // namespace hn::plugins::test
