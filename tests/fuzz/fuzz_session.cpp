#include "support.h"
#include "hn/platform/window_placement.h"
extern "C" int hn_fuzz_session(const uint8_t *data, size_t size) {
  bool ok = false;
  hn::platform::fromJson(QByteArray(reinterpret_cast<const char *>(data), qsizetype(size)), &ok);
  return 0;
}
#ifndef HN_FUZZ_REGRESSION_BUILD
HN_FUZZ_TARGET(session)
#endif
