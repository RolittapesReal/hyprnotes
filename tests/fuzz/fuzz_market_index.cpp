#include "support.h"
#include "hn/plugins/market.h"
extern "C" int hn_fuzz_market_index(const uint8_t *data, size_t size) {
  hn::plugins::MarketIndex index;
  QString error;
  hn::plugins::parseMarketIndex(QByteArray(reinterpret_cast<const char *>(data), qsizetype(size)), &index, &error);
  return 0;
}
#ifndef HN_FUZZ_REGRESSION_BUILD
HN_FUZZ_TARGET(market_index)
#endif
