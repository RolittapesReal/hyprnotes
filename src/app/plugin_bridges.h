#pragma once
// Real implementations of the plugin runtime's host bridges (hn/plugins/types.h) over the app's objects.
// The runtime enforces permissions and limits before it calls any of these; each bridge still validates its own inputs.
#include "hn/plugins/runtime.h"
#include "note_session.h"
#include <QPointer>
#include <functional>

class QNetworkAccessManager;

namespace hn::app {

class AppController;
class PluginService;

// The active note of one window. Each plugin callback is one begin/end pair = ONE undo step (a Format transaction over the
// whole document, so replace/insert/set_text in one callback undo together) and, since it goes through the normal edit
// path, it also schedules autosave. Safe against a dying session: detach() (or the QPointer) turns every call into a no-op.
class PluginNoteBridge : public hn::plugins::NoteBridge {
public:
    explicit PluginNoteBridge(NoteSession *s) : m_s(s) {}
    ~PluginNoteBridge() override { endTransaction(); }
    void detach() { endTransaction(); m_s = nullptr; }
    NoteSession *session() const { return m_s.data(); }
    QString text() override;
    QString selection() override;
    QString path() override { return m_s ? m_s->rel() : QString(); }
    QString title() override { return m_s ? m_s->title() : QString(); }
    QStringList tags() override { return m_s ? m_s->tags() : QStringList(); }
    void beginTransaction(const QString &name) override;
    void replaceSelection(const QString &s) override;
    void insert(const QString &s) override;
    void setText(const QString &s) override;
    void endTransaction() override { m_scope = hn::editor::EditRecorder::Scope(); }
private:
    hn::editor::NoteEditor *ed() const { return m_s ? m_s->editor() : nullptr; }
    QTextCursor cursor() const;
    void setCursor(const QTextCursor &c);
    QPointer<NoteSession> m_s;
    hn::editor::EditRecorder::Scope m_scope;
};

// list/read/create/write/delete over NoteRepository / LibraryIndex. Open notes are read from, and written through, their
// session (undoable, normal autosave, no conflict with the editor); closed notes use the repository's atomic save with its
// conflict detection (this call waits for the result, 5 s cap).
class PluginLibraryBridge : public hn::plugins::LibraryBridge {
public:
    explicit PluginLibraryBridge(AppController *c) : m_c(c) {}
    QList<hn::plugins::NoteInfo> list(const QString &query) override;
    bool read(const QString &path, QString *text) override;
    QString create(const QString &title, const QString &text) override;
    bool write(const QString &path, const QString &text) override;
    bool remove(const QString &path) override;
    static constexpr int kMaxList = 200;
    static constexpr qint64 kMaxText = 4 * 1024 * 1024;
private:
    bool validRel(const QString &rel) const;
    AppController *m_c;
};

// notify -> status strip of the active window; prompt/confirm/pick -> flat modernist dialogs (or the test hooks).
class PluginUiBridge : public hn::plugins::UiBridge {
public:
    explicit PluginUiBridge(PluginService *s) : m_s(s) {}
    void notify(const QString &pluginId, const QString &msg) override;
    std::optional<QString> prompt(const QString &pluginId, const QString &title, const QString &label, const QString &def) override;
    bool confirm(const QString &pluginId, const QString &msg) override;
    int pick(const QString &pluginId, const QString &title, const QStringList &items) override;
private:
    PluginService *m_s;
};

class PluginClipboardBridge : public hn::plugins::ClipboardBridge {
public:
    QString get() override;
    void set(const QString &s) override;
};

class PluginThemeBridge : public hn::plugins::ThemeBridge {
public:
    explicit PluginThemeBridge(AppController *c) : m_c(c) {}
    bool setToken(const QString &pluginId, const QString &token, const QString &value) override;
private:
    AppController *m_c;
};

// QNetworkAccessManager created on the FIRST request only. https only; every hop (initial URL and each redirect) is checked
// against the allowed hosts, a refused redirect ends the request with an error. Size cap, total timeout, at most 5 redirects,
// no cookie jar contents, no HTTP cache, nothing persisted.
class PluginNetBridge : public hn::plugins::NetBridge {
public:
    using Validator = std::function<bool(const QUrl &url, const QStringList &allowedHosts, QString *why)>;
    PluginNetBridge();
    ~PluginNetBridge() override;
    hn::plugins::HttpResponse perform(const hn::plugins::HttpRequest &req) override;
    bool managerCreated() const { return m_nam != nullptr; }
    static int managersCreated() { return s_created; }
    void setValidator(Validator v) { m_validate = std::move(v); }   // tests only (plain-http loopback server)
    static constexpr int kMaxRedirects = 5;
    // Pure check used for every hop.
    static bool defaultValidator(const QUrl &url, const QStringList &allowedHosts, QString *why);
private:
    QNetworkAccessManager *nam();
    QNetworkAccessManager *m_nam = nullptr;
    Validator m_validate;
    static inline int s_created = 0;
};

} // namespace hn::app
