#pragma once
#include <QString>
#include <md4c.h>

namespace hn::core::detail {
QString decodeEntity(const MD_CHAR *text, MD_SIZE size);
QString attrText(const MD_ATTRIBUTE &a);   // decoded link href/title
// QString::fromUtf8 silently drops a leading U+FEFF. md4c hands text out in pieces (a backslash escape ends one), so a
// piece can start with a BOM that is literal content: keep it, otherwise export -> import -> export loses characters.
inline QString u8(const MD_CHAR *t, MD_SIZE n) {
    qsizetype size = qsizetype(n);
    qsizetype boms = 0;
    while (size - boms * 3 >= 3 && static_cast<uchar>(t[boms * 3]) == 0xEF && static_cast<uchar>(t[boms * 3 + 1]) == 0xBB
           && static_cast<uchar>(t[boms * 3 + 2]) == 0xBF)
        ++boms;
    QString out(boms, QChar(0xFEFF));
    out += QString::fromUtf8(t + boms * 3, size - boms * 3);
    return out;
}
}
