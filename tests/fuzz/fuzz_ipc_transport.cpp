#include "support.h"
#include "hn/platform/instance_transport.h"
#include <QFile>
extern "C" int hn_fuzz_ipc_transport(const uint8_t *data, size_t size) {
  const QByteArray bytes(reinterpret_cast<const char *>(data), qsizetype(size));
  hn::platform::Action action{};
  QString error;
  hn::platform::decodeInstanceRequest(bytes, &action, &error);
  Q_UNUSED(action);
  return 0;
}
#ifndef HN_FUZZ_REGRESSION_BUILD
HN_FUZZ_TARGET(ipc_transport)
#endif
