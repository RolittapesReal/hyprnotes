// Pinned cross-module contract. Implemented in src/theme/.
#pragma once
#include <QColor>
#include <QString>
#include <QIcon>
#include <QWidget>
#include <QFont>

namespace hn::theme {

struct Theme {
    bool dark = false;
    QColor bg, surface, text, muted, accent, accentText, border, danger, success, selection;
    QColor noteAccent[6];          // per-note flat accent colors (index stored in note state)
    QString fontFamily;            // UI + body text
    QString monoFamily;            // code
    int baseSize = 14;             // px
    qreal lineHeight = 1.45;
    int padding = 14;              // px, editor padding
    int radius = 0;                // px, zero explicitly requests square controls
    int borderWidth = 1;           // px hairline
};

// name: a built-in ID (case-insensitive, empty aliases modernist) or a user theme file under themesDir().
// dark: follow system scheme when std::nullopt callers pass QGuiApplication colorScheme result.
Theme loadTheme(const QString &name, bool dark);

// Applies palette/font defaults and staged top-level QSS (flat, opaque, square document surfaces).
void applyTheme(const Theme &theme);

// Stylesheet fragment usable by individual widgets that need extra rules.
QString styleSheetFor(const Theme &theme);

// Popup corners derive from control radius; explicit zero keeps them square.
int popupRadius(const Theme &theme);

// Flat monochrome SVG icon tinted with the theme text color: "bold","italic","strike","code","h1","list-ul",
// "list-ol","check","quote","link","close","pin","popout","popin","search","plus","more","tag","folder","source","visual".
QIcon icon(const QString &name, const QColor &tint, int px = 16);

// Additive: last loadTheme() failure ("" if the last load succeeded). On failure loadTheme returns the built-in base.
QString lastThemeError();

// Additive: bold, small, tracked uppercase label font (QSS cannot express letter-spacing/uppercase).
QFont labelFont(const Theme &theme);

} // namespace hn::theme
