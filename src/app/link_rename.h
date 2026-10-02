#pragma once
// Rename-with-link-update support: bounded waits on the save path and the one confirm dialog (Update links / Rename only / Cancel).
#include "hn/core/note_repository.h"
#include <QByteArray>
#include <QString>
#include <QStringList>

class QWidget;

namespace hn::app {

class NoteSession;

enum class LinkMode { Ask, None };                         // Ask: offer to rewrite links in other notes (only when some exist)
enum class LinkChoice { Update, RenameOnly, Cancel };
enum class SaveOutcome { Saved, Failed, Conflict, Timeout };

// Saves through the repository (atomic, conflict detected against the baseline set by repo.read()) and waits for the result.
SaveOutcome saveAndWait(hn::core::NoteRepository &repo, const QString &rel, const QByteArray &bytes, quint64 baseRev, int capMs = 5000);
// Waits (input events excluded) until the session is Clean (true) or reaches Failed/Conflict/Removed or the cap passes (false).
bool waitSettled(NoteSession *s, int capMs = 5000);

// What the last renameNoteWithLinks() did.
struct LinkUpdateReport {
    QString oldRel, newRel;
    bool renamed = false, cancelled = false, offered = false;
    int updatedFiles = 0, updatedLinks = 0;
    QStringList failed;   // "path: reason" for every note that was left with its old links
};

// Flat modal dialog listing the affected notes. `lines` are shown verbatim, one per row.
LinkChoice askLinkUpdate(QWidget *parent, const QString &summary, const QStringList &lines);

}  // namespace hn::app
