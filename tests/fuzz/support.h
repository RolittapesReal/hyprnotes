#pragma once
#include <cstddef>
#include <cstdint>
#include <QString>

void hnFuzzInit();
void hnFuzzShutdown();
QString hnFuzzTempPath();
using HnFuzzFunction = int (*)(const uint8_t *, size_t);

#ifdef HN_FUZZ_LIFECYCLE_DIAGNOSTIC
struct HnFuzzLifecycleSnapshot {
  std::uint64_t initCalls = 0;
  std::uint64_t tempCreated = 0;
  std::uint64_t tempDestroyed = 0;
  std::uint64_t appCreated = 0;
  std::uint64_t appDestroyed = 0;
  QString lastTempPath;
};
HnFuzzLifecycleSnapshot hnFuzzLifecycleSnapshot();
#endif

#define HN_FUZZ_TARGET(name) \
  extern "C" int LLVMFuzzerInitialize(int *, char ***) { hnFuzzInit(); return 0; } \
  extern "C" int hn_fuzz_##name(const uint8_t *data, size_t size); \
  extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) { \
    hnFuzzInit(); return hn_fuzz_##name(data, size); \
  }
