#pragma once
#include <QJsonObject>
#include <QStringList>

namespace hn::theme::detail {

QStringList builtinThemeIds();
// Native version-1 JSON, base=modernist, dark section only.
// Empty for modernist or an unknown ID; callers resolve case before lookup.
QJsonObject presetThemeObject(const QString &canonicalId);

} // namespace hn::theme::detail
