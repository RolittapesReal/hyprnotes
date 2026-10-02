#include "hn/plugins/store.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <lua.hpp>

namespace hn::plugins {
namespace {

struct Walk { QStringList files; QList<PluginError> errs; };

void walkTree(const QString &root, const QString &rel, int depth, Walk &w, const QString &who) {
    if (depth > 8) { w.errs.append({who, QStringLiteral("folders are nested deeper than 8 levels")}); return; }
    const QDir d(rel.isEmpty() ? root : root + QLatin1Char('/') + rel);
    for (const QFileInfo &fi : d.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name)) {
        const QString name = fi.fileName();
        if (name.startsWith(QLatin1Char('.'))) continue;  // hidden entries are ignored everywhere
        const QString r = rel.isEmpty() ? name : rel + QLatin1Char('/') + name;
        if (fi.isSymLink()) w.errs.append({who, QStringLiteral("'%1' is a symbolic link (links are not allowed)").arg(r)});
        else if (fi.isDir()) walkTree(root, r, depth + 1, w, who);
        else if (fi.isFile()) w.files << r;
        else w.errs.append({who, QStringLiteral("'%1' is a special file (device, socket or pipe)").arg(r)});
        if (w.files.size() > kMaxFiles) {
            w.errs.append({who, QStringLiteral("more than %1 files").arg(kMaxFiles)});
            return;
        }
    }
}

QString luaSyntaxError(const QByteArray &src, const QString &chunk) {
    lua_State *L = luaL_newstate();
    if (!L) return QStringLiteral("out of memory");
    QString e;
    if (luaL_loadbufferx(L, src.constData(), size_t(src.size()), ("@" + chunk.toUtf8()).constData(), "t") != LUA_OK)
        e = QString::fromUtf8(lua_tostring(L, -1));
    lua_close(L);
    return e;
}

bool validUtf8(const QByteArray &b) {
    QStringDecoder dec(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    (void)QString(dec(b));  // decoding is lazy: force it so errors are recorded
    return !dec.hasError();
}

bool looksNative(const QString &rel, const QByteArray &head) {
    static const QRegularExpression ext(QStringLiteral("\\.(so|dll|dylib|exe|node|a|o|ko|sys)(\\.[0-9.]+)?$"), QRegularExpression::CaseInsensitiveOption);
    if (ext.match(rel).hasMatch()) return true;
    if (head.startsWith("\x7f""ELF") || head.startsWith("MZ") || head.startsWith("\xfe\xed\xfa\xce") || head.startsWith("\xfe\xed\xfa\xcf") ||
        head.startsWith("\xce\xfa\xed\xfe") || head.startsWith("\xcf\xfa\xed\xfe") || head.startsWith("\xca\xfe\xba\xbe") || head.startsWith("#!"))
        return true;
    return false;
}

}  // namespace

QString hashDirectory(const QString &dir, QString *err) {
    Walk w;
    walkTree(dir, QString(), 0, w, QString());
    if (!w.errs.isEmpty()) { if (err) *err = w.errs.first().message; return {}; }
    std::sort(w.files.begin(), w.files.end());
    QCryptographicHash h(QCryptographicHash::Sha256);
    for (const auto &r : w.files) {
        QFile f(dir + QLatin1Char('/') + r);
        if (!f.open(QIODevice::ReadOnly)) { if (err) *err = QStringLiteral("cannot read '%1'").arg(r); return {}; }
        h.addData(r.toUtf8());
        h.addData(QByteArrayView("\0", 1));
        h.addData(QByteArray::number(f.size()));
        h.addData(QByteArrayView("\0", 1));
        h.addData(&f);
    }
    return QString::fromLatin1(h.result().toHex());
}

CheckResult checkDirectory(const QString &dirIn, const QString &appVersion) {
    CheckResult r;
    const QString dir = QDir::cleanPath(QFileInfo(dirIn).absoluteFilePath());
    const QString who = QFileInfo(dir).fileName();
    auto err = [&](const QString &m) { r.errors.append({who, m}); };
    if (!QFileInfo(dir).isDir()) { err(QStringLiteral("not a folder: %1").arg(dirIn)); return r; }
    QFile pj(dir + QStringLiteral("/plugin.json"));
    bool manifestOk = false;
    if (!pj.open(QIODevice::ReadOnly)) err(QStringLiteral("plugin.json is missing"));
    else manifestOk = parseManifest(pj.read(64 * 1024 + 1), dir, &r.manifest, &r.errors, appVersion);
    Walk w;
    walkTree(dir, QString(), 0, w, who);
    r.errors += w.errs;
    std::sort(w.files.begin(), w.files.end());
    r.fileCount = int(w.files.size());
    const bool native = manifestOk && r.manifest.tier == Tier::Native;
    for (const auto &rel : w.files) {
        const QFileInfo fi(dir + QLatin1Char('/') + rel);
        if (rel.contains(QLatin1Char('\\')) || rel.contains(QStringLiteral(".."))) { err(QStringLiteral("'%1' has an unsafe file name").arg(rel)); continue; }
        r.totalBytes += fi.size();
        if (fi.size() > (native ? kMaxNativeFileBytes : kMaxScriptFileBytes)) {
            err(QStringLiteral("'%1' is larger than %2 MiB").arg(rel).arg((native ? kMaxNativeFileBytes : kMaxScriptFileBytes) >> 20));
            continue;
        }
        QFile f(fi.filePath());
        if (!f.open(QIODevice::ReadOnly)) { err(QStringLiteral("cannot read '%1'").arg(rel)); continue; }
        const bool isLua = rel.endsWith(QLatin1String(".lua"));
        const QByteArray data = (isLua || rel == QLatin1String("plugin.json")) ? f.readAll() : f.read(16);
        if (!native && looksNative(rel, data.left(16))) err(QStringLiteral("'%1' looks like a native library or executable; script plugins cannot contain native code").arg(rel));
        if (isLua) {
            if (data.startsWith("\x1b""Lua")) err(QStringLiteral("'%1' is precompiled Lua bytecode, which is not allowed").arg(rel));
            else if (data.contains('\0') || !validUtf8(data)) err(QStringLiteral("'%1' is not valid UTF-8 text").arg(rel));
        }
    }
    if (r.totalBytes > kMaxTotalBytes) err(QStringLiteral("package is larger than %1 MiB unpacked").arg(kMaxTotalBytes >> 20));
    if (manifestOk) {
        if (!w.files.contains(r.manifest.entry)) err(QStringLiteral("entry file '%1' does not exist").arg(r.manifest.entry));
        for (const auto &p : r.manifest.permissions)
            if (isDangerousPermission(p)) r.warnings << QStringLiteral("requests dangerous permission '%1': %2").arg(p, permissionDescription(p));
        if (r.manifest.tier == Tier::Script && r.errors.isEmpty()) {
            for (const auto &rel : w.files) {
                if (!rel.endsWith(QLatin1String(".lua"))) continue;
                QFile f(dir + QLatin1Char('/') + rel);
                if (f.open(QIODevice::ReadOnly))
                    if (const QString e = luaSyntaxError(f.readAll(), rel); !e.isEmpty()) err(QStringLiteral("Lua syntax error: %1").arg(e));
            }
        }
    }
    if (r.errors.isEmpty()) {
        QString he;
        r.hash = hashDirectory(dir, &he);
        if (r.hash.isEmpty()) err(he);
    }
    r.ok = r.errors.isEmpty();
    return r;
}

namespace {

bool badArchivePath(const QString &p) {
    if (p.isEmpty() || p.startsWith(QLatin1Char('/')) || p.contains(QLatin1Char('\\')) || p.contains(QChar(0))) return true;
    if (p.size() > 1 && p[1] == QLatin1Char(':')) return true;
    for (QChar c : p) if (c.unicode() < 0x20) return true;
    const auto parts = p.split(QLatin1Char('/'));
    for (int i = 0; i < parts.size(); ++i) {
        if (parts[i] == QLatin1String("..")) return true;
        if (parts[i].isEmpty() && i != parts.size() - 1) return true;  // "a//b"
    }
    return false;
}

// Extracts regular files and directories only, by hand (never via archive_write_disk), with every cap enforced while reading.
bool extractArchive(const QString &file, const QString &dest, QList<PluginError> *errs, const QString &who) {
    auto fail = [&](const QString &m) { errs->append({who, m}); return false; };
    if (QFileInfo(file).size() > kMaxArchiveBytes) return fail(QStringLiteral("archive is larger than %1 MiB").arg(kMaxArchiveBytes >> 20));
    QDir().mkpath(dest);
    const QString canonDest = QFileInfo(dest).canonicalFilePath();
    struct archive *a = archive_read_new();
    archive_read_support_format_zip(a);
    archive_read_support_format_tar(a);
    archive_read_support_filter_gzip(a);
    archive_read_support_filter_xz(a);
    archive_read_support_filter_bzip2(a);
    struct Guard { archive *a; ~Guard() { archive_read_free(a); } } guard{a};
    if (archive_read_open_filename(a, file.toLocal8Bit().constData(), 16384) != ARCHIVE_OK)
        return fail(QStringLiteral("cannot read archive: %1").arg(QString::fromUtf8(archive_error_string(a))));
    QSet<QString> seen;
    qint64 total = 0;
    int entries = 0;
    archive_entry *e;
    int rc;
    while ((rc = archive_read_next_header(a, &e)) == ARCHIVE_OK || rc == ARCHIVE_WARN) {
        if (++entries > kMaxFiles * 2) return fail(QStringLiteral("archive has too many entries (limit %1 files)").arg(kMaxFiles));
        const char *pn = archive_entry_pathname_utf8(e);
        if (!pn) pn = archive_entry_pathname(e);
        QString path = QString::fromUtf8(pn ? pn : "");
        const auto type = archive_entry_filetype(e);
        if (archive_entry_hardlink(e) || archive_entry_symlink(e))
            return fail(QStringLiteral("archive entry '%1' is a link (links are not allowed)").arg(path));
        if (type != AE_IFREG && type != AE_IFDIR)
            return fail(QStringLiteral("archive entry '%1' is not a regular file or folder (devices/pipes are not allowed)").arg(path));
        while (path.startsWith(QStringLiteral("./"))) path.remove(0, 2);
        if (path.isEmpty() || path == QLatin1String(".")) continue;
        if (badArchivePath(path)) return fail(QStringLiteral("archive entry '%1' has an unsafe path (absolute or contains '..')").arg(path));
        if (path.endsWith(QLatin1Char('/'))) path.chop(1);
        bool hidden = false;
        for (const auto &c : path.split(QLatin1Char('/'))) hidden |= c.startsWith(QLatin1Char('.'));
        if (hidden) continue;
        if (seen.contains(path)) return fail(QStringLiteral("archive contains '%1' twice").arg(path));
        seen << path;
        const QString out = dest + QLatin1Char('/') + path;
        if (type == AE_IFDIR) { QDir().mkpath(out); continue; }
        if (archive_entry_size(e) > kMaxNativeFileBytes) return fail(QStringLiteral("'%1' is larger than %2 MiB").arg(path).arg(kMaxNativeFileBytes >> 20));
        QDir().mkpath(QFileInfo(out).absolutePath());
        const QString canonParent = QFileInfo(QFileInfo(out).absolutePath()).canonicalFilePath();
        if (canonParent != canonDest && !canonParent.startsWith(canonDest + QLatin1Char('/'))) return fail(QStringLiteral("'%1' escapes the install folder").arg(path));
        QFile f(out);
        if (QFileInfo(out).exists() || !f.open(QIODevice::WriteOnly)) return fail(QStringLiteral("cannot create '%1' (conflicting entry)").arg(path));
        char buf[16384];
        qint64 fileBytes = 0;
        for (;;) {
            const la_ssize_t n = archive_read_data(a, buf, sizeof buf);
            if (n == 0) break;
            if (n < 0) return fail(QStringLiteral("archive is corrupt: %1").arg(QString::fromUtf8(archive_error_string(a))));
            fileBytes += n;
            total += n;
            if (fileBytes > kMaxNativeFileBytes || total > kMaxTotalBytes) return fail(QStringLiteral("archive expands to more than %1 MiB").arg(kMaxTotalBytes >> 20));
            f.write(buf, n);
        }
    }
    if (rc != ARCHIVE_EOF) return fail(QStringLiteral("archive is corrupt: %1").arg(QString::fromUtf8(archive_error_string(a))));
    return true;
}

}  // namespace

PackageInstaller::PackageInstaller(QString d, TrustStore *t, AuditLog *a, QString v)
    : dir_(std::move(d)), appVersion_(std::move(v)), trust_(t), audit_(a) {}

InstallResult PackageInstaller::install(const QString &source, bool allowUpgrade) {
    InstallResult r;
    const QString who = QFileInfo(source).fileName();
    auto fail = [&](const QString &m) { r.ok = false; r.errors.append({r.id.isEmpty() ? who : r.id, m}); return r; };
    const QFileInfo si(source);
    if (!si.exists()) return fail(QStringLiteral("source not found: %1").arg(source));
    if (!QDir().mkpath(dir_)) return fail(QStringLiteral("cannot create %1").arg(dir_));
    QTemporaryDir stage(dir_ + QStringLiteral("/.staging-XXXXXX"));
    if (!stage.isValid()) return fail(QStringLiteral("cannot create a staging folder in %1").arg(dir_));
    const QString content = stage.path() + QStringLiteral("/content");
    if (si.isDir()) {
        const auto pre = checkDirectory(source, appVersion_);
        if (!pre.ok) { r.errors = pre.errors; return r; }
        Walk w;
        walkTree(source, QString(), 0, w, who);
        for (const auto &rel : w.files) {
            const QString to = content + QLatin1Char('/') + rel;
            QDir().mkpath(QFileInfo(to).absolutePath());
            if (!QFile::copy(source + QLatin1Char('/') + rel, to)) return fail(QStringLiteral("cannot copy '%1'").arg(rel));
        }
        QDir().mkpath(content);
    } else if (!extractArchive(source, content, &r.errors, who)) {
        return r;
    }
    QString root = content;
    if (!QFileInfo::exists(root + QStringLiteral("/plugin.json"))) {  // tolerate "archive of the folder itself"
        const auto subs = QDir(content).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        if (subs.size() == 1 && QFileInfo::exists(subs[0].filePath() + QStringLiteral("/plugin.json")) && QDir(content).entryInfoList(QDir::Files).isEmpty())
            root = subs[0].filePath();
    }
    const CheckResult c = checkDirectory(root, appVersion_);
    if (!c.ok) { r.errors = c.errors; return r; }
    r.id = c.manifest.id;
    r.hash = c.hash;
    const QString target = dir_ + QLatin1Char('/') + r.id;
    const bool existed = QFileInfo(target).exists() || QFileInfo(target).isSymLink();
    if (existed) {
        if (!allowUpgrade) return fail(QStringLiteral("plugin '%1' is already installed; upgrade it explicitly or uninstall it first").arg(r.id));
        QFile old(target + QStringLiteral("/plugin.json"));
        Manifest im;
        QList<PluginError> ignore;
        const bool readable = !QFileInfo(target).isSymLink() && old.open(QIODevice::ReadOnly) && parseManifest(old.read(64 * 1024 + 1), target, &im, &ignore, QStringLiteral("999.0.0"));
        if (!readable || im.id != r.id)
            return fail(QStringLiteral("refusing to overwrite the installed folder '%1': it does not contain plugin '%1'").arg(r.id));
        const QString bak = dir_ + QStringLiteral("/.old-") + QUuid::createUuid().toString(QUuid::Id128);
        if (!QDir().rename(target, bak)) return fail(QStringLiteral("cannot replace the installed plugin"));
        if (!QDir().rename(root, target)) {
            QDir().rename(bak, target);
            return fail(QStringLiteral("cannot move the new version into place"));
        }
        QDir(bak).removeRecursively();
        r.upgraded = true;
    } else if (!QDir().rename(root, target)) {
        return fail(QStringLiteral("cannot move the plugin into place"));
    }
    if (trust_) {
        if (r.upgraded) {
            const auto before = trust_->record(r.id);
            trust_->verifyHash(r.id, r.hash);
            const auto after = trust_->record(r.id);
            r.consentKept = before.hasConsent && !after.needsReconsent && after.hash == r.hash;
        } else {
            trust_->remove(r.id);
        }
    }
    if (audit_) audit_->log(r.upgraded ? QStringLiteral("upgrade") : QStringLiteral("install"), r.id,
                            QStringLiteral("version %1 sha256 %2 from %3").arg(c.manifest.version, r.hash, QDir::cleanPath(si.absoluteFilePath())));
    r.ok = true;
    return r;
}

bool PackageInstaller::uninstall(const QString &id, QString *err) {
    auto fail = [&](const QString &m) { if (err) *err = m; return false; };
    if (!validPluginId(id)) return fail(QStringLiteral("invalid plugin id"));
    const QString target = dir_ + QLatin1Char('/') + id;
    if (!QFileInfo(target).isDir() || QFileInfo(target).isSymLink()) return fail(QStringLiteral("plugin '%1' is not installed").arg(id));
    if (!QDir(target).removeRecursively()) return fail(QStringLiteral("could not remove all files of '%1'").arg(id));
    if (trust_) trust_->remove(id);
    if (audit_) audit_->log(QStringLiteral("remove"), id);
    return true;
}

bool packDirectory(const QString &dirIn, const QString &outFile, QList<PluginError> *errs) {
    const QString dir = QDir::cleanPath(QFileInfo(dirIn).absoluteFilePath());
    auto fail = [&](const QString &m) { if (errs) errs->append({QFileInfo(dir).fileName(), m}); return false; };
    const CheckResult c = checkDirectory(dir);
    if (!c.ok) { if (errs) *errs += c.errors; return false; }
    const QString out = QDir::cleanPath(QFileInfo(outFile).absoluteFilePath());
    if (out.startsWith(dir + QLatin1Char('/'))) return fail(QStringLiteral("the output file must be outside the plugin folder"));
    Walk w;
    walkTree(dir, QString(), 0, w, QString());
    std::sort(w.files.begin(), w.files.end());
    struct archive *a = archive_write_new();
    archive_write_set_format_zip(a);
    if (archive_write_open_filename(a, out.toLocal8Bit().constData()) != ARCHIVE_OK) {
        const QString m = QString::fromUtf8(archive_error_string(a));
        archive_write_free(a);
        return fail(QStringLiteral("cannot write %1: %2").arg(out, m));
    }
    bool ok = true;
    for (const auto &rel : w.files) {
        QFile f(dir + QLatin1Char('/') + rel);
        if (!f.open(QIODevice::ReadOnly)) { ok = false; break; }
        const QByteArray data = f.readAll();
        archive_entry *e = archive_entry_new();
        archive_entry_set_pathname_utf8(e, rel.toUtf8().constData());
        archive_entry_set_size(e, data.size());
        archive_entry_set_filetype(e, AE_IFREG);
        archive_entry_set_perm(e, 0644);
        archive_entry_set_mtime(e, 1700000000, 0);  // deterministic archives
        ok = archive_write_header(a, e) == ARCHIVE_OK && archive_write_data(a, data.constData(), size_t(data.size())) == data.size();
        archive_entry_free(e);
        if (!ok) break;
    }
    ok = (archive_write_close(a) == ARCHIVE_OK) && ok;
    archive_write_free(a);
    if (!ok) { QFile::remove(out); return fail(QStringLiteral("writing the archive failed")); }
    if (QFileInfo(out).size() > kMaxArchiveBytes) { QFile::remove(out); return fail(QStringLiteral("archive would exceed %1 MiB").arg(kMaxArchiveBytes >> 20)); }
    return true;
}

bool createTemplate(const QString &dir, const QString &name, QString *err, int api) {
    auto fail = [&](const QString &m) { if (err) *err = m; return false; };
    QString id = name.toLower();
    id.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    id = id.left(64);
    while (id.startsWith(QLatin1Char('-'))) id.remove(0, 1);
    while (id.endsWith(QLatin1Char('-'))) id.chop(1);
    if (id.isEmpty() || !validPluginId(id)) return fail(QStringLiteral("'%1' cannot be turned into a plugin id; use letters and digits").arg(name));
    if (api < kApiVersion || api > kApiVersionMax) return fail(QStringLiteral("unsupported plugin API %1 (use %2 to %3)").arg(api).arg(kApiVersion).arg(kApiVersionMax));
    if (QFileInfo::exists(dir) && !QDir(dir).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden))
        return fail(QStringLiteral("%1 already exists and is not empty").arg(dir));
    if (!QDir().mkpath(dir)) return fail(QStringLiteral("cannot create %1").arg(dir));
    const QJsonObject m{{"id", id}, {"name", name.left(80)}, {"version", "0.1.0"}, {"author", "Your Name"},
                        {"description", "Describe what this plugin does."}, {"api", api}, {"tier", "script"},
                        {"entry", "main.lua"},
                        {"permissions", api >= 2 ? QJsonArray{"note.read", "notes.read", "notes.index", "ui.panel"} : QJsonArray{"note.read", "note.edit", "ui"}},
                        {"min_app", "0.1.0"}};
    const QByteArray lua2 = QByteArrayLiteral(
        "-- Hyprnotes plugin (API 2). Edit this file, then press Reload on the Plugins page; no restart needed.\n"
        "-- API 2 adds the note index (hn.notes.links/backlinks/resolve/frontmatter/query), side panels (hn.panel),\n"
        "-- completion popups (hn.complete) and [[link]] handlers (hn.link_handler). Each needs its own permission in plugin.json.\n"
        "-- A panel is described, not drawn: render() returns blocks and the app draws them. Budget: 50 ms per render.\n"
        "\n"
        "hn.panel{\n"
        "  id = \"backlinks\",\n"
        "  title = \"Backlinks\",\n"
        "  icon = \"link\",\n"
        "  refresh_on = { \"note.opened\", \"note.saved\" },        -- re-render when these happen (only while the panel is visible)\n"
        "  render = function(ctx)\n"
        "    if not ctx.path then return { { type = \"empty\", text = \"No note open\" } } end   -- ctx.path needs note.read\n"
        "    local rows = hn.notes.backlinks(ctx.path, { limit = 50 })                         -- permission: notes.index\n"
        "    if not rows then return { { type = \"empty\", text = \"The index is not available\" } } end\n"
        "    if #rows == 0 then return { { type = \"empty\", text = \"Nothing links here yet\" } } end\n"
        "    local items = {}\n"
        "    for i, row in ipairs(rows) do\n"
        "      items[i] = {\n"
        "        type = \"item\", title = row.src, subtitle = row.context, path = row.src, line = row.line,\n"
        "        on_click = function() hn.notes.open(row.src) end,                              -- permission: notes.read\n"
        "      }\n"
        "    end\n"
        "    return { { type = \"heading\", text = #rows .. \" backlinks\" }, { type = \"list\", items = items } }\n"
        "  end,\n"
        "}\n");
    const QByteArray lua = api >= 2 ? lua2 : QByteArrayLiteral(
        "-- Hyprnotes plugin (API 1). Edit this file, then press Reload on the Plugins page; no restart needed.\n"
        "-- Scripts run in a sandbox: no io, os, debug or package. You get the standard string, table, math, utf8\n"
        "-- and coroutine libraries plus the `hn` API. Each callback has a time budget (events 50 ms, commands 2 s).\n"
        "-- Every hn.* call needs the matching permission in plugin.json (\"permissions\"); anything else raises an error.\n"
        "\n"
        "-- A command appears in the command palette. `run` is called when the user runs it.\n"
        "hn.command{\n"
        "  id = \"shout\",\n"
        "  title = \"Shout selection\",\n"
        "  run = function()\n"
        "    local sel = hn.note.selection()            -- permission: note.read\n"
        "    if sel == \"\" then\n"
        "      hn.ui.notify(\"Select some text first\")   -- permission: ui\n"
        "      return\n"
        "    end\n"
        "    hn.note.replace_selection(sel:upper())     -- permission: note.edit (one undo step)\n"
        "  end,\n"
        "}\n"
        "\n"
        "-- More things to try:\n"
        "--   hn.on(\"note.saved\", function(path) hn.log(\"saved \" .. path) end)\n"
        "--   hn.trigger{ pattern = \"::hi\", replace = function() return \"Hello!\" end }\n"
        "--   hn.toolbar_button{ id = \"b\", title = \"Shout\", run = function() end }\n"
        "--   hn.storage.set(\"count\", 1)   -- permission: storage (1 MiB per plugin)\n"
        "-- Want panels, link completion or the note index? Set \"api\": 2 in plugin.json (see docs/plugin-api.md).\n");
    QFile pj(dir + QStringLiteral("/plugin.json")), ml(dir + QStringLiteral("/main.lua"));
    if (!pj.open(QIODevice::WriteOnly) || !ml.open(QIODevice::WriteOnly)) return fail(QStringLiteral("cannot write files in %1").arg(dir));
    pj.write(QJsonDocument(m).toJson(QJsonDocument::Indented));
    ml.write(lua);
    return true;
}

}  // namespace hn::plugins
