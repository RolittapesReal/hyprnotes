#pragma once
#include "hn/plugins/runtime.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
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

inline QByteArray manifestJson(const QString &id, const QStringList &perms = {}, const QStringList &hosts = {}, const QString &tier = "script", const QString &entry = "main.lua", int api = 1) {
    QJsonObject o{{"id", id}, {"name", "Test " + id}, {"version", "1.0.0"}, {"author", "tests"}, {"description", "test plugin"},
                  {"api", api}, {"tier", tier}, {"entry", entry}, {"permissions", QJsonArray::fromStringList(perms)}, {"min_app", "0.1.0"}};
    if (!hosts.isEmpty()) o["net_hosts"] = QJsonArray::fromStringList(hosts);
    return QJsonDocument(o).toJson();
}
// Writes <root>/<id>/{plugin.json,main.lua}
inline QString writePlugin(const QString &root, const QString &id, const QStringList &perms, const QByteArray &lua, const QStringList &hosts = {}, int api = 1) {
    const QString dir = root + "/" + id;
    writeFile(dir + "/plugin.json", manifestJson(id, perms, hosts, "script", "main.lua", api));
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
// API 2: a library with a canned index. Records every call, can be forced to a status and made slow.
struct FakeIndexLib : FakeLib {
    QMap<QString, QList<LinkRow>> outgoing, incoming;
    QMap<QString, QJsonObject> fm;
    QMap<QString, ResolveResult> resolves;
    QJsonArray queryRows;
    QList<QJsonObject> querySpecs;
    QStringList opened, renamed;
    int indexCalls = 0, backlinkLimit = 0, backlinkOffset = 0;
    BridgeStatus forced = BridgeStatus::Ok;
    int delayMs = 0;
    FakeIndexLib() {
        LinkRow l1{"link", "b", "", "", "b.md", "", "see [[b]] for details", 3};
        LinkRow l2{"embed", "pic", "", "", "", "", "![[pic]]", 7};
        outgoing["notes/a.md"] = {l1, l2};
        incoming["notes/a.md"] = {{"link", "a", "A", "", "notes/a.md", "b.md", "links to [[a|A]]", 2}, {"link", "a", "", "intro", "notes/a.md", "c.md", "see [[a#intro]]", 9}};
        fm["notes/a.md"] = QJsonObject{{"title", "A"}, {"tags", QJsonArray{"x", "y"}}, {"status", "draft"}};
        resolves["b"] = {"resolved", "b.md", {}};
        resolves["dup"] = {"ambiguous", "", {"x/dup.md", "y/dup.md"}};
        resolves["nope"] = {"unresolved", "", {}};
        queryRows = QJsonArray{QJsonObject{{"path", "inbox/todo.md"}, {"line", 3}, {"text", "buy milk"}, {"done", false}},
                               QJsonObject{{"path", "work/plan.md"}, {"line", 12}, {"text", "write the report"}, {"done", false}}};
    }
    BridgeStatus gate() {
        ++indexCalls;
        if (delayMs > 0) QThread::msleep(ulong(delayMs));
        return forced;
    }
    BridgeStatus links(const QString &p, QList<LinkRow> *o) override { calls << "links:" + p; if (auto s = gate(); s != BridgeStatus::Ok) return s; if (!outgoing.contains(p)) return BridgeStatus::NotFound; *o = outgoing[p]; return BridgeStatus::Ok; }
    BridgeStatus backlinks(const QString &p, int limit, int offset, QList<LinkRow> *o) override {
        calls << "backlinks:" + p; backlinkLimit = limit; backlinkOffset = offset;
        if (auto s = gate(); s != BridgeStatus::Ok) return s;
        *o = incoming.value(p).mid(offset, limit);
        return BridgeStatus::Ok;
    }
    BridgeStatus resolve(const QString &n, ResolveResult *o) override { calls << "resolve:" + n; if (auto s = gate(); s != BridgeStatus::Ok) return s; *o = resolves.value(n, ResolveResult{"unresolved", "", {}}); return BridgeStatus::Ok; }
    BridgeStatus frontmatter(const QString &p, QJsonObject *o) override { calls << "frontmatter:" + p; if (auto s = gate(); s != BridgeStatus::Ok) return s; if (!fm.contains(p)) return BridgeStatus::NotFound; *o = fm[p]; return BridgeStatus::Ok; }
    BridgeStatus query(const QJsonObject &spec, QJsonArray *rows) override { calls << "query"; querySpecs << spec; if (auto s = gate(); s != BridgeStatus::Ok) return s; *rows = queryRows; return BridgeStatus::Ok; }
    BridgeStatus open(const QString &p, const QString &w) override { calls << "open:" + p + "@" + w; opened << p + "@" + w; return forced; }
    BridgeStatus rename(const QString &p, const QString &n, bool ul, QString *np) override {
        calls << "rename:" + p + "->" + n + (ul ? "+links" : ""); renamed << p + "->" + n + (ul ? "+links" : "");
        if (forced != BridgeStatus::Ok) return forced;
        *np = QFileInfo(p).path() == "." ? n : QFileInfo(p).path() + "/" + n;
        return BridgeStatus::Ok;
    }
};
struct FakePanel : PanelBridge {
    QStringList shown, removed;
    QMap<QString, QList<PanelBlock>> blocks;
    QMap<QString, QString> errors;
    int updates = 0;
    void showPanel(const QString &q, const QString &t, const QString &i) override { shown << q + "|" + t + "|" + i; }
    void updatePanel(const QString &q, const QList<PanelBlock> &b, const QString &e) override { ++updates; blocks[q] = b; errors[q] = e; }
    void removePanel(const QString &q) override { removed << q; }
};
struct FakeEditor : EditorHooksBridge {
    QList<QPair<quint64, QList<CompletionItem>>> completions;
    QList<QPair<quint64, bool>> links;
    void completionReply(quint64 t, const QList<CompletionItem> &i) override { completions << qMakePair(t, i); }
    void linkActivationReply(quint64 t, bool h) override { links << qMakePair(t, h); }
};

struct FakeUi : UiBridge {
    QStringList notes;
    int prompts = 0;
    QStringList nextPrompts;  // when non-empty, prompt() pops from the front instead of returning default + "!"
    int pickIndex = 1;        // host-side answer of pick(): 0-based index into the items, negative = cancel (Lua sees index + 1)
    void notify(const QString &, const QString &m) override { notes << m; }
    std::optional<QString> prompt(const QString &, const QString &, const QString &, const QString &d) override {
        ++prompts;
        if (!nextPrompts.isEmpty()) return nextPrompts.takeFirst();
        return d + "!";
    }
    bool confirmAnswer = true;  // what confirm() answers
    bool confirm(const QString &, const QString &) override { return confirmAnswer; }
    QStringList lastPickItems;  // items of the most recent pick()
    int pick(const QString &, const QString &, const QStringList &items) override { lastPickItems = items; return pickIndex; }
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
    FakeIndexLib lib;
    FakePanel panel;
    FakeEditor editor;
    FakeUi ui;
    FakeNet net;
    FakeClip clip;
    FakeTheme theme;
    QStringList logs;
    std::unique_ptr<PluginManager> mgr;
    Rig() {
        ManagerConfig c;
        c.bridges = {&lib, &ui, &net, &clip, &theme, &panel, &editor};
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
    bool addLua(const QString &id, const QStringList &perms, const QByteArray &lua, const QStringList &hosts = {}, QString *err = nullptr, int api = 1) {
        return setup(writePlugin(env.src(), id, perms, lua, hosts, api), {}, true, err);
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
