#include "support.h"
#include "hn/plugins/types.h"
extern "C" int hn_fuzz_manifest(const uint8_t *data, size_t size) {
  hn::plugins::Manifest manifest;
  QList<hn::plugins::PluginError> errors;
  hn::plugins::parseManifest(QByteArray(reinterpret_cast<const char *>(data), qsizetype(size)), hnFuzzTempPath(), &manifest, &errors);
  return 0;
}
#ifndef HN_FUZZ_REGRESSION_BUILD
HN_FUZZ_TARGET(manifest)
#endif
