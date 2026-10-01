#include "hn/plugins/types.h"
#include <QJsonArray>

namespace hn::plugins {

QJsonObject PluginRegs::toJson() const {
    QJsonObject o;
    QJsonArray c, t, m, g, s;
    for (const auto &x : commands) c.append(QJsonObject{{"id", x.id}, {"title", x.title}, {"key", x.key}});
    for (const auto &x : toolbars) t.append(QJsonObject{{"id", x.id}, {"title", x.title}, {"icon", x.icon}});
    for (const auto &x : menus) m.append(QJsonObject{{"id", x.id}, {"title", x.title}, {"where", x.where}});
    for (const auto &x : triggers) g.append(x.pattern);
    for (const auto &x : settings) s.append(QJsonObject{{"id", x.id}, {"type", x.type}, {"title", x.title}, {"default", QJsonValue::fromVariant(x.def)}});
    o["commands"] = c; o["toolbars"] = t; o["menus"] = m; o["triggers"] = g; o["settings"] = s;
    o["events"] = QJsonArray::fromStringList(events);
    return o;
}

PluginRegs PluginRegs::fromJson(const QString &p, const QJsonObject &o) {
    PluginRegs r;
    for (const auto &v : o["commands"].toArray()) { auto x = v.toObject(); r.commands.append({p, x["id"].toString(), x["title"].toString(), x["key"].toString()}); }
    for (const auto &v : o["toolbars"].toArray()) { auto x = v.toObject(); r.toolbars.append({p, x["id"].toString(), x["title"].toString(), x["icon"].toString()}); }
    for (const auto &v : o["menus"].toArray()) { auto x = v.toObject(); r.menus.append({p, x["id"].toString(), x["title"].toString(), x["where"].toString()}); }
    for (const auto &v : o["triggers"].toArray()) r.triggers.append({p, v.toString()});
    for (const auto &v : o["settings"].toArray()) { auto x = v.toObject(); r.settings.append({p, x["id"].toString(), x["type"].toString(), x["title"].toString(), x["default"].toVariant()}); }
    for (const auto &v : o["events"].toArray()) r.events << v.toString();
    return r;
}

void PluginRegistry::set(const QString &id, const PluginRegs &r) {
    if (auto it = m_.find(id); it != m_.end() && *it == r) return;
    m_[id] = r;
    emit changed();
}
void PluginRegistry::remove(const QString &id) { if (m_.remove(id)) emit changed(); }
void PluginRegistry::clear() { if (!m_.isEmpty()) { m_.clear(); emit changed(); } }
const PluginRegs *PluginRegistry::of(const QString &id) const { auto it = m_.constFind(id); return it == m_.constEnd() ? nullptr : &*it; }
QList<CommandReg> PluginRegistry::commands() const { QList<CommandReg> r; for (const auto &x : m_) r += x.commands; return r; }
QList<ToolbarReg> PluginRegistry::toolbarButtons() const { QList<ToolbarReg> r; for (const auto &x : m_) r += x.toolbars; return r; }
QList<MenuReg> PluginRegistry::menuItems(const QString &where) const {
    QList<MenuReg> r;
    for (const auto &x : m_) for (const auto &mi : x.menus) if (where.isEmpty() || mi.where == where) r << mi;
    return r;
}
QList<TriggerReg> PluginRegistry::triggers() const { QList<TriggerReg> r; for (const auto &x : m_) r += x.triggers; return r; }
QList<SettingReg> PluginRegistry::settings(const QString &id) const {
    QList<SettingReg> r;
    for (auto it = m_.begin(); it != m_.end(); ++it) if (id.isEmpty() || it.key() == id) r += it->settings;
    return r;
}
QStringList PluginRegistry::subscribers(const QString &event) const {
    QStringList r;
    for (auto it = m_.begin(); it != m_.end(); ++it) if (it->events.contains(event)) r << it.key();
    return r;
}

}  // namespace hn::plugins
