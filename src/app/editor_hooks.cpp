#include "editor_hooks.h"
#include "controller.h"
#include "plugin_service.h"
#include "hn/editor/note_editor.h"

using namespace hn::plugins;
using hn::editor::LinkState;

namespace hn::app {

EditorHooks::EditorHooks(PluginService *svc) : QObject(svc), m_svc(svc) {
    m_inval.setSingleShot(true);
    m_inval.setInterval(60);   // coalesces bursts of index updates into one repaint
    connect(&m_inval, &QTimer::timeout, this, [this] {
        m_cache.clear();
        for (auto it = m_conns.begin(); it != m_conns.end(); ++it) if (it.key() && it.key()->editor()) it.key()->editor()->invalidateLinkStates();
    });
}

bool EditorHooks::wanted() const {
    auto *mgr = m_svc->managerIfActive();
    if (!mgr) return false;
    for (const auto &i : mgr->plugins())
        if (i.status == Status::Enabled && (i.manifest.has(QStringLiteral("editor.links")) || i.manifest.has(QStringLiteral("editor.complete")))) return true;
    return false;
}

void EditorHooks::invalidate() { m_inval.start(); }

void EditorHooks::connectIndex() {
    if (m_indexConnected) return;
    m_indexConnected = true;
    auto *c = m_svc->controller();
    connect(c, &AppController::notesChanged, this, [this] { invalidate(); });
    connect(c, &AppController::noteRenamed, this, [this] { invalidate(); });
    if (auto *idx = c->index()) {
        connect(idx, &hn::core::LibraryIndex::synced, this, [this] { invalidate(); });
        connect(idx, &hn::core::LibraryIndex::pathUpdated, this, [this] { invalidate(); });
    }
}

int EditorHooks::stateFor(NoteSession *s, const QString &target, QString *resolvedPath) {
    const QString key = (s ? s->rel() : QString()) + QLatin1Char('\n') + target;
    if (auto it = m_cache.constFind(key); it != m_cache.constEnd()) { if (resolvedPath) *resolvedPath = it->second; return it->first; }
    ++m_resolverCalls;
    ResolveResult rr;
    const auto st = m_svc->library() ? m_svc->library()->resolveFrom(target, s ? s->rel() : QString(), &rr) : BridgeStatus::Unsupported;
    if (st != BridgeStatus::Ok) {   // index busy or unavailable: show it as resolved (neutral), do not cache, look again shortly
        invalidate();
        if (resolvedPath) resolvedPath->clear();
        return int(LinkState::Resolved);
    }
    const int state = rr.status == QLatin1String("resolved") ? int(LinkState::Resolved) : rr.status == QLatin1String("ambiguous") ? int(LinkState::Ambiguous) : int(LinkState::Unresolved);
    m_cache.insert(key, {state, rr.path});
    if (resolvedPath) *resolvedPath = rr.path;
    return state;
}

void EditorHooks::apply(NoteSession *s) {
    if (!s || !s->editor()) return;
    auto *ed = s->editor();
    auto *mgr = m_svc->managerIfActive();
    const bool on = mgr && wanted();
    if (!on) {
        if (m_conns.contains(s)) {
            for (const auto &c : m_conns.take(s)) disconnect(c);
            ed->setWikiLinksEnabled(false);
            ed->setLinkResolver({});
            ed->setCompletionTriggers({});
            ed->dismissCompletions();
        }
        return;
    }
    connectIndex();
    QStringList triggers;
    for (const auto &c : mgr->registry()->completions()) if (!triggers.contains(c.trigger)) triggers << c.trigger;
    ed->setCompletionTriggers(triggers);
    if (m_conns.contains(s)) { invalidate(); return; }   // already wired: only the trigger list can have changed
    QPointer<NoteSession> sp(s);
    ed->setLinkResolver([this, sp](const QString &target) { return LinkState(stateFor(sp.data(), target, nullptr)); });
    ed->setWikiLinksEnabled(true);
    QList<QMetaObject::Connection> cs;
    cs << connect(ed, &hn::editor::NoteEditor::completionRequested, this, [this, sp](const hn::editor::CompletionRequest &r) {
        if (!sp || !m_svc->managerIfActive()) return;
        for (auto it = m_completions.begin(); it != m_completions.end();) it = it->s == sp ? m_completions.erase(it) : std::next(it);   // a newer request supersedes
        const quint64 tok = ++m_next;
        m_completions.insert(tok, {sp, r.generation});
        m_svc->managerIfActive()->requestCompletion(tok, r.triggerId, r.query, m_svc->bridgeFor(sp.data()), r.atLineStart);
    });
    cs << connect(ed, &hn::editor::NoteEditor::completionDismissed, this, [this, sp] {
        for (auto it = m_completions.begin(); it != m_completions.end();) it = it->s == sp ? m_completions.erase(it) : std::next(it);
    });
    cs << connect(ed, &hn::editor::NoteEditor::wikiLinkActivated, this, [this, sp](const hn::editor::LinkRefInfo &l) {
        if (!sp || !m_svc->managerIfActive()) return;
        LinkActivation ref;
        ref.kind = l.kind == hn::editor::LinkRefInfo::Kind::Embed ? QStringLiteral("embed") : QStringLiteral("link");
        ref.target = l.target; ref.alias = l.alias; ref.anchor = l.anchor;
        QString path;
        if (stateFor(sp.data(), l.target, &path) != int(LinkState::Resolved)) path.clear();
        ref.resolved = path;
        const quint64 tok = ++m_next;
        m_links.insert(tok, sp);
        m_svc->managerIfActive()->requestLinkActivation(tok, ref, m_svc->bridgeFor(sp.data()));
        if (m_links.contains(tok)) m_links.remove(tok);   // a manager that replied later would find nothing: nothing destructive happens
    });
    m_conns.insert(s, cs);
}

void EditorHooks::forget(NoteSession *s) {
    for (const auto &c : m_conns.take(s)) disconnect(c);
    for (auto it = m_completions.begin(); it != m_completions.end();) it = (!it->s || it->s == s) ? m_completions.erase(it) : std::next(it);
    for (auto it = m_links.begin(); it != m_links.end();) it = (!it.value() || it.value() == s) ? m_links.erase(it) : std::next(it);
}

void EditorHooks::completionReply(quint64 token, const QList<CompletionItem> &items) {
    const auto it = m_completions.constFind(token);
    if (it == m_completions.constEnd()) return;   // superseded, dismissed or the note is gone
    const PendingCompletion p = *it;
    m_completions.erase(it);
    if (!p.s || !p.s->editor()) return;
    QList<hn::editor::CompletionItem> out;
    for (const auto &i : items) out.append({i.label, i.detail, i.insert, i.cursorOffset, i.markdown});
    p.s->editor()->showCompletions(out, p.generation);   // the editor drops it too when a newer query has started
}

void EditorHooks::linkActivationReply(quint64 token, bool handled) {
    const auto it = m_links.constFind(token);
    if (it == m_links.constEnd()) return;
    m_links.erase(it);
    if (!handled) m_svc->controller()->announce(tr("No plugin handled that link."));
}

}  // namespace hn::app
