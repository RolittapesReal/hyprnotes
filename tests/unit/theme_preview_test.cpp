#include <QtTest>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStatusBar>
#include <QTabBar>
#include <QTemporaryDir>
#include <QToolButton>
#include <QVBoxLayout>
#include <hn/theme/theme.h>
using namespace hn::theme;

class PreviewTest : public QObject {
    Q_OBJECT
    static QImage render(bool dark) {
        const Theme t = loadTheme("modernist", dark);
        applyTheme(t);
        QWidget w; w.resize(480, 360);
        auto *v = new QVBoxLayout(&w); v->setContentsMargins(16, 16, 16, 16); v->setSpacing(8);
        auto *h = new QLabel("Hyprnotes"); h->setFont(labelFont(t)); v->addWidget(h);
        auto *tabs = new QTabBar; tabs->addTab("Notes"); tabs->addTab("Tags"); tabs->addTab("Settings"); v->addWidget(tabs);
        auto *row = new QHBoxLayout; v->addLayout(row);
        auto *le = new QLineEdit; le->setPlaceholderText("Search notes"); le->setText("grocery"); row->addWidget(le);
        for (auto n : {"bold", "italic", "link", "pin", "close"}) { auto *b = new QToolButton; b->setIcon(icon(n, t.text)); row->addWidget(b); }
        auto *pb = new QPushButton("New note"); pb->setDefault(true); row->addWidget(pb);
        auto *list = new QListWidget; list->addItems({"Groceries", "Meeting notes", "Ideas", "Reading list"}); list->setCurrentRow(1); v->addWidget(list);
        auto *cb = new QCheckBox("Launch tray"); cb->setChecked(true); v->addWidget(cb);
        auto *combo = new QComboBox; combo->addItems({"modernist", "custom"}); v->addWidget(combo);
        auto *sb = new QStatusBar; sb->showMessage("Saved"); v->addWidget(sb);
        w.show();
        [[maybe_unused]] bool ex = QTest::qWaitForWindowExposed(&w);
        return w.grab().toImage();
    }
private slots:
    void rendersNonBlank() {
        QTemporaryDir out; QVERIFY(out.isValid());
        for (bool dark : {false, true}) {
            const QImage im = render(dark);
            const QString name = dark ? "dark" : "light";
            QVERIFY(im.save(out.filePath("modernist-preview-" + name + ".png")));
            QSet<QRgb> colors; for (int y = 0; y < im.height(); y += 2) for (int x = 0; x < im.width(); x += 2) colors.insert(im.pixel(x, y));
            QVERIFY2(colors.size() > 8, "preview looks blank");
            QCOMPARE(QColor(im.pixel(1, 1)), loadTheme("modernist", dark).bg);
            if (const QString d = qEnvironmentVariable("HN_PREVIEW_DIR"); !d.isEmpty()) {
                QDir().mkpath(d);
                QVERIFY(im.save(d + "/modernist-preview-" + name + ".png"));
            }
        }
    }
};
QTEST_MAIN(PreviewTest)
#include "theme_preview_test.moc"
