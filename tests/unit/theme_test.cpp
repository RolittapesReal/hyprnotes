#include <QtTest>
#include <QApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cmath>
#include <hn/theme/animation.h>
#include <hn/theme/theme.h>
using namespace hn::theme;

static double lum(const QColor &c) {
    auto f = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * f(c.redF()) + 0.7152 * f(c.greenF()) + 0.0722 * f(c.blueF());
}
static double contrast(const QColor &a, const QColor &b) {
    double x = lum(a), y = lum(b);
    if (x < y) std::swap(x, y);
    return (x + 0.05) / (y + 0.05);
}

class ThemeTest : public QObject {
    Q_OBJECT
    QTemporaryDir cfg;
    void writeTheme(const QString &name, const QByteArray &json) {
        QDir().mkpath(cfg.path() + "/hyprnotes/themes");
        QFile f(cfg.path() + "/hyprnotes/themes/" + name + ".json");
        QVERIFY(f.open(QIODevice::WriteOnly)); f.write(json);
    }
private slots:
    void initTestCase() { qputenv("XDG_CONFIG_HOME", cfg.path().toUtf8()); }

    void contrastAA_data() { QTest::addColumn<bool>("dark"); QTest::newRow("light") << false; QTest::newRow("dark") << true; }
    void contrastAA() {
        QFETCH(bool, dark);
        const Theme t = loadTheme("modernist", dark);
        QCOMPARE(t.dark, dark);
        QVERIFY2(contrast(t.text, t.bg) >= 4.5, "text/bg");
        QVERIFY2(contrast(t.text, t.surface) >= 4.5, "text/surface");
        QVERIFY2(contrast(t.muted, t.bg) >= 4.5, "muted/bg");
        QVERIFY2(contrast(t.muted, t.surface) >= 4.5, "muted/surface");
        QVERIFY2(contrast(t.accentText, t.accent) >= 4.5, "accentText/accent");
        QVERIFY2(contrast(t.accent, t.bg) >= 3.0, "accent focus ring vs bg (non-text 3:1)");
        QCOMPARE(t.radius, 4);
        QCOMPARE(t.borderWidth, 1);
        QCOMPARE(t.padding % 2, 0);
    }
    void stylesheetHasNoEffects() {
        const QString s = styleSheetFor(loadTheme("modernist", false));
        for (const char *w : std::initializer_list<const char *>{"QPushButton", "QToolButton", "QLineEdit", "QMenu", "QScrollBar", "QComboBox", "QListView", "QTreeView", "QTabBar", "QToolTip", "QCheckBox", "QDialog", "QStatusBar"})
            QVERIFY2(s.contains(w), w);
        QVERIFY(!s.contains("gradient")); QVERIFY(!s.contains("shadow")); QVERIFY(!s.contains("@"));
        QVERIFY(s.contains(QLatin1String("d92e18")));
    }
    void customRadiiKeepSquareAndDerivePopups() {
        QCOMPARE(Theme{}.radius, 0);
        for (const auto [radius, popup] : {std::pair{0, 0}, std::pair{10, 12}, std::pair{32, 32}}) {
            writeTheme("radii", QJsonDocument(QJsonObject{{"version", 1}, {"light", QJsonObject{{"radius", radius}}}}).toJson());
            const Theme theme = loadTheme("radii", false);
            QVERIFY(lastThemeError().isEmpty());
            QCOMPARE(theme.radius, radius);
            QCOMPARE(popupRadius(theme), popup);
        }
    }
    void loadBuiltinAndOverride() {
        writeTheme("mine", "{\"version\":1,\"base\":\"modernist\",\"light\":{\"accent\":\"#0055FF\",\"baseSize\":16},\"dark\":{\"bg\":\"#000000\"}}");
        Theme l = loadTheme("mine", false);
        QVERIFY2(lastThemeError().isEmpty(), qPrintable(lastThemeError()));
        QCOMPARE(l.accent, QColor("#0055FF")); QCOMPARE(l.baseSize, 16);
        QCOMPARE(l.bg, loadTheme("modernist", false).bg);          // partial: inherited
        QCOMPARE(loadTheme("mine", true).bg, QColor("#000000"));
        QCOMPARE(loadTheme("mine", true).accent, loadTheme("modernist", true).accent);
    }
    void invalidFallsBackToBase_data() {
        QTest::addColumn<QByteArray>("json");
        QTest::newRow("malformed") << QByteArray("{not json");
        QTest::newRow("version") << QByteArray("{\"version\":2}");
        QTest::newRow("bad color") << QByteArray("{\"version\":1,\"light\":{\"accent\":\"red-ish\"}}");
        QTest::newRow("bad range") << QByteArray("{\"version\":1,\"light\":{\"baseSize\":500}}");
        QTest::newRow("bad note accents") << QByteArray("{\"version\":1,\"light\":{\"noteAccent\":[\"#fff\"]}}");
        QTest::newRow("bad base") << QByteArray("{\"version\":1,\"base\":\"other\"}");
    }
    void invalidFallsBackToBase() {
        QFETCH(QByteArray, json);
        writeTheme("bad", json);
        const Theme t = loadTheme("bad", false);
        QVERIFY(!lastThemeError().isEmpty());
        QCOMPARE(t.accent, loadTheme("modernist", false).accent);
        loadTheme("modernist", false);
        QVERIFY(lastThemeError().isEmpty());                         // cleared on success
    }
    void missingAndTraversal() {
        loadTheme("nope", false); QVERIFY(!lastThemeError().isEmpty());
        loadTheme("../x", false); QVERIFY(!lastThemeError().isEmpty());
    }
    void iconsAllNamesAndDpr() {
        const QStringList names{"bold","italic","strike","code","h1","list-ul","list-ol","check","quote","link","close","pin","popout",
                                "popin","search","plus","more","tag","folder","source","visual"};
        for (const auto &n : names) {
            QIcon ic = icon(n, Qt::black);
            QVERIFY2(!ic.isNull(), qPrintable(n));
            for (qreal d : {1.0, 1.25, 1.5, 2.0}) {
                QPixmap pm = ic.pixmap(QSize(16, 16), d);
                QCOMPARE(pm.size(), QSize(qRound(16 * d), qRound(16 * d)));
                QImage im = pm.toImage().convertToFormat(QImage::Format_ARGB32);
                bool ink = false;
                for (int y = 0; y < im.height() && !ink; ++y) for (int x = 0; x < im.width(); ++x) if (qAlpha(im.pixel(x, y)) > 128) { ink = true; break; }
                QVERIFY2(ink, qPrintable(n));
            }
        }
        QVERIFY(icon("nonexistent", Qt::black).isNull());
        // tint applied, and cache returns the same icon
        QImage im = icon("plus", QColor("#ff0000")).pixmap(16).toImage();
        QVERIFY(QColor(im.pixel(8, 8)).red() > 200 && QColor(im.pixel(8, 8)).green() < 60);
        QCOMPARE(icon("plus", Qt::red).cacheKey(), icon("plus", Qt::red).cacheKey());
    }
    void animationPolicy() {
        AnimationPolicy p;
        QCOMPARE(p.duration(10), 100); QCOMPARE(p.duration(120), 120); QCOMPARE(p.duration(900), 150);
        p.reduceMotion = true; QCOMPARE(p.duration(120), 0);
    }
    void fontFamilyThatBreaksStylesheetIsRejected() {   // D11
        for (const QString &ff : {QString("Foo\\"), QString("Foo\nBar"), QString(QChar(1)), QString("Foo\"x")}) {
            const QByteArray tok = QJsonDocument(QJsonObject{{"fontFamily", ff}}).toJson(QJsonDocument::Compact);
            writeTheme("badff", "{\"version\":1,\"dark\":" + tok + ",\"light\":" + tok + "}");
            loadTheme("badff", true);
            QVERIFY2(!lastThemeError().isEmpty(), qPrintable(ff));
        }
        writeTheme("okff", "{\"version\":1,\"dark\":{\"fontFamily\":\"Noto Sans\"}}");
        loadTheme("okff", true);
        QVERIFY(lastThemeError().isEmpty());
    }
    void applyDoesNotCrash() {
        applyTheme(loadTheme("modernist", true));
        QCOMPARE(qApp->palette().color(QPalette::Window), QColor("#111111"));
    }
};
int main(int argc, char **argv) { QApplication a(argc, argv); ThemeTest t; return QTest::qExec(&t, argc, argv); }
#include "theme_test.moc"
