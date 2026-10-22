#include "support.h"

#include <QCoreApplication>

#include <cstdint>
#include <cstdio>

extern "C" int hn_fuzz_fts_query(const uint8_t *, size_t);

int main() {
  constexpr uint8_t input[] = {'s', 'e', 'a', 'r', 'c', 'h'};
  if (QCoreApplication::instance()) {
    std::fputs("application existed before first FTS callback\n", stderr);
    return 10;
  }

  for (int iteration = 1; iteration <= 2; ++iteration) {
    const auto before = hnFuzzLifecycleSnapshot();
    if (hn_fuzz_fts_query(input, sizeof(input)) != 0) return 11;
    const auto after = hnFuzzLifecycleSnapshot();
    const auto appCreateDelta = after.appCreated - before.appCreated;
    const auto appDestroyDelta = after.appDestroyed - before.appDestroyed;
    const auto tempCreateDelta = after.tempCreated - before.tempCreated;
    const auto tempDestroyDelta = after.tempDestroyed - before.tempDestroyed;
    if (appCreateDelta != 1 || appDestroyDelta != 1 ||
        tempCreateDelta != 1 || tempDestroyDelta != 1 ||
        QCoreApplication::instance()) {
      std::fprintf(stderr,
                   "FTS callback %d did not reset its app/temp lifecycle: "
                   "app_create=%llu app_destroy=%llu temp_create=%llu "
                   "temp_destroy=%llu app_alive=%d\n",
                   iteration,
                   static_cast<unsigned long long>(appCreateDelta),
                   static_cast<unsigned long long>(appDestroyDelta),
                   static_cast<unsigned long long>(tempCreateDelta),
                   static_cast<unsigned long long>(tempDestroyDelta),
                   QCoreApplication::instance() != nullptr);
      return 20 + iteration;
    }
  }
  return 0;
}
