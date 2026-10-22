#include "support.h"
#include "hn/core/query_engine.h"
#include <QJsonDocument>
extern "C" int hn_fuzz_query_spec(const uint8_t *data, size_t size) {
  const auto doc = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char *>(data), qsizetype(size)));
  if (doc.isObject()) hn::core::QueryEngine::compile(doc.object());
  return 0;
}
#ifndef HN_FUZZ_REGRESSION_BUILD
HN_FUZZ_TARGET(query_spec)
#endif
