#include "note_state.h"
#include "hn/core/fs_util.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace hn::app {

void NoteStateStore::load() const {
    if (m_loaded) return;
    m_loaded = true;
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (o["version"].toInt() != 1) return;
    const QJsonObject notes = o["notes"].toObject();
    for (auto it = notes.begin(); it != notes.end(); ++it) {
        const QJsonObject n = it.value().toObject();
        m_map.insert(it.key(), {n.contains("color") ? n["color"].toInt(-1) : -1, n["cursor"].toInt(), n["scroll"].toInt()});
    }
}

void NoteStateStore::save() const {
    QJsonObject notes;
    for (auto it = m_map.begin(); it != m_map.end(); ++it)
        notes[it.key()] = QJsonObject{{"color", it->color}, {"cursor", it->cursor}, {"scroll", it->scroll}};
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    hn::core::atomicWrite(m_path, QJsonDocument(QJsonObject{{"version", 1}, {"notes", notes}}).toJson(QJsonDocument::Compact), nullptr);
}

NoteView NoteStateStore::get(const QString &rel) const { load(); return m_map.value(rel); }

int NoteStateStore::color(const QString &rel) const {
    const int c = get(rel).color;
    if (c >= 0 && c < 6) return c;
    return qChecksum(rel.toUtf8()) % 4;   // red, blue, yellow or green; stable across runs
}

void NoteStateStore::setColor(const QString &rel, int idx) { load(); m_map[rel].color = qBound(0, idx, 5); save(); }
void NoteStateStore::setView(const QString &rel, int cursor, int scroll) {
    load();
    const bool had = m_map.contains(rel);
    NoteView &v = m_map[rel];
    if (had && v.cursor == cursor && v.scroll == scroll) return;
    v.cursor = cursor; v.scroll = scroll;
    save();
}
void NoteStateStore::rename(const QString &o, const QString &n) {
    load();
    NoteView v = m_map.take(o);
    if (v.color < 0) v.color = qChecksum(o.toUtf8()) % 4;   // keep the colour the old path implied
    m_map.insert(n, v);
    save();
}
void NoteStateStore::remove(const QString &rel) { load(); if (m_map.remove(rel)) save(); }

} // namespace hn::app
