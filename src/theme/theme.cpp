#include <algorithm>
#include <cmath>
#include <hn/theme/theme.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <QStyleFactory>
#include <QTemporaryDir>
#include "config.h"
#include <hn/theme/theme_io.h>

namespace hn::theme {
namespace {
QString g_error;

QString pickFamily(const QStringList &prefs, const QString &fallback) {
    if (!qGuiApp) return prefs.first();
    const QStringList have = QFontDatabase::families();
    for (const auto &p : prefs) if (have.contains(p)) return p;
    return fallback;
}

Theme modernist(bool dark) {
    Theme t;
    t.dark = dark;
    auto c = [](const char *h) { return QColor(QLatin1String(h)); };
    if (dark) {
        t.bg = c("#111111"); t.surface = c("#1A1A1A"); t.text = c("#F3F1EC"); t.muted = c("#A3A09A");
        t.accent = c("#FF5A43"); t.accentText = c("#111111"); t.border = c("#3A3A38");
        t.danger = c("#FF7A6B"); t.success = c("#5FBF7A"); t.selection = c("#262624");
        const char *n[6] = {"#E5533D", "#4F7FD9", "#E8B923", "#4FA36A", "#3A3A38", "#D8D5CC"};
        for (int i = 0; i < 6; ++i) t.noteAccent[i] = c(n[i]);
    } else {
        t.bg = c("#F3F1EC"); t.surface = c("#FBFAF7"); t.text = c("#111111"); t.muted = c("#5C5A55");
        t.accent = c("#D92E18"); t.accentText = c("#FFFFFF"); t.border = c("#C9C5BB");
        t.danger = c("#B3261E"); t.success = c("#1E7A3C"); t.selection = c("#E6E2D8");
        const char *n[6] = {"#D92E18", "#1F4FB5", "#F2B705", "#2E8B57", "#111111", "#FFFFFF"};
        for (int i = 0; i < 6; ++i) t.noteAccent[i] = c(n[i]);
    }
    t.padding = 16;   // 8px grid; header text in the organizer aligns with the editor text edge
    t.fontFamily = pickFamily({"Inter", "Noto Sans"}, "sans-serif");
    t.monoFamily = pickFamily({"JetBrains Mono"}, "monospace");
    return t;
}

bool color(const QJsonObject &o, const char *k, QColor &dst, QString &err) {
    if (!o.contains(k)) return true;
    const QString s = o[k].toString();
    const QColor c(s);
    if (!o[k].isString() || !s.startsWith('#') || !c.isValid()) { err = QString("token \"%1\": invalid color").arg(k); return false; }
    dst = c;
    return true;
}

bool applyTokens(const QJsonObject &o, Theme &t, QString &err) {
    struct { const char *k; QColor *d; } cs[] = {{"bg", &t.bg}, {"surface", &t.surface}, {"text", &t.text}, {"muted", &t.muted},
        {"accent", &t.accent}, {"accentText", &t.accentText}, {"border", &t.border}, {"danger", &t.danger},
        {"success", &t.success}, {"selection", &t.selection}};
    for (auto &e : cs) if (!color(o, e.k, *e.d, err)) return false;
    if (o.contains("noteAccent")) {
        const QJsonArray a = o["noteAccent"].toArray();
        if (a.size() != 6) { err = "token \"noteAccent\": need exactly 6 colors"; return false; }
        for (int i = 0; i < 6; ++i) {
            const QColor c(a[i].toString());
            if (!a[i].isString() || !c.isValid()) { err = "token \"noteAccent\": invalid color"; return false; }
            t.noteAccent[i] = c;
        }
    }
    for (auto [k, d] : {std::pair{"fontFamily", &t.fontFamily}, std::pair{"monoFamily", &t.monoFamily}})
        if (o.contains(k)) {
            const QString v = o[k].toString();   // D11: backslash/control chars/length would break the QSS string
            const bool bad = !o[k].isString() || v.trimmed().isEmpty() || v.size() > 200 || v.contains('"') || v.contains('\\')
                             || std::any_of(v.cbegin(), v.cend(), [](QChar c) { return c.unicode() < 0x20 || c.unicode() == 0x7f; });
            if (bad) { err = QString("token \"%1\": invalid").arg(k); return false; }
            *d = o[k].toString();
        }
    struct { const char *k; int *d; int lo, hi; } is[] = {{"baseSize", &t.baseSize, 8, 32}, {"padding", &t.padding, 0, 64},
        {"radius", &t.radius, 0, 32}, {"borderWidth", &t.borderWidth, 0, 4}};
    for (auto &e : is) if (o.contains(e.k)) {
        const int v = o[e.k].toInt(-1);
        if (!o[e.k].isDouble() || v < e.lo || v > e.hi) { err = QString("token \"%1\": must be %2..%3").arg(e.k).arg(e.lo).arg(e.hi); return false; }
        *e.d = v;
    }
    if (o.contains("lineHeight")) {
        const double v = o["lineHeight"].toDouble(-1);
        if (v < 1.0 || v > 3.0) { err = "token \"lineHeight\": must be 1.0..3.0"; return false; }
        t.lineHeight = v;
    }
    return true;
}

// Checked-box tick for QSS url(); written once per colour into a process-lifetime temp dir.
QString tickPath(const QColor &c) {
    static QTemporaryDir dir;
    if (!dir.isValid()) return {};
    const QString p = dir.filePath("tick-" + c.name().mid(1) + ".svg");
    if (!QFile::exists(p)) {
        QFile f(p);
        if (!f.open(QIODevice::WriteOnly)) return {};
        f.write(QString("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16' width='16' height='16'><path d='M3.5 8.5l3 3 6-7' "
                        "fill='none' stroke='%1' stroke-width='2' stroke-linecap='square'/></svg>").arg(c.name()).toUtf8());
    }
    return p;
}
} // namespace


double contrastRatio(const QColor &a, const QColor &b) {
    auto lum = [](const QColor &c) {
        auto ch = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
        return 0.2126 * ch(c.redF()) + 0.7152 * ch(c.greenF()) + 0.0722 * ch(c.blueF());
    };
    const double x = lum(a), y = lum(b);
    return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}

QString validateThemeObject(const QJsonObject &o, QStringList *warnings) {
    if (o["version"].toDouble(-1) != 1) return "unsupported or missing \"version\" (expected 1)";
    if (o.contains("base") && o["base"].toString() != "modernist") return "unknown base theme";
    bool any = false;
    for (bool dark : {false, true}) {
        const QString key = dark ? "dark" : "light";
        if (!o.contains(key)) continue;
        if (!o[key].isObject()) return key + ": scheme section must be an object";
        Theme t = modernist(dark);
        QString err;
        if (!applyTokens(o[key].toObject(), t, err)) return key + ": " + err;
        any = true;
        const double r = contrastRatio(t.text, t.bg);
        if (warnings && r < 4.5) warnings->append(QString("%1 scheme: text/background contrast is %2:1 (below 4.5:1, may be hard to read)").arg(key).arg(r, 0, 'f', 1));
    }
    return any ? QString() : QString("needs a \"light\" or \"dark\" section");
}

QJsonObject builtinThemeObject() {
    QJsonObject root{{"version", 1}, {"name", "modernist-copy"}, {"base", "modernist"}};
    for (bool dark : {false, true}) {
        const Theme t = modernist(dark);
        QJsonArray na;
        for (const QColor &c : t.noteAccent) na.append(c.name().toUpper());
        QJsonObject s{{"bg", t.bg.name().toUpper()}, {"surface", t.surface.name().toUpper()}, {"text", t.text.name().toUpper()},
            {"muted", t.muted.name().toUpper()}, {"accent", t.accent.name().toUpper()}, {"accentText", t.accentText.name().toUpper()},
            {"border", t.border.name().toUpper()}, {"danger", t.danger.name().toUpper()}, {"success", t.success.name().toUpper()},
            {"selection", t.selection.name().toUpper()}, {"noteAccent", na}, {"baseSize", t.baseSize}, {"lineHeight", t.lineHeight},
            {"padding", t.padding}, {"radius", t.radius}, {"borderWidth", t.borderWidth}};
        root[dark ? "dark" : "light"] = s;
    }
    return root;
}

QString lastThemeError() { return g_error; }

Theme loadTheme(const QString &name, bool dark) {
    g_error.clear();
    const Theme base = modernist(dark);
    if (name.isEmpty() || name == "modernist") return base;
    if (name.contains('/') || name.contains("..")) { g_error = "invalid theme name"; return base; }
    QFile f(configHome() + "/hyprnotes/themes/" + name + ".json");
    if (!f.open(QIODevice::ReadOnly)) { g_error = QString("theme \"%1\" not found").arg(name); return base; }
    QJsonParseError pe;
    const QJsonDocument d = QJsonDocument::fromJson(f.readAll(), &pe);
    if (!d.isObject()) { g_error = "malformed JSON: " + pe.errorString(); return base; }
    const QJsonObject o = d.object();
    if (o["version"].toInt(-1) != 1) { g_error = "unsupported or missing \"version\" (expected 1)"; return base; }
    if (o.contains("base") && o["base"].toString() != "modernist") { g_error = "unknown base theme"; return base; }
    Theme t = base;
    QString err;
    if (o.contains(dark ? "dark" : "light")) {
        const auto v = o[dark ? "dark" : "light"];
        if (!v.isObject()) { g_error = "scheme section must be an object"; return base; }
        if (!applyTokens(v.toObject(), t, err)) { g_error = err; return base; }
    }
    return t;
}

QFont labelFont(const Theme &t) {
    QFont f(t.fontFamily);
    f.setPixelSize(qMax(10, t.baseSize - 3));
    f.setWeight(QFont::Bold);
    f.setCapitalization(QFont::AllUppercase);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
    return f;
}

QString styleSheetFor(const Theme &t) {
    const QString q = QString(R"(
* { font-family: "@font@", "Noto Sans", sans-serif; }
QWidget { background: @bg@; color: @text@; selection-background-color: @accent@; selection-color: @accentText@; }
QDialog, QMainWindow, QStatusBar { background: @bg@; }
QStatusBar { border-top: @bw@px solid @border@; color: @muted@; }
QStatusBar::item { border: none; }
QLabel { background: transparent; }
QToolTip { background: @text@; color: @bg@; border: @bw@px solid @text@; padding: 4px 8px; }
QPushButton, QToolButton { background: @surface@; color: @text@; border: @bw@px solid @border@; border-radius: @r@px;
  padding: 3px 15px; min-height: 24px; font-weight: 600; }
QToolButton { padding: 3px 7px; min-width: 16px; background: transparent; border-color: transparent; }
QPushButton:hover, QToolButton:hover { background: @text@; color: @bg@; border-color: @text@; }
QPushButton:pressed, QToolButton:pressed { background: @accent@; color: @accentText@; border-color: @accent@; }
QPushButton:focus, QToolButton:focus { border: 2px solid @accent@; padding: 2px 14px; }
QToolButton:focus { padding: 2px 6px; }
QPushButton:default { border-color: @accent@; }
QPushButton:disabled, QToolButton:disabled { color: @muted@; background: transparent; border-color: @border@; }
QToolButton:checked { border-bottom: 2px solid @accent@; }
QLineEdit, QTextEdit, QPlainTextEdit, QSpinBox, QComboBox { background: @surface@; color: @text@; border: @bw@px solid @border@;
  border-radius: @r@px; padding: 3px 7px; min-height: 24px; }
QTextEdit, QPlainTextEdit { padding: @pad@px; }
QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus, QSpinBox:focus, QComboBox:focus { border: 2px solid @accent@; padding: 2px 6px; }
QTextEdit:focus, QPlainTextEdit:focus { padding: @padm@px; }
QComboBox::drop-down { border: none; width: 24px; }
QComboBox QAbstractItemView { background: @surface@; border: @bw@px solid @border@; selection-background-color: @selection@; selection-color: @text@; outline: 0; }
QMenu { background: @surface@; color: @text@; border: @bw@px solid @border@; padding: 4px 0; }
QMenu::item { padding: 4px 24px; border-left: 3px solid transparent; }
QMenu::item:selected { background: @selection@; border-left: 3px solid @accent@; }
QMenu::item:disabled { color: @muted@; }
QMenu::separator { height: @bw@px; background: @border@; margin: 4px 0; }
QMenuBar { background: @bg@; } QMenuBar::item:selected { background: @selection@; }
QListView, QTreeView, QTableView { background: @bg@; border: @bw@px solid @border@; outline: 0; alternate-background-color: @surface@; }
QListView::item, QTreeView::item { padding: 4px 8px; border-left: 3px solid transparent; }
QListView::item:hover, QTreeView::item:hover { background: @surface@; }
QListView::item:selected, QTreeView::item:selected { background: @selection@; color: @text@; border-left: 3px solid @accent@; }
QHeaderView::section { background: @bg@; color: @muted@; border: none; border-bottom: @bw@px solid @border@; padding: 4px 8px; font-weight: 700; }
QTabWidget::pane { border: @bw@px solid @border@; top: -1px; }
QTabBar::tab { background: transparent; color: @muted@; padding: 4px 16px; border: none; border-bottom: 2px solid transparent; font-weight: 700; }
QTabBar::tab:hover { color: @text@; }
QTabBar::tab:selected { color: @text@; border-bottom: 2px solid @accent@; }
QTabBar::tab:focus { border-bottom: 2px solid @accent@; }
QCheckBox { spacing: 8px; background: transparent; }
QCheckBox::indicator { width: 16px; height: 16px; border: @bw@px solid @text@; background: @surface@; border-radius: 0; }
QCheckBox::indicator:checked { background: @accent@; border-color: @accent@; image: url(@tick@); }
QCheckBox:focus { outline: 2px solid @accent@; }
QScrollBar:vertical { background: transparent; width: 8px; margin: 0; border: none; }
QScrollBar:horizontal { background: transparent; height: 8px; margin: 0; border: none; }
QScrollBar::handle { background: @border@; border-radius: 0; min-height: 24px; min-width: 24px; }
QScrollBar::handle:hover { background: @muted@; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; background: none; border: none; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QSplitter::handle { background: @border@; }
)");
    QString s = q;
    const QHash<QString, QString> m{{"font", t.fontFamily}, {"bg", t.bg.name()}, {"surface", t.surface.name()}, {"text", t.text.name()},
        {"muted", t.muted.name()}, {"accent", t.accent.name()}, {"accentText", t.accentText.name()}, {"border", t.border.name()},
        {"selection", t.selection.name()}, {"bw", QString::number(t.borderWidth)}, {"r", QString::number(t.radius)},
        {"pad", QString::number(t.padding)}, {"padm", QString::number(qMax(0, t.padding - 1))}, {"tick", tickPath(t.accentText)}};
    // longest keys first so "accentText" is not clobbered by "accent"
    QStringList keys = m.keys();
    std::sort(keys.begin(), keys.end(), [](const QString &a, const QString &b) { return a.size() > b.size(); });
    for (const auto &k : keys) s.replace('@' + k + '@', m[k]);
    s.replace("image: url();", "");
    return s;
}

namespace {
// D10: qApp->setStyleSheet() re-polishes every widget of every window in one blocking call (~19 ms/window). The sheet
// is instead set per top-level window: visible windows in time-boxed slices (the first slice runs synchronously),
// hidden ones when they are next shown (event filter), so the GUI thread is never blocked for long.
QString g_sheet;
quint64 g_gen = 0;
constexpr int kSliceMs = 25;
const char *kGenProp = "hnSheetGen";

void applySheetTo(QWidget *w) {
    if (w->property(kGenProp).toULongLong() == g_gen) return;
    w->setProperty(kGenProp, quint64(g_gen));
    if (w->styleSheet() != g_sheet) w->setStyleSheet(g_sheet);
}

void sheetSlice(quint64 gen) {
    if (gen != g_gen) return;   // superseded by a newer theme
    QElapsedTimer t; t.start();
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (!w->isVisible()) continue;   // hidden: applied on show
        applySheetTo(w);
        if (t.elapsed() >= kSliceMs) break;
    }
    for (QWidget *w : QApplication::topLevelWidgets())
        if (w->isVisible() && w->property(kGenProp).toULongLong() != g_gen) { QTimer::singleShot(0, qApp, [gen] { sheetSlice(gen); }); return; }
}

struct ShowFilter : QObject {
    bool eventFilter(QObject *o, QEvent *e) override {
        if (e->type() == QEvent::Show && o->isWidgetType()) { auto *w = static_cast<QWidget *>(o); if (w->isWindow()) applySheetTo(w); }
        return false;
    }
};

void stageSheet(const QString &sheet) {
    static ShowFilter *filter = nullptr;
    if (!filter) { filter = new ShowFilter; filter->setParent(qApp); qApp->installEventFilter(filter); }
    if (!qApp->styleSheet().isEmpty()) qApp->setStyleSheet({});   // legacy app-wide sheet: one-time cost
    g_sheet = sheet;
    sheetSlice(++g_gen);
}
} // namespace

void applyTheme(const Theme &t) {
    if (!qApp) return;
    static bool styled = false;   // setStyle() re-polishes every widget: once is enough
    if (!styled) { styled = true; if (auto *fusion = QStyleFactory::create("Fusion")) qApp->setStyle(fusion); }
    QPalette p;
    p.setColor(QPalette::Window, t.bg); p.setColor(QPalette::WindowText, t.text);
    p.setColor(QPalette::Base, t.surface); p.setColor(QPalette::AlternateBase, t.bg);
    p.setColor(QPalette::Text, t.text); p.setColor(QPalette::Button, t.surface); p.setColor(QPalette::ButtonText, t.text);
    p.setColor(QPalette::ToolTipBase, t.text); p.setColor(QPalette::ToolTipText, t.bg);
    p.setColor(QPalette::Highlight, t.accent); p.setColor(QPalette::HighlightedText, t.accentText);
    p.setColor(QPalette::PlaceholderText, t.muted); p.setColor(QPalette::Link, t.accent);
    p.setColor(QPalette::Mid, t.border); p.setColor(QPalette::Dark, t.border);
    p.setColor(QPalette::Disabled, QPalette::Text, t.muted); p.setColor(QPalette::Disabled, QPalette::ButtonText, t.muted);
    qApp->setPalette(p);
    QFont f(t.fontFamily);
    f.setPixelSize(t.baseSize);
    qApp->setFont(f);
    QFont tab = labelFont(t);
    qApp->setFont(tab, "QTabBar");
    stageSheet(styleSheetFor(t));
}

} // namespace hn::theme
