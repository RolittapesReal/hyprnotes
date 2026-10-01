#pragma once
// Native-tier plugins on top of hn::mods::ModHost: one private ModHost per activated plugin (activate/deactivate are
// independent; deactivating shuts that plugin's library down). The package is staged (mod.json + a copy of the library)
// under <stateDir>/native-stage so the plugin folder - whose SHA-256 was consented to - is never modified.
#include "hn/mods/mods.h"
#include "hn/plugins/runtime.h"
#include <map>
#include <memory>

namespace hn::app {

class ModNativeAdapter : public hn::plugins::NativePluginAdapter {
public:
    explicit ModNativeAdapter(QString stateDir) : m_state(std::move(stateDir)) {}
    ~ModNativeAdapter() override;
    bool activate(const hn::plugins::Manifest &m, QString *err) override;
    void deactivate(const QString &pluginId) override;
    bool runCommand(const QString &pluginId, const QString &commandId, hn::plugins::NoteBridge *note, QString *err) override;
    void postEvent(const QString &pluginId, const QString &event, const QString &arg) override;

    QList<hn::plugins::CommandReg> commands() const;   // declared + registered commands of active native plugins
    bool isActive(const QString &id) const { return m_hosts.count(id) > 0; }
    int activeCount() const { return int(m_hosts.size()); }
    bool isLoaded(const QString &id) const;            // the library is actually dlopen'ed

private:
    QString m_state;
    std::map<QString, std::unique_ptr<hn::mods::ModHost>> m_hosts;
};

} // namespace hn::app
