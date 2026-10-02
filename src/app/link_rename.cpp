#include "link_rename.h"
#include "controller.h"
#include "plugin_bridges.h"
#include "ui_common.h"
#include <QApplication>
#include <QDialog>
#include <QEventLoop>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

using namespace hn::core;

namespace hn::app {

SaveOutcome saveAndWait(NoteRepository &repo, const QString &rel, const QByteArray &bytes, quint64 baseRev, int capMs) {
    const quint64 rev = repo.save(rel, bytes, baseRev);
    SaveOutcome out = SaveOutcome::Timeout;
    bool done = false;
    QEventLoop loop;
    QTimer cap;
    cap.setSingleShot(true);
    auto finish = [&](SaveOutcome o) { done = true; out = o; loop.quit(); };
    const auto c1 = QObject::connect(&repo, &NoteRepository::saved, &loop, [&](const QString &r, quint64 v) { if (r == rel && v == rev) finish(SaveOutcome::Saved); });
    const auto c2 = QObject::connect(&repo, &NoteRepository::saveFailed, &loop, [&](const QString &r, const QString &, quint64 v) { if (r == rel && v == rev) finish(SaveOutcome::Failed); });
    const auto c3 = QObject::connect(&repo, &NoteRepository::conflict, &loop, [&](const QString &r, const QByteArray &, const QByteArray &, quint64 v) { if (r == rel && v == rev) finish(SaveOutcome::Conflict); });
    QObject::connect(&cap, &QTimer::timeout, &loop, &QEventLoop::quit);
    cap.start(capMs);
    if (!done) loop.exec(QEventLoop::ExcludeUserInputEvents);
    QObject::disconnect(c1); QObject::disconnect(c2); QObject::disconnect(c3);
    return out;
}

bool waitSettled(NoteSession *s, int capMs) {
    using St = NoteSession::State;
    auto terminal = [s] { return s->state() == St::Clean || s->state() == St::Failed || s->state() == St::Conflict || s->state() == St::Removed; };
    if (!terminal()) {
        QEventLoop loop;
        QTimer cap;
        cap.setSingleShot(true);
        QObject::connect(s, &NoteSession::statusChanged, &loop, [&] { if (terminal()) loop.quit(); });
        QObject::connect(&cap, &QTimer::timeout, &loop, &QEventLoop::quit);
        cap.start(capMs);
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    }
    return s->state() == St::Clean;
}

LinkChoice askLinkUpdate(QWidget *parent, const QString &summary, const QStringList &lines) {
    const auto &t = ui::theme();
    QDialog d(parent);
    d.setWindowTitle(QObject::tr("Update links?"));
    d.setWindowIcon(ui::appIcon());
    d.setModal(true);
    d.setMinimumWidth(460);
    auto *root = new QVBoxLayout(&d);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto *head = new QWidget(&d);
    head->setObjectName("hnLinkHead");
    head->setStyleSheet(QString("QWidget#hnLinkHead { border-left: 6px solid %1; border-bottom: 1px solid %2; }").arg(t.accent.name(), t.border.name()));
    auto *hl = new QVBoxLayout(head);
    hl->setContentsMargins(20, 12, 20, 12);
    auto *title = new QLabel(QObject::tr("Update links to this note?"), head);
    title->setFont(ui::uiFont(16, QFont::Bold));
    title->setWordWrap(true);
    hl->addWidget(title);
    root->addWidget(head);
    auto *body = new QWidget(&d);
    auto *bl = new QVBoxLayout(body);
    bl->setContentsMargins(20, 16, 20, 8);
    bl->setSpacing(8);
    auto *sum = new QLabel(summary, body);
    sum->setWordWrap(true);
    bl->addWidget(sum);
    auto *list = new QListWidget(body);
    list->setObjectName("hnLinkList");
    list->addItems(lines);
    list->setSelectionMode(QAbstractItemView::NoSelection);
    list->setFocusPolicy(Qt::NoFocus);
    list->setFrameShape(QFrame::NoFrame);
    list->setStyleSheet(QString("QListWidget { border: 1px solid %1; } QListWidget::item { padding: 4px 8px; }").arg(t.border.name()));
    list->setFixedHeight(qBound(48, 28 * int(lines.size()) + 8, 224));
    bl->addWidget(list);
    root->addWidget(body, 1);
    auto *bar = new QWidget(&d);
    auto *bh = new QHBoxLayout(bar);
    bh->setContentsMargins(20, 8, 20, 16);
    bh->addStretch(1);
    auto *cancel = new QPushButton(QObject::tr("Cancel"), bar);
    auto *only = new QPushButton(QObject::tr("Rename only"), bar);
    auto *update = new QPushButton(QObject::tr("Update links"), bar);
    for (auto *b : {cancel, only, update}) b->setMinimumHeight(36);
    update->setDefault(true);
    update->setStyleSheet(ui::accentButtonStyle() + "QPushButton { text-align: center; min-height: 36px; }");
    bh->addWidget(cancel);
    bh->addWidget(only);
    bh->addWidget(update);
    root->addWidget(bar);
    LinkChoice choice = LinkChoice::Cancel;
    QObject::connect(cancel, &QPushButton::clicked, &d, &QDialog::reject);
    QObject::connect(only, &QPushButton::clicked, &d, [&] { choice = LinkChoice::RenameOnly; d.accept(); });
    QObject::connect(update, &QPushButton::clicked, &d, [&] { choice = LinkChoice::Update; d.accept(); });
    d.exec();
    return choice;
}

namespace {
QString textOf(QByteArray b) {
    if (b.startsWith("\xEF\xBB\xBF")) b.remove(0, 3);
    QString t = QString::fromUtf8(b);
    t.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    t.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return t;
}
struct Plan {
    FileEdit edit;
    QString problem;            // non-empty: this note is left alone
    qint64 size = 0, mtime = 0; // closed notes: stat at plan time, re-checked right before the save
};
}  // namespace

// Rename + (optionally) rewrite links in other notes. Order matters because FileEdit paths are keyed by the OLD path:
//   1. compute the edits while the index still knows the old name (open notes with unsaved text are saved first so the
//      edits are computed from what the user sees),  2. confirm,  3. rename (nothing else has changed yet, so a failure
//      here aborts cleanly),  4. apply each edit; the renamed note's own edit goes to its NEW path.
// Each file is written atomically through the normal save path; a file that cannot be updated is reported and left as it was.
bool AppController::renameNoteWithLinks(const QString &rel, const QString &newTitle, LinkMode mode, QString *err, QString *newRelOut) {
    m_linkReport = {};
    auto fail = [&](const QString &m) { if (err) *err = m; return false; };
    QString e;
    const QString newRel = renamedRelFor(rel, newTitle, &e);
    if (newRel.isEmpty()) return fail(e);
    if (newRelOut) *newRelOut = newRel;
    if (newRel == rel) return true;
    m_linkReport.oldRel = rel;
    m_linkReport.newRel = newRel;
    if (mode == LinkMode::None) {
        const bool ok = renameNote(rel, newTitle, err);
        m_linkReport.renamed = ok;
        return ok;
    }
    if (NoteSession *s = session(rel); s && !s->settled()) {
        s->flush();
        if (!waitSettled(s)) return fail(tr("The note is still saving. Try again in a moment."));
    }
    if (QFileInfo::exists(m_repo->absolutePath(newRel))) return fail(tr("A note named “%1” already exists.").arg(QFileInfo(newRel).completeBaseName()));

    LibraryIndex *idx = index();
    QList<FileEdit> edits = idx->rewriteLinksForRename(rel, newRel);
    QList<NoteSession *> unsaved;
    for (const auto &fe : std::as_const(edits)) if (NoteSession *ss = session(fe.path); ss && ss->state() != NoteSession::State::Clean) unsaved << ss;
    if (!unsaved.isEmpty()) {   // the disk copy is stale for these: save, tell the index, compute again from the saved text
        for (auto *ss : unsaved) ss->flush();
        for (auto *ss : unsaved) if (waitSettled(ss)) idx->updatePath(ss->rel());
        edits = idx->rewriteLinksForRename(rel, newRel);
    }
    if (edits.isEmpty()) {   // nothing links here: a plain rename, no dialog
        const bool ok = renameNote(rel, newTitle, err);
        m_linkReport.renamed = ok;
        return ok;
    }

    QList<Plan> plan;
    int links = 0, files = 0;
    for (const auto &fe : std::as_const(edits)) {
        Plan p;
        p.edit = fe;
        if (NoteSession *ss = session(fe.path)) {
            if (ss->state() != NoteSession::State::Clean) p.problem = tr("unsaved changes that cannot be saved (%1)").arg(ss->stateLabel().toLower());
            else if (ss->editor()->isReadOnly()) p.problem = tr("opened read-only");
        } else {
            const QFileInfo fi(m_repo->absolutePath(fe.path));
            if (!fi.exists()) p.problem = tr("the file is gone");
            else if (!fi.isWritable()) p.problem = tr("the file is read-only");
            else if (!QFileInfo(fi.absolutePath()).isWritable()) p.problem = tr("the folder is read-only");
            p.size = fi.size();
            p.mtime = fi.lastModified().toMSecsSinceEpoch();
        }
        if (p.problem.isEmpty()) { links += fe.changedCount; ++files; }
        plan << p;
    }
    QStringList lines;
    for (const auto &p : std::as_const(plan)) {
        QString l = tr("%1  -  %n link(s)", "", p.edit.changedCount).arg(p.edit.path);
        if (!p.problem.isEmpty()) l += tr("   (will not be updated: %1)").arg(p.problem);
        lines << l;
    }
    m_linkReport.offered = true;
    const QString summary = tr("Renaming “%1” to “%2”: %n link(s) in %3 other note(s) point to it and can be updated. Each note is saved separately; open notes get one undo step.", "", links)
                                .arg(QFileInfo(rel).completeBaseName(), QFileInfo(newRel).completeBaseName()).arg(files);
    LinkChoice choice;
    if (m_opt.linkChoice) choice = m_opt.linkChoice(summary, lines);
    else {
        QWidget *parent = QApplication::activeWindow() ? QApplication::activeWindow() : static_cast<QWidget *>(m_organizer.data());
        choice = askLinkUpdate(parent, summary, lines);
    }
    if (choice == LinkChoice::Cancel) { m_linkReport.cancelled = true; if (err) err->clear(); return false; }
    if (!renameNote(rel, newTitle, &e)) return fail(e);   // nothing was touched yet
    m_linkReport.renamed = true;
    if (choice == LinkChoice::Update) {
        for (const auto &p : std::as_const(plan)) {
            const QString target = p.edit.path == rel ? newRel : p.edit.path;
            auto bad = [&](const QString &why) { m_linkReport.failed << QStringLiteral("%1: %2").arg(target, why); };
            if (!p.problem.isEmpty()) { bad(p.problem); continue; }
            if (NoteSession *ss = session(target)) {
                if (ss->state() != NoteSession::State::Clean || ss->editor()->isReadOnly()) { bad(tr("changed while renaming")); continue; }
                const int cur = ss->cursorPosition(), scroll = ss->scrollValue();
                PluginNoteBridge b(ss);   // one undo step; the normal edit path (autosave, recovery copy) takes over
                b.beginTransaction(QStringLiteral("update links"));
                b.setText(textOf(p.edit.newBytes));
                b.endTransaction();
                ss->restoreView(cur, scroll);
                ss->flush();
                if (!waitSettled(ss)) { bad(tr("kept in the editor, not saved yet (%1)").arg(ss->stateLabel().toLower())); continue; }
            } else {
                const QFileInfo fi(m_repo->absolutePath(target));
                if (!fi.exists() || fi.size() != p.size || fi.lastModified().toMSecsSinceEpoch() != p.mtime) { bad(tr("changed on disk meanwhile")); continue; }
                const auto snap = m_repo->read(target);
                if (!snap.ok) { bad(snap.error); continue; }
                const SaveOutcome o = saveAndWait(*m_repo, target, p.edit.newBytes, snap.revision);
                if (o != SaveOutcome::Saved) { bad(o == SaveOutcome::Conflict ? tr("changed on disk meanwhile") : o == SaveOutcome::Timeout ? tr("save timed out") : tr("could not be written")); continue; }
                idx->updatePath(target);
            }
            ++m_linkReport.updatedFiles;
            m_linkReport.updatedLinks += p.edit.changedCount;
        }
    }
    if (choice == LinkChoice::Update) {
        QString msg = tr("Renamed to “%1”. Updated %n link(s) in %2 note(s).", "", m_linkReport.updatedLinks).arg(QFileInfo(newRel).completeBaseName()).arg(m_linkReport.updatedFiles);
        if (!m_linkReport.failed.isEmpty()) msg += tr(" Not updated: %1").arg(m_linkReport.failed.join(QStringLiteral("; ")));
        announce(msg);
    }
    emit notesChanged();
    return true;
}

}  // namespace hn::app
