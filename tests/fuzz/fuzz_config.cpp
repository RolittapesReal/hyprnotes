#include "support.h"
#include "config.h"
#include <QFile>
extern "C" int hn_fuzz_config(const uint8_t *data, size_t size) {
  hnFuzzInit();
  const QString path = hnFuzzTempPath() + QStringLiteral("/config.json");
  QFile file(path);
  if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    file.write(reinterpret_cast<const char *>(data), qint64(size));
    file.close();
    hn::theme::Config config(path);
    config.reload();
  }
  return 0;
}
#ifndef HN_FUZZ_REGRESSION_BUILD
HN_FUZZ_TARGET(config)
#endif
