#include "settings_dialog.h"
#include "plugins_page.h"
#include "hn/platform/autostart.h"
#include "ui_common.h"
#include "hn/theme/theme_io.h"
#include <QCheckBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMessageBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QTableWidget>
#include <QVBoxLayout>

using namespace hn::theme;

namespace hn::app {

namespace {
QLabel *section(const QString &t, QWidget *p) {
    auto *l = new QLabel(t, p);
    l->setFont(ui::labelFont(10));
    l->setStyleSheet(QString("color: %1;").arg(ui::theme().muted.name()));
    return l;
}
QLabel *hint(const QString &t, QWidget *p) {
    auto *l = new QLabel(t, p);
    l->setWordWrap(true);
    l->setStyleSheet(QString("color: %1;").arg(ui::theme().muted.name()));
    return l;
}
QWidget *page(QVBoxLayout **out, QWidget *parent) {
    auto *w = new QWidget(parent);
    *out = new QVBoxLayout(w);
    (*out)->setContentsMargins(24, 24, 24, 24);
    (*out)->setSpacing(8);
    return w;
}
} // namespace

QString runFirstRunDialog(const QString &suggested) {
    QDialog d;
    d.setWindowTitle(QObject::tr("Welcome to Hyprnotes"));
    d.setWindowIcon(ui::appIcon());
    auto *l = new QVBoxLayout(&d);
    l->setContentsMargins(32, 32, 32, 24);
    l->setSpacing(8);
    auto *h = new QLabel(QObject::tr("Where should your notes live?"), &d);
    h->setFont(ui::uiFont(20, QFont::Bold));
    l->addWidget(h);
    l->addWidget(hint(QObject::tr("Notes are plain Markdown files in a folder you control, so any sync tool or Git can manage them."), &d));
    l->addSpacing(16);
    auto *row = new QHBoxLayout;
    auto *edit = new QLineEdit(suggested, &d);
    auto *browse = new QPushButton(QObject::tr("Browse…"), &d);
    row->addWidget(edit, 1);
    row->addWidget(browse);
    l->addLayout(row);
    l->addSpacing(16);
    auto *box = new QDialogButtonBox(&d);
    auto *ok = box->addButton(QObject::tr("Use this folder"), QDialogButtonBox::AcceptRole);
    ok->setStyleSheet(ui::accentButtonStyle());
    ok->setDefault(true);
    l->addWidget(box);
    QObject::connect(browse, &QPushButton::clicked, &d, [&] {
        const QString p = QFileDialog::getExistingDirectory(&d, QObject::tr("Notes folder"), edit->text());
        if (!p.isEmpty()) edit->setText(p);
    });
    QObject::connect(ok, &QPushButton::clicked, &d, &QDialog::accept);
    d.setMinimumWidth(480);
    return d.exec() == QDialog::Accepted && !edit->text().trimmed().isEmpty() ? edit->text().trimmed() : QString();
}

SettingsDialog::SettingsDialog(AppController *c, QWidget *parent) : QDialog(parent), m_c(c) {
    setWindowTitle(tr("Settings"));
    setWindowIcon(ui::appIcon());
    setMinimumSize(520, 460);
    resize(840, 720);
    setAcceptDrops(true);
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    m_tabs = new QTabWidget(this);
    m_tabs->addTab(tabAppearance(), tr("Appearance"));
    m_tabs->addTab(tabNotes(), tr("Notes"));
    m_tabs->addTab(tabBehavior(), tr("Behavior"));
    m_pluginsPage = new PluginsPage(m_c, this);   // lists plugins and migrated mods; its manager is created on first show
    m_tabs->addTab(m_pluginsPage, tr("Plugins"));
    m_tabs->addTab(tabKeys(), tr("Keys"));
    lay->addWidget(m_tabs);
    m_note = new QLabel(this);
    m_note->setContentsMargins(24, 8, 24, 16);
    m_note->setWordWrap(true);
    lay->addWidget(m_note);
    m_loading = false;
}

QWidget *SettingsDialog::tabAppearance() {
    QVBoxLayout *l;
    auto *w = page(&l, this);
    const auto s = m_c->settings();
    l->addWidget(section(tr("THEME"), w));
    m_theme = new QComboBox(w);
    l->addWidget(m_theme);
    auto *trow = new QHBoxLayout;
    auto *imp = new QPushButton(tr("Import theme…"), w);
    auto *exp = new QPushButton(tr("Export current theme…"), w);
    m_removeTheme = new QPushButton(tr("Remove"), w);
    for (auto *b : {imp, exp, m_removeTheme}) trow->addWidget(b);
    trow->addStretch(1);
    l->addLayout(trow);
    l->addWidget(hint(tr("Import a Hyprnotes theme (.json), a base16 scheme (.yaml) or a VS Code color theme (.json), or drop the file here."), w));
    l->addSpacing(16);
    l->addWidget(section(tr("COLOR SCHEME"), w));
    m_scheme = new QComboBox(w);
    m_scheme->addItems({tr("System"), tr("Light"), tr("Dark")});
    m_scheme->setCurrentIndex(s.colorScheme == "light" ? 1 : s.colorScheme == "dark" ? 2 : 0);
    l->addWidget(m_scheme);
    l->addSpacing(16);
    l->addWidget(section(tr("TEXT SIZE"), w));
    m_font = new QSpinBox(w);
    m_font->setRange(8, 32);
    m_font->setSuffix(tr(" px"));
    m_font->setValue(m_c->theme().baseSize);
    l->addWidget(m_font);
    l->addSpacing(16);
    l->addWidget(section(tr("MOTION"), w));
    m_motion = new QCheckBox(tr("Reduce motion"), w);
    m_motion->setChecked(s.reduceMotion);
    l->addWidget(m_motion);
    l->addStretch(1);
    refreshThemes(s.theme);
    connect(m_theme, &QComboBox::currentIndexChanged, this, [this] { m_removeTheme->setEnabled(m_theme->currentIndex() > 0); apply(); });
    connect(imp, &QPushButton::clicked, this, [this] {
        const QString p = QFileDialog::getOpenFileName(this, tr("Import theme"), QString(), tr("Themes (*.json *.yaml *.yml)"));
        if (!p.isEmpty()) importThemeFile(p);
    });
    connect(exp, &QPushButton::clicked, this, [this] {
        const QString p = QFileDialog::getSaveFileName(this, tr("Export theme"), m_theme->currentText() + ".json", tr("Theme (*.json)"));
        if (!p.isEmpty()) exportThemeTo(p);
    });
    connect(m_removeTheme, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Remove theme"), tr("Delete the theme \"%1\"?").arg(m_theme->currentText())) == QMessageBox::Yes) removeSelectedTheme();
    });
    connect(m_scheme, &QComboBox::currentIndexChanged, this, [this] { apply(); });
    connect(m_font, &QSpinBox::valueChanged, this, [this] { apply(); });
    connect(m_motion, &QCheckBox::toggled, this, [this] { apply(); });
    return w;
}

QWidget *SettingsDialog::tabNotes() {
    QVBoxLayout *l;
    auto *w = page(&l, this);
    l->addWidget(section(tr("NOTES FOLDER"), w));
    auto *row = new QHBoxLayout;
    m_folder = new QLineEdit(m_c->notesRoot(), w);
    auto *browse = new QPushButton(tr("Browse…"), w);
    row->addWidget(m_folder, 1);
    row->addWidget(browse);
    l->addLayout(row);
    l->addWidget(hint(tr("Plain .md files. Changing the folder takes effect immediately when no notes are open; otherwise after a restart."), w));
    l->addStretch(1);
    connect(m_folder, &QLineEdit::editingFinished, this, [this] { apply(); });
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString p = QFileDialog::getExistingDirectory(this, tr("Notes folder"), m_folder->text());
        if (!p.isEmpty()) { m_folder->setText(p); apply(); }
    });
    return w;
}

QWidget *SettingsDialog::tabBehavior() {
    QVBoxLayout *l;
    auto *w = page(&l, this);
    l->addWidget(section(tr("TRAY"), w));
    m_tray = new QCheckBox(tr("Keep Hyprnotes in the tray when the organizer is closed"), w);
    m_tray->setChecked(m_c->settings().trayEnabled);
    l->addWidget(m_tray);
    l->addWidget(hint(tr("Needs a status-notifier host such as Waybar's tray module. Without one, closing the organizer closes it normally."), w));
    l->addSpacing(16);
    l->addWidget(section(tr("STARTUP"), w));
    m_autostart = new QCheckBox(tr("Start in the background at login"), w);
    hn::platform::Autostart as;
    const auto st = as.state();
    m_autostart->setChecked(st != hn::platform::Autostart::State::Off);
    if (st == hn::platform::Autostart::State::ExternallyManaged) {
        m_autostart->setEnabled(false);
        l->addWidget(m_autostart);
        l->addWidget(hint(tr("Startup is managed by an entry you created (%1). Hyprnotes will not change it.").arg(as.filePath()), w));
    } else l->addWidget(m_autostart);
    l->addStretch(1);
    connect(m_tray, &QCheckBox::toggled, this, [this] { apply(); });
    connect(m_autostart, &QCheckBox::toggled, this, [this](bool on) {
        if (m_loading) return;
        hn::platform::Autostart a;
        const auto r = on ? a.enable() : a.disable();
        if (!r.ok) {
            m_note->setText(r.message);
            QSignalBlocker b(m_autostart);
            m_autostart->setChecked(!on);
        } else m_note->clear();
    });
    return w;
}

QWidget *SettingsDialog::tabKeys() {
    QVBoxLayout *l;
    auto *w = page(&l, this);
    l->addWidget(section(tr("KEYBINDINGS"), w));
    m_keys = new QTableWidget(0, 2, w);
    m_keys->setHorizontalHeaderLabels({tr("Action"), tr("Shortcut")});
    m_keys->verticalHeader()->hide();
    m_keys->horizontalHeader()->setStretchLastSection(true);
    m_keys->setSelectionMode(QAbstractItemView::NoSelection);
    const auto kb = m_c->settings().keybindings;
    auto addRow = [&](const QString &id, const QString &label, const QKeySequence &ks, const QString &pluginDefault) {
        const int r = m_keys->rowCount();
        m_keys->insertRow(r);
        auto *name = new QTableWidgetItem(label);
        name->setData(Qt::UserRole, id);
        name->setData(Qt::UserRole + 1, pluginDefault);
        name->setData(Qt::UserRole + 2, ks.toString(QKeySequence::PortableText));   // shown value: only a changed row is saved
        name->setFlags(Qt::ItemIsEnabled);
        m_keys->setItem(r, 0, name);
        auto *ed = new QKeySequenceEdit(ks, m_keys);   // few fixed rows: acceptable, unlike the note list
        ed->setMaximumSequenceLength(1);
        m_keys->setCellWidget(r, 1, ed);
        connect(ed, &QKeySequenceEdit::editingFinished, this, [this] { apply(); });
    };
    for (const auto &a : ui::actionNames()) addRow(a.first, tr(a.second.toUtf8().constData()), kb.contains(a.first) ? kb.value(a.first) : ui::defaultKey(a.first), {});
    if (m_c->plugins().active()) {   // plugin commands share the same keybinding system
        const auto pk = m_c->plugins().pluginKeys();
        for (const auto &a : m_c->plugins().commands())
            addRow(QStringLiteral("plugin:") + a.qid, tr("Plugin: %1: %2").arg(a.pluginName, a.title), pk.value(QStringLiteral("plugin:") + a.qid), a.key);
    }
    m_keys->setColumnWidth(0, 220);
    l->addWidget(m_keys, 1);
    l->addWidget(hint(tr("Clear a field to unbind the action."), w));
    return w;
}

void SettingsDialog::refreshThemes(const QString &select) {
    QSignalBlocker b(m_theme);
    m_theme->clear();
    m_theme->addItems(hn::theme::listThemes());
    const int i = m_theme->findText(select);
    m_theme->setCurrentIndex(i < 0 ? 0 : i);
    m_removeTheme->setEnabled(m_theme->currentIndex() > 0);
}

QString SettingsDialog::note() const { return m_note->text(); }

bool SettingsDialog::importThemeFile(const QString &path) {
    QString msg;
    const bool ok = m_c->importThemeFile(path, &msg);
    refreshThemes(m_c->settings().theme);
    m_note->setText(msg);
    return ok;
}

bool SettingsDialog::exportThemeTo(const QString &path) {
    QString err;
    const bool ok = hn::theme::exportTheme(m_theme->currentText(), path, &err);
    m_note->setText(ok ? tr("Exported \"%1\" to %2.").arg(m_theme->currentText(), path) : tr("Export failed: %1").arg(err));
    return ok;
}

bool SettingsDialog::removeSelectedTheme() {
    const QString name = m_theme->currentText();
    QString err;
    if (!hn::theme::removeTheme(name, &err)) { m_note->setText(tr("Could not remove: %1").arg(err)); return false; }
    refreshThemes("modernist");   // blocked signals: apply the fallback explicitly
    apply();
    m_note->setText(tr("Removed theme \"%1\".").arg(name));
    return true;
}

void SettingsDialog::showTab(int i) { m_tabs->setCurrentIndex(i); }

void SettingsDialog::dragEnterEvent(QDragEnterEvent *e) {
    if (!AppController::themeFileFromMime(e->mimeData()).isEmpty() || !AppController::pluginPathFromMime(e->mimeData()).isEmpty()) e->acceptProposedAction();
}

void SettingsDialog::dropEvent(QDropEvent *e) {
    if (const QString p = AppController::themeFileFromMime(e->mimeData()); !p.isEmpty()) { e->acceptProposedAction(); importThemeFile(p); return; }
    if (const QString p = AppController::pluginPathFromMime(e->mimeData()); !p.isEmpty()) {
        e->acceptProposedAction();
        showTab(kPluginsTab);
        QTimer::singleShot(0, this, [this, p] { m_pluginsPage->installPath(p); });   // after the drop handler returns: the consent dialog is modal
    }
}

void SettingsDialog::apply() {
    if (m_loading) return;
    Settings s = m_c->settings();
    s.theme = m_theme->currentText().isEmpty() ? s.theme : m_theme->currentText();
    s.colorScheme = m_scheme->currentIndex() == 1 ? "light" : m_scheme->currentIndex() == 2 ? "dark" : "system";
    s.reduceMotion = m_motion->isChecked();
    s.trayEnabled = m_tray->isChecked();
    const QString folder = m_folder->text().trimmed();
    if (QDir::isAbsolutePath(folder)) s.notesFolder = QDir::cleanPath(folder);
    for (int r = 0; r < m_keys->rowCount(); ++r) {
        const QString id = m_keys->item(r, 0)->data(Qt::UserRole).toString();
        const QKeySequence ks = static_cast<QKeySequenceEdit *>(m_keys->cellWidget(r, 1))->keySequence();
        if (id.startsWith(QLatin1String("plugin:"))) {   // only rows the user changed are persisted; the plugin default stays a default
            if (ks.toString(QKeySequence::PortableText) != m_keys->item(r, 0)->data(Qt::UserRole + 2).toString()) s.keybindings[id] = ks;
            continue;
        }
        s.keybindings[id] = ks;
    }
    AppPrefs p = m_c->prefs();
    if (m_font->value() != m_c->theme().baseSize) p.fontSize = m_font->value();
    m_c->applySettings(s, p);
    m_note->clear();
}

} // namespace hn::app
