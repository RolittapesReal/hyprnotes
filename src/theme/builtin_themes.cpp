#include "builtin_themes.h"
#include <QJsonArray>

namespace hn::theme::detail {
namespace {
struct Palette {
    const char *id;
    const char *bg, *surface, *text, *muted, *accent, *accentText;
    const char *border, *danger, *success, *selection, *yellow, *blue;
};

constexpr Palette kPalettes[]{
    {"catppuccin-mocha", "#1E1E2E", "#181825", "#CDD6F4", "#BAC2DE", "#CBA6F7", "#11111B",
        "#45475A", "#F38BA8", "#A6E3A1", "#313244", "#F9E2AF", "#89B4FA"},
    {"tokyo-night", "#1A1B26", "#24283B", "#C0CAF5", "#A9B1D6", "#7AA2F7", "#1A1B26",
        "#414868", "#F7768E", "#9ECE6A", "#292E42", "#E0AF68", "#7AA2F7"},
    {"dracula", "#282A36", "#21222C", "#F8F8F2", "#BFBFD3", "#BD93F9", "#282A36",
        "#6272A4", "#FF6E6E", "#50FA7B", "#44475A", "#F1FA8C", "#8BE9FD"},
    {"nord", "#2E3440", "#3B4252", "#ECEFF4", "#D8DEE9", "#88C0D0", "#2E3440",
        "#4C566A", "#E7A2AA", "#A3BE8C", "#434C5E", "#EBCB8B", "#81A1C1"},
    {"gruvbox-dark", "#282828", "#3C3836", "#EBDBB2", "#BDAE93", "#FABD2F", "#282828",
        "#665C54", "#FB7C6D", "#B8BB26", "#504945", "#FABD2F", "#83A598"},
    {"one-dark", "#282C34", "#21252B", "#ABB2BF", "#ABB2BF", "#61AFEF", "#21252B",
        "#4B5263", "#E99AA2", "#98C379", "#3E4451", "#E5C07B", "#61AFEF"},
};
} // namespace

QStringList builtinThemeIds() {
    QStringList ids{"modernist"};
    for (const auto &p : kPalettes) ids << QLatin1String(p.id);
    return ids;
}

QJsonObject presetThemeObject(const QString &canonicalId) {
    for (const auto &p : kPalettes) {
        if (canonicalId != QLatin1String(p.id)) continue;
        const QJsonObject dark{{"bg", p.bg}, {"surface", p.surface}, {"text", p.text}, {"muted", p.muted},
            {"accent", p.accent}, {"accentText", p.accentText}, {"border", p.border}, {"danger", p.danger},
            {"success", p.success}, {"selection", p.selection}, {"radius", 4}, {"borderWidth", 1},
            {"noteAccent", QJsonArray{p.danger, p.blue, p.yellow, p.success, p.border, p.text}}};
        return {{"version", 1}, {"name", canonicalId}, {"base", "modernist"}, {"dark", dark}};
    }
    return {};
}

} // namespace hn::theme::detail
