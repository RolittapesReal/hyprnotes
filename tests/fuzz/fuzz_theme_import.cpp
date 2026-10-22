#include "support.h"
#include "hn/theme/theme_io.h"
#include <QDir>
#include <QFile>
extern "C" int hn_fuzz_theme_import(const uint8_t *data, size_t size) {
  hnFuzzInit();
  QDir(hn::theme::themesDir()).removeRecursively();
  const QString path = hnFuzzTempPath() + QStringLiteral("/input.theme");
  QFile::remove(path);
  QFile file(path);
  if (file.open(QIODevice::WriteOnly)) {
    file.write(reinterpret_cast<const char *>(data), qint64(size));
    file.close();
    QString name, error;
    QStringList warnings;
    hn::theme::importTheme(path, &name, &error, &warnings);
  }
  QFile::remove(path);
  hnFuzzShutdown();
  return 0;
}
#ifndef HN_FUZZ_REGRESSION_BUILD
HN_FUZZ_TARGET(theme_import)
#endif
