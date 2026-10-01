#include "app_test_util.h"
#include "cli.h"
#include "settings_dialog.h"
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QProcess>
#include <QPushButton>
#include <hn/theme/theme_io.h>

#include "theme_fixtures.h"
using namespace apptest;

static const QByteArray kTheme = J("{'version':1,'name':'inkblue','light':{'accent':'#1F4FB5','bg':'#FFFFFF'},'dark':{'accent':'#4F7FD9'}}");

class ThemeAppTest : public QObject {
    Q_OBJECT
    QTemporaryDir xdg;
    QString file(const QString &n, const QByteArray &b = kTheme) {
        QFile f(xdg.filePath(n));
        if (f.open(QIODevice::WriteOnly)) f.write(b);
        return f.fileName();
    }
private slots:
    void init() { qputenv("XDG_CONFIG_HOME", xdg.filePath("xdg").toUtf8()); QDir(xdg.filePath("xdg")).removeRecursively(); }

    void picker_lists_imports_applies_and_persists() {
        Lib l;
        AppController c(l.opts());
        c.config().save(c.settings());
        c.config().reload();
        hn::app::SettingsDialog dlg(&c);
        QCOMPARE(dlg.themeBox()->count(), 1);
        QVERIFY(!dlg.removeThemeButton()->isEnabled());
        QVERIFY(dlg.importThemeFile(file("t.json")));
        QCOMPARE(dlg.themeBox()->currentText(), QString("inkblue"));
        QCOMPARE(c.settings().theme, QString("inkblue"));
        QCOMPARE(c.theme().accent.name().toUpper(), c.theme().dark ? QString("#4F7FD9") : QString("#1F4FB5"));
        c.config().reload();
        QCOMPARE(c.config().settings().theme, QString("inkblue"));          // persisted
        QVERIFY(dlg.removeThemeButton()->isEnabled());
        dlg.themeBox()->setCurrentIndex(0);                                  // picker applies live
        QCOMPARE(c.settings().theme, QString("modernist"));
        dlg.themeBox()->setCurrentIndex(1);
        QCOMPARE(c.settings().theme, QString("inkblue"));
        QVERIFY(dlg.importThemeFile(file("bad.json", "{x")) == false);
        QVERIFY(dlg.note().contains("not imported"));
        QCOMPARE(c.settings().theme, QString("inkblue"));
        QVERIFY(dlg.exportThemeTo(xdg.filePath("exp.json")));
        QVERIFY(dlg.removeSelectedTheme());
        QCOMPARE(c.settings().theme, QString("modernist"));
        QVERIFY(!QFile::exists(hn::theme::themesDir() + "/inkblue.json"));
    }

    void contrast_warning_is_shown() {
        Lib l;
        AppController c(l.opts());
        hn::app::SettingsDialog dlg(&c);
        QVERIFY(dlg.importThemeFile(file("lc.json", J("{'version':1,'name':'faint','light':{'bg':'#FFFFFF','text':'#EEEEEE'}}"))));
        QVERIFY2(dlg.note().contains("contrast"), qPrintable(dlg.note()));
    }

    void dropping_a_theme_file_on_the_dialog_and_organizer_imports_it() {
        Lib l;
        AppController c(l.opts());
        hn::app::SettingsDialog dlg(&c);
        auto drop = [&](QWidget *w, const QString &path) {
            auto *m = new QMimeData;
            m->setUrls({QUrl::fromLocalFile(path)});
            QDropEvent e(QPointF(5, 5), Qt::CopyAction, m, Qt::LeftButton, Qt::NoModifier);
            w->show();
            QDragEnterEvent en(QPoint(5, 5), Qt::CopyAction, m, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(w, &en);
            if (!en.isAccepted()) return;   // a non-theme file is refused at drag-enter
            QApplication::sendEvent(w, &e);
            delete m;
        };
        drop(&dlg, file("d.json"));
        QCOMPARE(c.settings().theme, QString("inkblue"));
        c.showOrganizer();
        drop(c.organizer(), file("e.json", J("{'version':1,'name':'second','light':{}}")));
        QCOMPARE(c.settings().theme, QString("second"));
        drop(c.organizer(), file("notes.md", "x"));                           // not a theme: ignored
        QCOMPARE(c.settings().theme, QString("second"));
    }

    void cli_import_selects_in_config_and_reports() {
        QTemporaryDir d;
        QDir().mkpath(d.path() + "/cfg");
        { QFile f(d.path() + "/cfg/config.json"); if (f.open(QIODevice::WriteOnly)) f.write(J("{'version':1,'colorScheme':'dark'}")); }
        qputenv("HN_CONFIG_DIR", (d.path() + "/cfg").toUtf8());
        QString so, se;
        QTextStream out(&so), err(&se);
        QVERIFY(hn::app::parseCli({"--import-theme", "f.json"}).importTheme == "f.json");
        QVERIFY(!hn::app::parseCli({"--import-theme"}).error.isEmpty());
        QCOMPARE(hn::app::runImportTheme(file("c.json"), out, err), 0);
        QVERIFY2(so.contains("inkblue"), qPrintable(so));
        hn::theme::Config cfg(d.path() + "/cfg/config.json");
        QCOMPARE(cfg.settings().theme, QString("inkblue"));
        QCOMPARE(cfg.settings().colorScheme, QString("dark"));              // rest of config preserved
        se.clear();
        QCOMPARE(hn::app::runImportTheme(file("bad.json", "{x"), out, err), 1);
        QVERIFY2(se.contains("not imported"), qPrintable(se));
        QCOMPARE(hn::theme::Config(d.path() + "/cfg/config.json").settings().theme, QString("inkblue"));
        qunsetenv("HN_CONFIG_DIR");
    }

    void cli_binary_imports_headless_and_prints_result() {
        QTemporaryDir d;
        QDir().mkpath(d.path() + "/cfg");
        QProcess p;
        QProcessEnvironment e;
        e.insert("PATH", qEnvironmentVariable("PATH"));
        e.insert("HOME", d.path() + "/home");
        e.insert("QT_QPA_PLATFORM", "offscreen");
        e.insert("XDG_CONFIG_HOME", d.path() + "/xdg");
        e.insert("HN_CONFIG_DIR", d.path() + "/cfg");
        p.setProcessEnvironment(e);
        p.start(HN_BIN, {"--import-theme", file("b.json")});
        QVERIFY(p.waitForFinished(15000));
        QCOMPARE(p.exitCode(), 0);
        QVERIFY(QString::fromLocal8Bit(p.readAllStandardOutput()).contains("inkblue"));
        QVERIFY(QFile::exists(d.path() + "/xdg/hyprnotes/themes/inkblue.json"));
        p.start(HN_BIN, {"--import-theme", d.path() + "/nope.json"});
        QVERIFY(p.waitForFinished(15000));
        QCOMPARE(p.exitCode(), 1);
        QVERIFY(QString::fromLocal8Bit(p.readAllStandardError()).contains("not imported"));
    }
};

QTEST_MAIN(ThemeAppTest)
#include "app_theme_test.moc"
