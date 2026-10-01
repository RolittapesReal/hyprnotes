#pragma once
#include <QString>
#include <md4c.h>

namespace hn::core::detail {
QString decodeEntity(const MD_CHAR *text, MD_SIZE size);
QString attrText(const MD_ATTRIBUTE &a);   // decoded link href/title
inline QString u8(const MD_CHAR *t, MD_SIZE n) { return QString::fromUtf8(t, qsizetype(n)); }
}
