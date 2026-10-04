#include <QtTest>
#include <QDir>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <hn/theme/theme.h>
#include <hn/theme/theme_io.h>
#include "theme_fixtures.h"
using namespace hn::theme;

static const QByteArray kNative = J("{'version':1,'name':'Mine','light':{'accent':'#1F4FB5'},'dark':{}}");
static const QStringList kBuiltins{"modernist", "catppuccin-mocha", "tokyo-night", "dracula", "nord", "gruvbox-dark", "one-dark"};

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
        QCOMPARE(listThemes(), kBuiltins + QStringList{"Mine"});
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
        QCOMPARE(listThemes(), kBuiltins);
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
    void reservedNames_data() {
        QTest::addColumn<QString>("id");
        for (const QString &canonical : kBuiltins)
            for (const QString &id : {canonical, canonical.toUpper()}) QTest::newRow(qPrintable(id)) << id;
    }
    void reservedNames() {
        QFETCH(QString, id);
        QVERIFY(isBuiltinTheme(id));
        QVERIFY(!QFile::exists(themesDir()));
    }
    void nonReservedNamesAndEmptyAliases() {
        for (const QString &id : {QString(), QString("modernist-copy"), QString("nord-copy"), QString(" nord"), QString("nord "), QString("unknown")})
            QVERIFY(!isBuiltinTheme(id));
        QCOMPARE(builtinThemeObject()["name"].toString(), QString("modernist-copy"));
        for (bool dark : {false, true}) {
            const Theme alias = loadTheme({}, dark);
            QVERIFY(lastThemeError().isEmpty());
            QCOMPARE(alias.bg, loadTheme("modernist", dark).bg);
        }
        const QString path = dir.filePath("empty-alias.json");
        QVERIFY(exportTheme({}, path));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(f.readAll()).object(), builtinThemeObject());
        QVERIFY(!QFile::exists(themesDir()));
    }
    void reservedImportsRejected_data() { reservedNames_data(); }
    void reservedImportsRejected() {
        QFETCH(QString, id);
        const QJsonObject object{{"version", 1}, {"name", id}, {"dark", QJsonObject{{"bg", "#010203"}}}};
        QString error;
        QVERIFY(!imp(src("reserved.json", QJsonDocument(object).toJson()), nullptr, &error));
        QVERIFY2(error.contains("built-in"), qPrintable(error));
        QVERIFY(!QFile::exists(themesDir()));
    }
    void shadowFilesStayHiddenAndDoNotOverride_data() { reservedNames_data(); }
    void shadowFilesStayHiddenAndDoNotOverride() {
        QFETCH(QString, id);
        QVERIFY(QDir().mkpath(themesDir()));
        const QByteArray shadow = J("{'version':1,'dark':{'bg':'#010203'},'light':{'bg':'#010203'}}");
        const QString path = src("xdg/hyprnotes/themes/" + id + ".json", shadow);
        src("xdg/hyprnotes/themes/zeta.json", kNative);
        src("xdg/hyprnotes/themes/alpha.json", kNative);
        src("xdg/hyprnotes/themes/a..b.json", kNative);
        QCOMPARE(listThemes(), kBuiltins + QStringList({"alpha", "zeta"}));
        for (bool dark : {false, true}) {
            const Theme theme = loadTheme(id, dark);
            QVERIFY2(lastThemeError().isEmpty(), qPrintable(lastThemeError()));
            QVERIFY(theme.bg != QColor("#010203"));
            QCOMPARE(theme.bg, loadTheme(id.toLower(), dark).bg);
        }
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), shadow);
    }
    void reservedRemovalPreservesShadowBytes_data() { reservedNames_data(); }
    void reservedRemovalPreservesShadowBytes() {
        QFETCH(QString, id);
        QVERIFY(QDir().mkpath(themesDir()));
        const QString path = src("xdg/hyprnotes/themes/" + id + ".json", kNative);
        QString error;
        QVERIFY(!removeTheme(id, &error));
        QVERIFY2(error.contains("built-in"), qPrintable(error));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), kNative);
    }
    void exportedCopiesAreEditable_data() { reservedNames_data(); }
    void exportedCopiesAreEditable() {
        QFETCH(QString, id);
        const QString path = dir.filePath("copy.json");
        QString error, name;
        QVERIFY2(exportTheme(id, path, &error), qPrintable(error));
        QFile exported(path);
        QVERIFY(exported.open(QIODevice::ReadOnly));
        const QJsonObject object = QJsonDocument::fromJson(exported.readAll()).object();
        QCOMPARE(object["name"].toString(), id.toLower() + "-copy");
        QCOMPARE(object["version"].toInt(), 1);
        QCOMPARE(object["base"].toString(), QString("modernist"));
        QVERIFY(object["dark"].isObject());
        if (id.toLower() != "modernist") {
            QVERIFY(!object.contains("light"));
            QCOMPARE(object["dark"].toObject()["radius"].toInt(), 4);
            QCOMPARE(object["dark"].toObject()["borderWidth"].toInt(), 1);
            for (const char *inherited : {"fontFamily", "monoFamily", "baseSize", "padding", "lineHeight"})
                QVERIFY(!object["dark"].toObject().contains(inherited));
        }
        QVERIFY2(imp(path, &name, &error), qPrintable(error));
        QCOMPARE(name, id.toLower() + "-copy");
        QVERIFY(!isBuiltinTheme(name));
        const Theme original = loadTheme(id, true);
        QCOMPARE(loadTheme(name, true).accent, original.accent);
        QJsonObject edited = object;
        QJsonObject dark = edited["dark"].toObject();
        dark["accent"] = "#123456";
        edited["dark"] = dark;
        src("xdg/hyprnotes/themes/" + name + ".json", QJsonDocument(edited).toJson());
        QCOMPARE(loadTheme(name, true).accent, QColor("#123456"));
        QCOMPARE(loadTheme(id, true).accent, original.accent);
        QVERIFY(removeTheme(name, &error));
    }
};
QTEST_MAIN(ThemeIoTest)
#include "theme_io_test.moc"
