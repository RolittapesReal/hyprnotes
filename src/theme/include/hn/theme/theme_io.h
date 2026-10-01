// Theme import/export/list/remove. User themes live in themesDir() as <name>.json (schema: docs/theme-reference.md).
#pragma once
#include <QColor>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace hn::theme {

constexpr qint64 kMaxThemeBytes = 64 * 1024;

QString themesDir();                              // $XDG_CONFIG_HOME/hyprnotes/themes
bool validThemeName(const QString &name);         // [A-Za-z0-9][A-Za-z0-9._-]{0,47}, no ".."
QString sanitizeThemeName(const QString &raw);    // empty when nothing usable is left
double contrastRatio(const QColor &a, const QColor &b);   // WCAG
// "" when valid; otherwise the reason. Contrast problems are only appended to *warnings.
QString validateThemeObject(const QJsonObject &o, QStringList *warnings);
QJsonObject builtinThemeObject();                 // built-in modernist as a Hyprnotes theme (name "modernist-copy")

// Accepts Hyprnotes theme JSON, base16 YAML or a VS Code color theme JSON. Installs atomically under a sanitized unique
// name (never overwrites a different theme: "-2", "-3"... suffix; an identical theme is reused). *nameOut = installed name.
bool importTheme(const QString &srcPath, QString *nameOut, QString *errorOut, QStringList *warningsOut = nullptr);
bool exportTheme(const QString &name, const QString &destPath, QString *errorOut = nullptr);   // "modernist" exports the built-in
QStringList listThemes();                         // "modernist" first, then user themes sorted
bool removeTheme(const QString &name, QString *errorOut = nullptr);   // user themes only

// Foreign converters: strict, produce a Hyprnotes theme object (not yet validated) and a suggested name.
bool convertBase16(const QByteArray &yaml, QJsonObject *out, QString *nameOut, QString *errorOut);
bool convertVsCode(const QJsonObject &doc, QJsonObject *out, QString *nameOut, QString *errorOut);

} // namespace hn::theme
