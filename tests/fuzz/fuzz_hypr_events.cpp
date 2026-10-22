#include "support.h"
#include "hn/platform/hyprland_ipc.h"
extern "C" int hn_fuzz_hypr_events(const uint8_t *data, size_t size) {
  const QByteArray bytes(reinterpret_cast<const char *>(data), qsizetype(size));
  const auto lines = bytes.split('\n');
  for (const auto &line : lines) { hn::platform::HyprEventLine event; hn::platform::parseHyprEventLine(line, &event); }
  return 0;
}
#ifndef HN_FUZZ_REGRESSION_BUILD
HN_FUZZ_TARGET(hypr_events)
#endif
