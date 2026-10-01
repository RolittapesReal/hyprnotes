#include "native_adapter.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>

namespace hn::app {

namespace {
// NoteBridge -> mods::DocumentBridge. Native plugins hold the "native" permission, so no further gating happens here.
class DocAdapter : public hn::mods::DocumentBridge {
public:
    explicit DocAdapter(hn::plugins::NoteBridge *n) : m_n(n) {}
    QString selectionText() override { return m_n ? m_n->selection() : QString(); }
    void beginTransaction(const QString &name) override { if (m_n) m_n->beginTransaction(name); }
    void replaceSelection(const QString &s) override { if (m_n) m_n->replaceSelection(s); }
    void insertText(const QString &s) override { if (m_n) m_n->insert(s); }
    void endTransaction() override { if (m_n) m_n->endTransaction(); }
private:
    hn::plugins::NoteBridge *m_n;
};
}  // namespace

ModNativeAdapter::~ModNativeAdapter() = default;

bool ModNativeAdapter::activate(const hn::plugins::Manifest &m, QString *err) {
    auto fail = [&](const QString &why) { if (err) *err = why; return false; };
    if (m_hosts.count(m.id)) return true;
    const QFileInfo lib(m.dir + QLatin1Char('/') + m.entry);
    const QString canonDir = QFileInfo(m.dir).canonicalFilePath();
    if (!lib.isFile() || !lib.canonicalFilePath().startsWith(canonDir + QLatin1Char('/'))) return fail(QStringLiteral("native library '%1' not found inside the plugin folder").arg(m.entry));
    // Optional mod.json supplies activation, entry symbol and static command declarations.
    QJsonObject mod;
    if (QFile f(m.dir + QStringLiteral("/mod.json")); f.open(QIODevice::ReadOnly)) mod = QJsonDocument::fromJson(f.read(1 << 20)).object();
    const QString root = m_state + QStringLiteral("/native-stage"), stage = root + QLatin1Char('/') + m.id;
    QDir(stage).removeRecursively();
    if (!QDir().mkpath(stage)) return fail(QStringLiteral("cannot create %1").arg(stage));
    const QString libName = QStringLiteral("plugin.so");
    if (!QFile::copy(lib.absoluteFilePath(), stage + QLatin1Char('/') + libName)) return fail(QStringLiteral("cannot stage the native library"));
    QJsonObject mj{{"id", m.id}, {"name", m.name}, {"version", m.version}, {"host_api_version", 1}, {"arch", QSysInfo::buildCpuArchitecture()},
                   {"library", libName}, {"entry", mod.value(QLatin1String("entry")).toString(QStringLiteral("hn_mod_entry"))},
                   {"activation", mod.contains(QLatin1String("activation")) ? mod.value(QLatin1String("activation")) : QJsonValue(QJsonArray{QStringLiteral("on-startup")})}};
    if (mod.contains(QLatin1String("commands"))) mj["commands"] = mod.value(QLatin1String("commands"));
    QFile out(stage + QStringLiteral("/mod.json"));
    if (!out.open(QIODevice::WriteOnly)) return fail(QStringLiteral("cannot stage mod.json"));
    out.write(QJsonDocument(mj).toJson());
    out.close();
    const QString enabled = stage + QStringLiteral("/enabled.json");   // enabled.json is a file, not a mod dir: ignored by the scan
    if (!hn::mods::saveEnabled(enabled, {m.id})) return fail(QStringLiteral("cannot write the staged enabled list"));
    auto host = std::make_unique<hn::mods::ModHost>();
    const QString id = m.id;
    host->setLogger([id](int, const QString &, const QString &msg) { qWarning("[native plugin %s] %s", qPrintable(id), qPrintable(msg)); });
    host->start(root, enabled);
    for (const auto &e : host->errors())
        if (e.modId == m.id) return fail(e.message);   // manifest/arch/ELF/entry problems are reported against this id
    m_hosts[m.id] = std::move(host);
    return true;
}

void ModNativeAdapter::deactivate(const QString &id) {
    auto it = m_hosts.find(id);
    if (it == m_hosts.end()) return;
    it->second->shutdown();
    m_hosts.erase(it);
    QDir(m_state + QStringLiteral("/native-stage/") + id).removeRecursively();
}

bool ModNativeAdapter::runCommand(const QString &id, const QString &cmd, hn::plugins::NoteBridge *note, QString *err) {
    auto it = m_hosts.find(id);
    if (it == m_hosts.end()) { if (err) *err = QStringLiteral("native plugin '%1' is not active").arg(id); return false; }
    DocAdapter doc(note);
    if (!it->second->runCommand(cmd, &doc)) {
        if (err) { *err = QStringLiteral("native command '%1' failed").arg(cmd); for (const auto &e : it->second->errors()) if (e.modId == id) *err = e.message; }
        return false;
    }
    return true;
}

void ModNativeAdapter::postEvent(const QString &id, const QString &event, const QString &arg) {
    auto it = m_hosts.find(id);
    if (it == m_hosts.end()) return;
    using hn::mods::EventType;
    if (event == QLatin1String("note.opened")) it->second->post(EventType::NoteOpened, arg);
    else if (event == QLatin1String("note.saved")) it->second->post(EventType::NoteSaved, arg);
    else if (event == QLatin1String("note.closed")) it->second->post(EventType::NoteClosed, arg);
    else if (event == QLatin1String("selection.changed")) it->second->post(EventType::SelectionChanged, arg);
}

QList<hn::plugins::CommandReg> ModNativeAdapter::commands() const {
    QList<hn::plugins::CommandReg> out;
    for (const auto &[id, h] : m_hosts)
        for (const auto &c : h->commands()) out.append({id, c.id, c.title, QString()});
    return out;
}

bool ModNativeAdapter::isLoaded(const QString &id) const {
    auto it = m_hosts.find(id);
    return it != m_hosts.end() && it->second->isLoaded(id);
}

} // namespace hn::app
