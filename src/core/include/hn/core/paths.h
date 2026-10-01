#pragma once
#include <QString>

// XDG helpers for 'hyprnotes'. Each call re-reads the environment (cheap, test-friendly).
// Test overrides (win over XDG): HN_CONFIG_DIR, HN_STATE_DIR, HN_CACHE_DIR, HN_DATA_DIR, HN_NOTES_DIR.
// Nothing here creates directories.
namespace hn::core::paths {
QString configDir();       // $XDG_CONFIG_HOME/hyprnotes  (~/.config/hyprnotes)
QString stateDir();        // $XDG_STATE_HOME/hyprnotes   (~/.local/state/hyprnotes)
QString cacheDir();        // $XDG_CACHE_HOME/hyprnotes   (~/.cache/hyprnotes)
QString dataDir();         // $XDG_DATA_HOME/hyprnotes    (~/.local/share/hyprnotes)
QString defaultNotesDir(); // ~/Notes/Hyprnotes
}
