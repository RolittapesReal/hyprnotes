#include <QtTest>
#include <QDir>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <hn/theme/theme.h>
#include <hn/theme/theme_io.h>
#include "theme_fixtures.h"
using namespace hn::theme;

static const QByteArray kNative = J("{'version':1,'name':'Mine','light':{'accent':'#1F4FB5'},'dark':{}}");

class ThemeIoTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir;
    QString src(const QString &file, const QByteArray &bytes) {
        QFile f(dir.filePath(file));
        if (f.open(QIODevice::WriteOnly)) f.write(bytes);
        return f.fileName();
    }
    bool imp(const QString &file, QString *name = nullptr, QString *err = nullptr, QStringList *w = nullptr) {
        QString n, e;
        const bool ok = importTheme(file, name ? name : &n, err ? err : &e, w);
        return ok;
    }
private slots:
    void init() {
        QDir(dir.path()).removeRecursively();
        QDir().mkpath(dir.path());
        qputenv("XDG_CONFIG_HOME", (dir.path() + "/xdg").toUtf8());
    }
    void validImportInstallsAndLoads() {
        QString name, err;
        QVERIFY2(imp(src("a.json", kNative), &name, &err), qPrintable(err));
        QCOMPARE(name, QString("Mine"));
        QVERIFY(QFile::exists(themesDir() + "/Mine.json"));
        QCOMPARE(loadTheme("Mine", false).accent, QColor("#1F4FB5"));
        QVERIFY(lastThemeError().isEmpty());
        QCOMPARE(listThemes(), (QStringList{"modernist", "Mine"}));
    }
    void invalidInputsRejected_data() {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<QString>("msg");
        QTest::newRow("malformed") << QByteArray("{nope") << "malformed";
        QTest::newRow("version") << J("{'version':2,'light':{}}") << "version";
        QTest::newRow("no scheme") << J("{'version':1}") << "light";
        QTest::newRow("bad color") << J("{'version':1,'light':{'bg':'red'}}") << "invalid color";
        QTest::newRow("unsafe font") << J("{'version':1,'light':{'fontFamily':'a\\\\b'}}") << "fontFamily"; // brace chars are fine; backslash/quote/control are not (below)
        QTest::newRow("range") << J("{'version':1,'light':{'baseSize':99}}") << "baseSize";
        QTest::newRow("traversal") << J("{'version':1,'name':'../../evil','light':{}}") << "unsafe";
        QTest::newRow("slash") << J("{'version':1,'name':'a/b','light':{}}") << "unsafe";
        QTest::newRow("reserved") << J("{'version':1,'name':'modernist','light':{}}") << "built-in";
        QTest::newRow("unknown json") << J("{'hello':1}") << "unrecognized";
        QTest::newRow("unknown text") << QByteArray("hello: world\n") << "unrecognized";
    }
    void invalidInputsRejected() {
        QFETCH(QByteArray, bytes); QFETCH(QString, msg);
        QString err;
        QVERIFY(!imp(src("bad.json", bytes), nullptr, &err));
        QVERIFY2(err.contains(msg), qPrintable(err));
        QVERIFY(listThemes() == QStringList{"modernist"});
        QVERIFY(!QFile::exists(dir.path() + "/evil.json"));
    }
    void oversizeRejected() {
        QString err;
        QVERIFY(!imp(src("big.json", J("{'version':1,'light':{},'pad':'") + QByteArray(70000, 'x') + "\"}"), nullptr, &err));
        QVERIFY2(err.contains("larger"), qPrintable(err));
        QVERIFY(!imp(dir.filePath("missing.json"), nullptr, &err));
    }
    void lowContrastWarnsButImports() {
        QStringList w; QString err;
        QVERIFY(imp(src("lc.json", J("{'version':1,'name':'lc','light':{'bg':'#FFFFFF','text':'#DDDDDD'}}")), nullptr, &err, &w));
        QCOMPARE(w.size(), 1);
        QVERIFY(w[0].contains("contrast"));
    }
    void duplicateNeverOverwritesSilently() {
        QString n1, n2, n3;
        QStringList w;
        QVERIFY(imp(src("a.json", kNative), &n1));
        QVERIFY(imp(src("b.json", kNative), &n2, nullptr, &w));            // identical: reused
        QCOMPARE(n2, n1);
        QVERIFY(w.join(' ').contains("already installed"));
        QVERIFY(imp(src("c.json", J("{'version':1,'name':'Mine','light':{'accent':'#000000'}}")), &n3, nullptr, &w));
        QCOMPARE(n3, QString("Mine-2"));
        QVERIFY(w.join(' ').contains("Mine-2"));
        QCOMPARE(loadTheme("Mine", false).accent, QColor("#1F4FB5"));      // original untouched
        QCOMPARE(loadTheme("Mine-2", false).accent, QColor("#000000"));
    }
    void base16Converts() {
        QString name, err;
        QVERIFY2(imp(src("t.yaml", kBase16), &name, &err), qPrintable(err));
        QCOMPARE(name, QString("Test-Dark"));
        const Theme d = loadTheme(name, true);
        QCOMPARE(d.bg, QColor("#181818")); QCOMPARE(d.text, QColor("#D8D8D8")); QCOMPARE(d.accent, QColor("#7CAFC2"));
        QCOMPARE(d.danger, QColor("#AB4642")); QCOMPARE(d.noteAccent[2], QColor("#F7CA88"));
        QVERIFY(lastThemeError().isEmpty());
        QCOMPARE(loadTheme(name, false).bg, loadTheme("modernist", false).bg);   // dark-only theme: light inherits the base
    }
    void base16LightGoesToLightSection() {
        QByteArray y = kBase16;
        y.replace("base00: \"181818\"", "base00: \"f8f8f8\"");
        QString name;
        QVERIFY(imp(src("l.yml", y), &name));
        QCOMPARE(loadTheme(name, false).bg, QColor("#F8F8F8"));
    }
    void base16Strict() {
        QString err;
        QByteArray y = kBase16;
        y.replace("base0F: \"a16946\"\n", "");
        QVERIFY(!imp(src("m.yaml", y), nullptr, &err));
        QVERIFY2(err.contains("base0f"), qPrintable(err));
        y = kBase16;
        y.replace("base05: \"d8d8d8\"", "base05: \"zzzzzz\"");
        QVERIFY(!imp(src("n.yaml", y), nullptr, &err));
    }
    void vscodeConverts() {
        QString name, err;
        QVERIFY2(imp(src("v.json", kVsCode), &name, &err), qPrintable(err));
        QCOMPARE(name, QString("My-VS"));
        const Theme t = loadTheme(name, false);
        QCOMPARE(t.bg, QColor("#FFFFFF")); QCOMPARE(t.text, QColor("#333333")); QCOMPARE(t.accent, QColor("#007FD4"));
        QCOMPARE(t.selection, QColor("#ADD6FF")); QCOMPARE(t.surface, QColor("#F3F3F3"));
        QVERIFY(!imp(src("v2.json", J("{'colors':{'editor.background':'#000'}}")), nullptr, &err));
        QVERIFY2(err.contains("editor.foreground"), qPrintable(err));
    }
    void exportRoundTrip() {
        QString name;
        QVERIFY(imp(src("a.json", kNative), &name));
        const QString out = dir.filePath("out/exp.json");
        QVERIFY(exportTheme(name, out));
        QVERIFY(removeTheme(name));
        QString n2, err;
        QVERIFY2(imp(out, &n2, &err), qPrintable(err));
        QCOMPARE(n2, name);
        QCOMPARE(loadTheme(n2, false).accent, QColor("#1F4FB5"));
        QVERIFY(!exportTheme("../x", dir.filePath("y.json")));
    }
    void builtinExportsAndImportsAsCopy() {
        const QString out = dir.filePath("m.json");
        QVERIFY(exportTheme("modernist", out));
        QString name, err;
        QVERIFY2(imp(out, &name, &err), qPrintable(err));
        QCOMPARE(name, QString("modernist-copy"));
        for (bool dark : {false, true}) QCOMPARE(loadTheme(name, dark).accent, loadTheme("modernist", dark).accent);
    }
    void removeOnlyUserThemes() {
        QString err;
        QVERIFY(!removeTheme("modernist", &err));
        QVERIFY(!removeTheme("../config", &err));
        QVERIFY(!removeTheme("nope", &err));
        QString name;
        QVERIFY(imp(src("a.json", kNative), &name));
        QVERIFY(removeTheme(name));
        QVERIFY(!QFile::exists(themesDir() + "/Mine.json"));
    }
};
QTEST_MAIN(ThemeIoTest)
#include "theme_io_test.moc"
