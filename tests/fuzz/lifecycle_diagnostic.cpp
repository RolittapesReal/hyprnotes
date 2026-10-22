#include "support.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QObject>
#include <QPointer>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

extern "C" int hn_fuzz_markdown_roundtrip(const uint8_t *, size_t);

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  std::ifstream input(argv[1], std::ios::binary);
  if (!input) return 2;
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};

  if (QCoreApplication::instance()) {
    std::fputs("application existed before first callback\n", stderr);
    return 10;
  }
  for (int iteration = 1; iteration <= 2; ++iteration) {
    const auto before = hnFuzzLifecycleSnapshot();
    if (QCoreApplication::instance()) return 11;
    if (hn_fuzz_markdown_roundtrip(bytes.data(), bytes.size()) != 0) return 12;
    const auto after = hnFuzzLifecycleSnapshot();
    const bool appAlive = QCoreApplication::instance() != nullptr;
    const bool tempExists = !after.lastTempPath.isEmpty() &&
                            QDir(after.lastTempPath).exists();
    const auto initDelta = after.initCalls - before.initCalls;
    const auto appCreateDelta = after.appCreated - before.appCreated;
    const auto appDestroyDelta = after.appDestroyed - before.appDestroyed;
    const auto tempCreateDelta = after.tempCreated - before.tempCreated;
    const auto tempDestroyDelta = after.tempDestroyed - before.tempDestroyed;
    std::fprintf(stderr,
                 "callback=%d init_delta=%llu app_create_delta=%llu "
                 "app_destroy_delta=%llu temp_create_delta=%llu "
                 "temp_destroy_delta=%llu app_alive=%d temp_exists=%d\n",
                 iteration, static_cast<unsigned long long>(initDelta),
                 static_cast<unsigned long long>(appCreateDelta),
                 static_cast<unsigned long long>(appDestroyDelta),
                 static_cast<unsigned long long>(tempCreateDelta),
                 static_cast<unsigned long long>(tempDestroyDelta),
                 int(appAlive), int(tempExists));
    std::fflush(stderr);
    if (initDelta != 1 || appCreateDelta != 1 || appDestroyDelta != 1 ||
        tempCreateDelta != 1 || tempDestroyDelta != 1 || appAlive ||
        tempExists) {
      std::fprintf(stderr, "callback %d did not own full app/temp lifecycle\n",
                   iteration);
      return 20 + iteration;
    }
  }

  int externalArgc = 1;
  char externalAppName[] = "hyprnotes-fuzz-external-app";
  char *externalArgv[] = {externalAppName, nullptr};
  QApplication externalApp(externalArgc, externalArgv);
  auto *callerOwnedObject = new QObject;
  QPointer<QObject> callerOwnedObjectGuard(callerOwnedObject);
  callerOwnedObject->deleteLater();
  const auto before = hnFuzzLifecycleSnapshot();
  if (hn_fuzz_markdown_roundtrip(bytes.data(), bytes.size()) != 0) return 13;
  const auto after = hnFuzzLifecycleSnapshot();
  const bool externalAppPreserved = QCoreApplication::instance() == &externalApp;
  const bool callerObjectPreserved = !callerOwnedObjectGuard.isNull();
  const bool tempRemoved = !after.lastTempPath.isEmpty() &&
                           !QDir(after.lastTempPath).exists();
  if (callerOwnedObjectGuard)
    QCoreApplication::sendPostedEvents(callerOwnedObjectGuard,
                                       QEvent::DeferredDelete);
  if (after.initCalls - before.initCalls != 1 ||
      after.appCreated - before.appCreated != 0 ||
      after.appDestroyed - before.appDestroyed != 0 ||
      after.tempCreated - before.tempCreated != 1 ||
      after.tempDestroyed - before.tempDestroyed != 1 ||
      !externalAppPreserved || !callerObjectPreserved || !tempRemoved) {
    std::fprintf(stderr,
                 "external application/caller object ownership was not preserved\n");
    return 40;
  }
  return 0;
}
