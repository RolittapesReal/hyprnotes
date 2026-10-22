#include "support.h"
#include "hn/core/library_index.h"
#include <QTemporaryDir>
#include <cstdint>
#include <cstddef>
namespace {
struct FuzzInputScope {
  ~FuzzInputScope() { hnFuzzShutdown(); }
};
}
extern "C" int hn_fuzz_fts_query(const uint8_t *data, size_t size) {
  FuzzInputScope scope;
  hnFuzzInit();
  QTemporaryDir dbDir;
  hn::core::Worker worker;
  hn::core::LibraryIndex index(hnFuzzTempPath(), dbDir.path(), nullptr, &worker);
  hn::core::IndexQuery query;
  query.text = QString::fromUtf8(reinterpret_cast<const char *>(data), qsizetype(size));
  query.limit = 1;
  index.searchNow(query);
  return 0;
}
#ifndef HN_FUZZ_REGRESSION_BUILD
extern "C" int LLVMFuzzerInitialize(int *, char ***) {
  // Warm the Qt SQL/SQLite path before libFuzzer attributes PCs to individual inputs.
  // Otherwise lazy driver setup can make retained-corpus coverage depend on input order.
  static constexpr uint8_t warmup[] = {'w', 'a', 'r', 'm', 'u', 'p'};
  return hn_fuzz_fts_query(warmup, sizeof(warmup));
}
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  return hn_fuzz_fts_query(data, size);
}
#endif
