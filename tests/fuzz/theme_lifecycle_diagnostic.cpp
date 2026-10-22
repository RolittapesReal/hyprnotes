#include "support.h"

#include <QCoreApplication>
#include <QFile>

#include <cstdio>

extern "C" int hn_fuzz_theme_import(const uint8_t *data, size_t size);

int main(int argc, char **argv) {
  if (argc != 2) return 2;

  QFile seed(QString::fromLocal8Bit(argv[1]));
  if (!seed.open(QIODevice::ReadOnly)) return 3;
  const QByteArray bytes = seed.readAll();
  seed.close();

  const auto before = hnFuzzLifecycleSnapshot();
  if (hn_fuzz_theme_import(
          reinterpret_cast<const uint8_t *>(bytes.constData()),
          static_cast<size_t>(bytes.size())) != 0)
    return 4;
  const auto after = hnFuzzLifecycleSnapshot();

  const bool appWasDestroyed = after.appCreated - before.appCreated == 1 &&
                               after.appDestroyed - before.appDestroyed == 1 &&
                               QCoreApplication::instance() == nullptr;
  const bool tempWasRemoved = after.tempCreated - before.tempCreated == 1 &&
                              after.tempDestroyed - before.tempDestroyed == 1 &&
                              !after.lastTempPath.isEmpty() &&
                              !QFile::exists(after.lastTempPath);
  if (!appWasDestroyed || !tempWasRemoved) {
    std::fprintf(stderr,
                 "theme callback cleanup missing: appDestroyed=%llu "
                 "tempDestroyed=%llu appAlive=%d tempExists=%d\n",
                 static_cast<unsigned long long>(after.appDestroyed -
                                                before.appDestroyed),
                 static_cast<unsigned long long>(after.tempDestroyed -
                                                before.tempDestroyed),
                 QCoreApplication::instance() != nullptr,
                 QFile::exists(after.lastTempPath));
    return 1;
  }
  return 0;
}
