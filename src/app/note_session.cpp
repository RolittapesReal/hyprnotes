#include "note_session.h"
#include "hn/core/fs_util.h"
#include "hn/core/library_index.h"
#include <QFileInfo>
#include <QScrollBar>
#include <QTextCursor>

using namespace hn::core;
using hn::editor::Mode;

namespace hn::app {

NoteSession::NoteSession(NoteRepository *repo, QString rel, QObject *parent)
    : QObject(parent), m_repo(repo), m_rel(std::move(rel)) {
    m_editor = std::make_unique<hn::editor::NoteEditor>();
    m_toolbar = std::make_unique<hn::editor::FormattingToolbar>(m_editor.get());
    m_idle.setSingleShot(true);
    m_max.setSingleShot(true);
    connect(&m_idle, &QTimer::timeout, this, [this] { stopTimers(); saveNow(); });
    connect(&m_max, &QTimer::timeout, this, [this] { stopTimers(); saveNow(); });
    connect(m_editor.get(), &hn::editor::NoteEditor::contentChanged, this, [this] { onContent(); });
    connect(repo, &NoteRepository::saved, this, [this](const QString &r, quint64 v) { onSaved(r, v); });
    connect(repo, &NoteRepository::saveFailed, this, [this](const QString &r, const QString &e, quint64 v) { onFailed(r, e, v); });
    connect(repo, &NoteRepository::conflict, this, [this](const QString &r, const QByteArray &ext, const QByteArray &, quint64 v) { onConflict(r, ext, v); });
    connect(repo, &NoteRepository::externalChange, this, [this](const QString &r) { onExternal(r); });
    connect(repo, &NoteRepository::fileRemoved, this, [this](const QString &r) { onRemoved(r); });
}

NoteSession::~NoteSession() = default;

QString NoteSession::folder() const {
    const int i = m_rel.lastIndexOf('/');
    return i < 0 ? QString() : m_rel.left(i);
}

void NoteSession::updateMeta(const QByteArray &bytes) {
    const NoteMeta m = extractMeta(QFileInfo(m_rel).completeBaseName(), bytes);
    if (m.title != m_title) { m_title = m.title; emit titleChanged(); }
    m_tags = m.tags;
}

bool NoteSession::load(QString *err) {
    const NoteSnapshot s = m_repo->read(m_rel);
    if (!s.ok) { if (err) *err = s.error; return false; }
    m_loading = true;
    m_editor->load(s.bytes);
    m_loading = false;
    m_baseRev = s.revision;
    updateMeta(s.bytes);
    m_repo->watch(m_rel);
    recompute();
    return true;
}

void NoteSession::loadDraft(const QByteArray &local) {
    m_loading = true;
    m_editor->load(local);
    m_loading = false;
    updateMeta(local);
    m_forceDirty = true;
    m_idle.start(m_timing.idleMs);
    recompute();
}

// ---------------------------------------------------------------- status
QString NoteSession::stateLabel() const {
    switch (m_state) {
    case State::Clean: return tr("Saved");
    case State::Dirty: return tr("Unsaved");
    case State::Saving: return tr("Saving");
    case State::Failed: return tr("Save failed");
    case State::Conflict: return tr("Conflict");
    case State::Removed: return tr("Deleted on disk");
    }
    return {};
}

QString NoteSession::detail() const {
    switch (m_state) {
    case State::Failed: return m_failMsg;
    case State::Conflict: return tr("This file changed on disk. Your edits are kept.");
    case State::Removed: return tr("Your edits are kept here and in recovery.");
    default: return {};
    }
}

void NoteSession::recompute() {
    State s = State::Clean;
    if (m_conflict) s = State::Conflict;
    else if (m_removed) s = State::Removed;
    else if (!m_jobs.isEmpty()) s = State::Saving;
    else if (!m_failMsg.isEmpty()) s = State::Failed;
    else if (dirty()) s = State::Dirty;
    const QString d = detail();
    const bool changed = s != m_state || d != m_lastDetail;
    m_state = s;
    m_lastDetail = d;
    if (changed) emit statusChanged();
    if (m_closing) {
        if (s == State::Clean) { m_closing = false; emit closeReady(); }
        else if (s == State::Failed || s == State::Conflict || s == State::Removed) { m_closing = false; emit closeRefused(); }
    }
}

// ---------------------------------------------------------------- autosave
void NoteSession::onContent() {
    if (m_loading) return;
    if (!m_conflict && !m_removed) {
        m_idle.start(m_timing.idleMs);                       // 750 ms after the last edit...
        if (!m_max.isActive()) m_max.start(m_timing.maxMs);  // ...but never later than 5 s during continuous typing
    }
    recompute();
    emit contentEdited();
}

void NoteSession::saveNow(bool force) {
    if (!dirty()) { recompute(); return; }
    Job j{m_editor->revision(), m_editor->toMarkdownBytes()};
    if (m_preSave && !m_editor->isReadOnly() && !j.bytes.isNull()) {   // never blocks saving: a failing plugin leaves the bytes unchanged
        const QByteArray t = m_preSave(j.bytes);
        if (!t.isNull()) j.bytes = t;
    }
    const quint64 rev = m_repo->save(m_rel, j.bytes, m_baseRev, force);
    m_jobs.insert(rev, j);
    m_failMsg.clear();
    recompute();
}

void NoteSession::flush() {
    stopTimers();
    if (m_conflict || m_removed) return;
    saveNow();
}

void NoteSession::requestClose() {
    m_closing = true;
    flush();
    recompute();
}

void NoteSession::dropJobsUpTo(quint64 rev) {
    // A queued snapshot replaced by a newer one never reports; anything older than a result is finished.
    for (auto it = m_jobs.begin(); it != m_jobs.end();) it = it.key() <= rev ? m_jobs.erase(it) : std::next(it);
}

void NoteSession::onSaved(const QString &rel, quint64 rev) {
    if (rel != m_rel) return;
    const auto it = m_jobs.constFind(rev);
    if (it == m_jobs.constEnd()) return;   // superseded by a reload/resolve
    const Job job = *it;
    dropJobsUpTo(rev);
    m_baseRev = rev;
    m_failMsg.clear();
    m_forceDirty = false;
    if (job.editorRev == m_editor->revision()) m_editor->markSaved(job.bytes);   // newer edits stay dirty
    updateMeta(job.bytes);
    emit savedOk(m_rel);
    if (dirty() && m_jobs.isEmpty() && !m_idle.isActive()) m_idle.start(m_timing.idleMs);
    recompute();
}

void NoteSession::onFailed(const QString &rel, const QString &err, quint64 rev) {
    if (rel != m_rel || !m_jobs.contains(rev)) return;
    dropJobsUpTo(rev);
    if (!m_removed) m_failMsg = err.isEmpty() ? tr("The file could not be written.") : err;
    recompute();
}

void NoteSession::onConflict(const QString &rel, const QByteArray &external, quint64 rev) {
    if (rel != m_rel || !m_jobs.contains(rev)) return;
    dropJobsUpTo(rev);
    m_conflict = true;
    m_external = external;
    stopTimers();
    recompute();
}

void NoteSession::onExternal(const QString &rel) {
    if (rel != m_rel) return;
    m_removed = false;
    if (dirty() || !m_jobs.isEmpty()) {
        QByteArray disk;
        if (readAll(m_repo->absolutePath(m_rel), &disk, nullptr)) m_external = disk;
        m_conflict = true;
        stopTimers();
        recompute();
        return;
    }
    reload();   // clean document: take the new version, keep cursor/scroll context
}

void NoteSession::onRemoved(const QString &rel) {
    if (rel != m_rel) return;
    if (dirty() || !m_jobs.isEmpty()) { m_removed = true; stopTimers(); recompute(); return; }
    emit removedWhileClean(m_rel);
}

// ---------------------------------------------------------------- resolve actions
void NoteSession::retry() { stopTimers(); saveNow(); }

void NoteSession::reload() {
    const int cur = cursorPosition(), scroll = scrollValue();
    const NoteSnapshot s = m_repo->read(m_rel);
    if (!s.ok) { m_failMsg = tr("Cannot read the file: %1").arg(s.error); recompute(); return; }
    m_loading = true;
    m_editor->load(s.bytes);
    m_loading = false;
    restoreView(cur, scroll);
    m_baseRev = s.revision;
    m_conflict = m_removed = m_forceDirty = false;
    m_failMsg.clear();
    m_jobs.clear();
    stopTimers();
    m_external.clear();
    m_repo->resolveRecovery(m_rel);   // the user chose the disk version
    updateMeta(s.bytes);
    recompute();
}

bool NoteSession::saveLocalCopy(QString *copyRel) {
    const QByteArray local = m_editor->toMarkdownBytes();
    QString err;
    const QString rel = m_repo->create(folder(), QFileInfo(m_rel).completeBaseName() + QStringLiteral(" (local copy)"), local, &err);
    if (rel.isEmpty()) { m_failMsg = tr("Could not save a copy: %1").arg(err); recompute(); return false; }
    if (copyRel) *copyRel = rel;
    emit copyCreated(rel);
    reload();
    return true;
}

void NoteSession::replaceDisk() {
    m_conflict = false;
    stopTimers();
    saveNow(true);
}

void NoteSession::recreate() {
    m_removed = false;
    stopTimers();
    saveNow(true);
}

void NoteSession::discard() {
    stopTimers();
    m_jobs.clear();
    m_conflict = m_removed = m_forceDirty = false;
    m_failMsg.clear();
    m_repo->resolveRecovery(m_rel);
    m_editor->markSaved();
    recompute();
}

// ---------------------------------------------------------------- view state
int NoteSession::cursorPosition() const {
    return m_editor->mode() == Mode::Visual ? m_editor->visualEdit()->textCursor().position()
                                            : m_editor->sourceEdit()->textCursor().position();
}

int NoteSession::scrollValue() const {
    return m_editor->mode() == Mode::Visual ? m_editor->visualEdit()->verticalScrollBar()->value()
                                            : m_editor->sourceEdit()->verticalScrollBar()->value();
}

void NoteSession::restoreView(int cursor, int scroll) {
    auto apply = [&](auto *edit) {
        QTextCursor c(edit->document());
        c.setPosition(qBound(0, cursor, qMax(0, edit->document()->characterCount() - 1)));
        edit->setTextCursor(c);
        edit->verticalScrollBar()->setValue(scroll);
    };
    if (m_editor->mode() == Mode::Visual) apply(m_editor->visualEdit()); else apply(m_editor->sourceEdit());
}

void NoteSession::applyTheme(const hn::theme::Theme &t) {
    // setTheme re-normalises the document, which the editor counts as an edit; a clean note must stay clean.
    const bool wasClean = !dirty();
    QByteArray before = wasClean ? m_editor->toMarkdownBytes() : QByteArray();
    if (before.isNull()) before = QByteArray("");
    m_editor->setTheme(t);
    if (wasClean && m_editor->isModified()) { m_editor->markSaved(before); stopTimers(); recompute(); }
    m_toolbar->setTheme(t);
    // The shared stylesheet hides toolbar icons on hover (text-on-text); give the strip its own hover rule.
    m_toolbar->setStyleSheet(QString("QToolButton:hover { background: %1; color: %2; border-color: transparent; }"
                                     "QToolButton:checked { background: transparent; border-bottom: 2px solid %3; }"
                                     "QToolButton::menu-indicator { image: none; }")
                                 .arg(t.selection.name(), t.text.name(), t.accent.name()));
}

} // namespace hn::app
