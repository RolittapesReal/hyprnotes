#pragma once
#include <QHash>
#include <QString>

namespace hn::app {

struct NoteView { int color = -1; int cursor = 0; int scroll = 0; };

// Per-note UI state (colour, last cursor/scroll) under $XDG_STATE_HOME/hyprnotes/notes-state.json.
// Loaded lazily; every mutation writes atomically (tiny file, user-initiated events only).
class NoteStateStore {
public:
    explicit NoteStateStore(QString path) : m_path(std::move(path)) {}
    NoteView get(const QString &rel) const;
    int color(const QString &rel) const;          // explicit choice, else stable default from the path
    void setColor(const QString &rel, int idx);
    void setView(const QString &rel, int cursor, int scroll);
    void rename(const QString &oldRel, const QString &newRel);
    void remove(const QString &rel);
    QString path() const { return m_path; }
private:
    void load() const;
    void save() const;
    QString m_path;
    mutable QHash<QString, NoteView> m_map;
    mutable bool m_loaded = false;
};

} // namespace hn::app
