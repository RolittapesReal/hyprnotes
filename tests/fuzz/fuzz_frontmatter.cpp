#include "support.h"
#include "hn/core/frontmatter.h"
extern "C" int hn_fuzz_frontmatter(const uint8_t *data, size_t size) {
  hn::core::parseFrontmatter(QByteArray(reinterpret_cast<const char *>(data), qsizetype(size)));
  return 0;
}
#ifndef HN_FUZZ_REGRESSION_BUILD
HN_FUZZ_TARGET(frontmatter)
#endif
