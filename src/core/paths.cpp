#include "hn/core/paths.h"
#include <QDir>

namespace hn::core::paths {
namespace {
QString resolve(const char *override_, const char *xdg, const char *homeRel) {
    QString o = qEnvironmentVariable(override_);
    if (!o.isEmpty()) return QDir::cleanPath(o);
    QString x = qEnvironmentVariable(xdg);
    // XDG spec: relative values are invalid and must be ignored.
    if (!x.isEmpty() && QDir::isAbsolutePath(x)) return QDir::cleanPath(x) + "/hyprnotes";
    return QDir::homePath() + "/" + homeRel + "/hyprnotes";
}
}
QString configDir() { return resolve("HN_CONFIG_DIR", "XDG_CONFIG_HOME", ".config"); }
QString stateDir() { return resolve("HN_STATE_DIR", "XDG_STATE_HOME", ".local/state"); }
QString cacheDir() { return resolve("HN_CACHE_DIR", "XDG_CACHE_HOME", ".cache"); }
QString dataDir() { return resolve("HN_DATA_DIR", "XDG_DATA_HOME", ".local/share"); }
QString defaultNotesDir() {
    QString o = qEnvironmentVariable("HN_NOTES_DIR");
    return o.isEmpty() ? QDir::homePath() + "/Notes/Hyprnotes" : QDir::cleanPath(o);
}
}
