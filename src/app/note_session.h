#pragma once
// One open note: owns its NoteEditor + FormattingToolbar, the save state machine, autosave timers
// and the conflict / failure / removal states (spec 4.3, 6.2, 6.3). Hosts (sticky, organizer) only
// place the widgets; ownership of content, cursor and history never leaves the session.
#include "hn/core/note_repository.h"
#include "hn/editor/formatting_toolbar.h"
#include "hn/editor/note_editor.h"
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <functional>
#include <memory>

namespace hn::app {

class NoteSession : public QObject {
    Q_OBJECT
public:
    enum class State { Clean, Dirty, Saving, Failed, Conflict, Removed };
    struct Timing { int idleMs = 750; int maxMs = 5000; };   // spec 6.2

    NoteSession(hn::core::NoteRepository *repo, QString rel, QObject *parent = nullptr);
    ~NoteSession() override;

    bool load(QString *err);                          // read from disk into the editor
    void loadDraft(const QByteArray &localBytes);     // crash recovery: disk baseline + local content, dirty

    QString rel() const { return m_rel; }
    void setRel(const QString &rel) { m_rel = rel; emit titleChanged(); }
    QString title() const { return m_title; }
    QStringList tags() const { return m_tags; }
    QString folder() const;
    hn::editor::NoteEditor *editor() const { return m_editor.get(); }
    hn::editor::FormattingToolbar *toolbar() const { return m_toolbar.get(); }

    State state() const { return m_state; }
    QString stateLabel() const;                       // "Saved", "Saving", ...
    QString detail() const;                           // error / conflict explanation, may be empty
    bool dirty() const { return m_editor->isModified() || m_forceDirty; }
    bool settled() const { return m_state == State::Clean; }
    QByteArray externalBytes() const { return m_external; }

    // Additive: plugin note.pre_save. Gets the bytes about to be written and returns the bytes to write (same on any failure).
    void setPreSaveHook(std::function<QByteArray(const QByteArray &)> h) { m_preSave = std::move(h); }
    void setTiming(Timing t) { m_timing = t; }
    Timing timing() const { return m_timing; }

    void flush();                   // save now if dirty
    void requestClose();            // flush; closeReady() when clean, closeRefused() when it cannot be
    void discard();                 // explicit user discard: drop local work and the recovery copy

    // resolve actions
    void retry();
    void reload();                  // take the disk version, drop local edits
    bool saveLocalCopy(QString *copyRel = nullptr);   // local text -> "<name> (local copy).md", then reload
    void replaceDisk();             // overwrite the disk version with local text
    void recreate();                // deleted-on-disk note: write it again

    int cursorPosition() const;
    int scrollValue() const;
    void restoreView(int cursor, int scroll);
    void applyTheme(const hn::theme::Theme &t);

signals:
    void statusChanged();
    void titleChanged();
    void closeReady();
    void closeRefused();
    void savedOk(const QString &rel);
    void copyCreated(const QString &rel);
    void removedWhileClean(const QString &rel);
    void contentEdited();

private:
    struct Job { int editorRev = 0; QByteArray bytes; };
    void saveNow(bool force = false);
    void recompute();
    void onContent();
    void onSaved(const QString &rel, quint64 rev);
    void onFailed(const QString &rel, const QString &err, quint64 rev);
    void onConflict(const QString &rel, const QByteArray &external, quint64 rev);
    void onExternal(const QString &rel);
    void onRemoved(const QString &rel);
    void updateMeta(const QByteArray &bytes);
    void stopTimers() { m_idle.stop(); m_max.stop(); }
    void dropJobsUpTo(quint64 rev);

    hn::core::NoteRepository *m_repo;
    std::function<QByteArray(const QByteArray &)> m_preSave;
    QString m_rel, m_title;
    QStringList m_tags;
    std::unique_ptr<hn::editor::NoteEditor> m_editor;
    std::unique_ptr<hn::editor::FormattingToolbar> m_toolbar;
    QTimer m_idle, m_max;
    Timing m_timing;
    QHash<quint64, Job> m_jobs;       // repository revision -> snapshot in flight or queued
    quint64 m_baseRev = 0;
    State m_state = State::Clean;
    QString m_failMsg, m_lastDetail;
    QByteArray m_external;
    bool m_conflict = false, m_removed = false, m_forceDirty = false, m_closing = false, m_loading = false;
};

} // namespace hn::app
