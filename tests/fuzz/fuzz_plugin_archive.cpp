#include "support.h"
#include "hn/plugins/store.h"
#include <QFile>
extern "C" int hn_fuzz_plugin_archive(const uint8_t *data, size_t size) {
  hnFuzzInit();
  const QString path = hnFuzzTempPath() + QStringLiteral("/input.hnplugin");
  QFile::remove(path);
  QFile file(path);
  if (file.open(QIODevice::WriteOnly)) {
    file.write(reinterpret_cast<const char *>(data), qint64(size));
    file.close();
    hn::plugins::checkPackage(path);
  }
  QFile::remove(path);
  return 0;
}
#ifndef HN_FUZZ_REGRESSION_BUILD
HN_FUZZ_TARGET(plugin_archive)
#endif
