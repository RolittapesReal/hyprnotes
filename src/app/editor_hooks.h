#pragma once
// Plugin API v2, editor side: per open note, the wiki-link overlay, the link resolver (LibraryIndex, cached), link activation and the
// completion popup, routed to PluginManager. Everything stays off (no overlay, no triggers, no connections) unless an enabled
// plugin has editor.links or editor.complete.
#include "hn/plugins/types.h"
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTimer>

namespace hn::app {

class AppController;
class NoteSession;
class PluginService;

class EditorHooks : public QObject, public hn::plugins::EditorHooksBridge {
    Q_OBJECT
public:
    explicit EditorHooks(PluginService *svc);
    bool wanted() const;                       // some enabled plugin has editor.links or editor.complete
    void apply(NoteSession *s);                // enable or disable this note's hooks to match the plugins that are enabled now
    void forget(NoteSession *s);               // the session is going away
    void completionReply(quint64 token, const QList<hn::plugins::CompletionItem> &items) override;
    void linkActivationReply(quint64 token, bool handled) override;
    // observability for tests
    int pendingCompletions() const { return int(m_completions.size()); }
    int pendingActivations() const { return int(m_links.size()); }
    int resolverCalls() const { return m_resolverCalls; }
    int cachedStates() const { return int(m_cache.size()); }
    bool hooked(NoteSession *s) const { return m_conns.contains(s); }
private:
    struct PendingCompletion { QPointer<NoteSession> s; int generation = 0; };
    int stateFor(NoteSession *s, const QString &target, QString *resolvedPath);   // hn::editor::LinkState as int
    void invalidate();
    void connectIndex();
    PluginService *m_svc;
    QHash<NoteSession *, QList<QMetaObject::Connection>> m_conns;
    QHash<quint64, PendingCompletion> m_completions;
    QHash<quint64, QPointer<NoteSession>> m_links;
    QHash<QString, QPair<int, QString>> m_cache;   // "<from>\n<target>" -> (state, path)
    QTimer m_inval;
    quint64 m_next = 0;
    int m_resolverCalls = 0;
    bool m_indexConnected = false;
};

}  // namespace hn::app
