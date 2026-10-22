#include "settings_dialog.h"
#include <QGuiApplication>
#include <QStyleHints>
#include "action_row.h"
#include "plugins_page.h"
#include "hn/platform/autostart.h"
#include "ui_common.h"
#include "hn/theme/theme_io.h"
#include <QCheckBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMessageBox>
#include <QComboBox>
#include <QDir>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScreen>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWindow>

using namespace hn::theme;

namespace hn::app {

namespace {
class FirstRunDialog : public QDialog {
};

QLabel *section(const QString &t, QWidget *p) {
    auto *l = new QLabel(t, p);
    l->setProperty("hnRole", "section");
    l->setTextFormat(Qt::PlainText);
    l->setWordWrap(true);
    return l;
}
QLabel *hint(const QString &t, QWidget *p) {
    auto *l = new QLabel(t, p);
    l->setWordWrap(true);
    l->setTextFormat(Qt::PlainText);
    l->setProperty("hnRole", "hint");
    return l;
}
QWidget *page(QVBoxLayout **out, QWidget *parent) {
    auto *w = new QWidget(parent);
    *out = new QVBoxLayout(w);
    (*out)->setContentsMargins(24, 24, 24, 24);
    (*out)->setSpacing(8);
    return w;
}
QWidget *scrollPage(QWidget *content) {
    auto *scroll = new QScrollArea(content->parentWidget());
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(content);
    return scroll;
}
} // namespace

QString runFirstRunDialog(const QString &suggested) {
    FirstRunDialog d;
    d.setObjectName("hnFirstRunDialog");
    d.setWindowTitle(QObject::tr("Welcome to Hyprnotes"));
    d.setWindowIcon(ui::appIcon());
    // A tiling compositor may stretch the dialog to a whole screen: keep the content in a centered column.
    auto *outer = new QHBoxLayout(&d);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *card = new QWidget(&d);
    card->setMaximumWidth(600);
    outer->addStretch(1);
    outer->addWidget(card, 100);
    outer->addStretch(1);
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(32, 24, 32, 24);
    cardLayout->setSpacing(8);
    auto *scroll = new QScrollArea(card);
    scroll->setAccessibleName(QObject::tr("Notes folder setup"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *content = new QWidget;
    auto *l = new QVBoxLayout(content);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(8);
    auto *h = new QLabel(QObject::tr("Where should your notes live?"), content);
    h->setProperty("hnRole", "heading");
    h->setWordWrap(true);
    const auto restyle = [h] {
        h->setStyleSheet(QString("border-left: 4px solid %1; padding-left: 16px;").arg(ui::theme().accent.name()));
    };
    restyle();
    QObject::connect(ui::themeNotifier(), &ui::ThemeNotifier::changed, &d, restyle);
    l->addWidget(h);
    l->addWidget(hint(QObject::tr("Notes are plain Markdown files in a folder you control, so any sync tool or Git can manage them."), content));
    l->addSpacing(16);
    auto *row = new QHBoxLayout;
    auto *edit = new QLineEdit(suggested, content);
    edit->setAccessibleName(QObject::tr("Notes folder"));
    edit->setMinimumWidth(0);
    auto *browse = new QPushButton(QObject::tr("Browse…"), content);
    row->addWidget(edit, 1);
    row->addWidget(browse);
    l->addLayout(row);
    l->addSpacing(16);
    l->addWidget(hint(QObject::tr("Closing this dialog uses the default notes folder."), content));
    scroll->setWidget(content);
    cardLayout->addWidget(scroll, 1);
    auto *box = new ui::ActionRow(card);
    auto *close = new ui::WrappingButton(QObject::tr("Close"), card);
    close->setObjectName("hnFirstRunClose");
    auto *ok = new ui::WrappingButton(QObject::tr("Use this folder"), card);
    box->addButton(close);
    box->addButton(ok);
    ok->setProperty("hnRole", "primary");
    ok->setDefault(true);
    cardLayout->addWidget(box);
    QObject::connect(browse, &QPushButton::clicked, &d, [&] {
        const QString p = ui::chooseDirectory(&d, QObject::tr("Notes folder"), edit->text());
        if (!p.isEmpty()) edit->setText(p);
    });
    QObject::connect(ok, &QPushButton::clicked, &d, &QDialog::accept);
    QObject::connect(close, &QPushButton::clicked, &d, &QDialog::reject);
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
    auto *title = new QLabel(tr("Settings"), this);
    title->setObjectName("hnSettingsTitle");
    title->setProperty("hnRole", "heading");
    title->setContentsMargins(24, 16, 24, 8);
    lay->addWidget(title);
    m_tabs = new QTabWidget(this);
    m_tabs->setAccessibleName(tr("Settings pages"));
    m_tabs->addTab(tabAppearance(), tr("Appearance"));
    m_tabs->addTab(tabNotes(), tr("Notes"));
    m_tabs->addTab(tabBehavior(), tr("Behavior"));
    m_pluginsPage = new PluginsPage(m_c, this);   // lists plugins and migrated mods; its manager is created on first show
    m_tabs->addTab(m_pluginsPage, tr("Plugins"));
    m_tabs->addTab(tabKeys(), tr("Keys"));
    for (int i = 0; i < m_tabs->count(); ++i)
        if (auto *scroll = qobject_cast<QScrollArea *>(m_tabs->widget(i)))
            scroll->setAccessibleName(tr("%1 settings").arg(m_tabs->tabText(i)));
    lay->addWidget(m_tabs);
    auto *fade = new ui::FadeOverlay(m_tabs->currentWidget()->parentWidget());
    connect(m_tabs, &QTabWidget::currentChanged, this,
        [this, fade, previous = QPointer<QWidget>(m_tabs->currentWidget())](int) mutable {
            const auto snapshot = previous ? fade->snapshot(previous) : QPixmap{};
            previous = m_tabs->currentWidget();
            fade->play(snapshot);
        });
    m_note = new QLabel(this);
    m_note->setContentsMargins(24, 0, 24, 0);
    m_note->setWordWrap(true);
    m_note->setTextFormat(Qt::PlainText);
    m_note->hide();
    lay->addWidget(m_note);
    auto *footer = new QDialogButtonBox(QDialogButtonBox::Close, this);
    footer->setContentsMargins(24, 8, 24, 16);
    auto *closeBtn = footer->button(QDialogButtonBox::Close);
    closeBtn->setObjectName("hnSettingsClose");
    // The style's stock close icon is unreadable on light themes; use the themed glyph and retint on theme change.
    auto tintClose = [closeBtn] { closeBtn->setIcon(ui::icon("close", ui::theme().text, 12)); };
    tintClose();
    connect(m_c, &AppController::themeChanged, closeBtn, tintClose);
    connect(footer, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(footer);
    connect(m_c, &AppController::themeChanged, this, &SettingsDialog::syncAppearanceFromController);
    m_loading = false;
}

bool SettingsDialog::event(QEvent *event) {
    const bool handled = QDialog::event(event);
    if (event->type() == QEvent::ScreenChangeInternal || event->type() == QEvent::LayoutRequest)
        fitAvailableScreen();
    return handled;
}

void SettingsDialog::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);
    fitAvailableScreen();
}

void SettingsDialog::fitAvailableScreen() {
    if (m_fittingScreen || !screen()) return;
    m_fittingScreen = true;
    if (m_screen != screen()) {
        disconnect(m_screenGeometryConnection);
        m_screen = screen();
        m_screenGeometryConnection = connect(screen(), &QScreen::availableGeometryChanged, this,
                                               [this] { fitAvailableScreen(); });
    }
    const QRect available = screen()->availableGeometry();
    if (available.isEmpty()) { m_fittingScreen = false; return; }
    QSize clientArea = available.size();
    if (windowHandle()) {
        const QMargins frame = windowHandle()->frameMargins();
        clientArea -= QSize(frame.left() + frame.right(), frame.top() + frame.bottom());
    }
    clientArea = clientArea.expandedTo(QSize(1, 1));
    setMinimumSize(qMin(520, clientArea.width()), qMin(460, clientArea.height()));
    setMaximumSize(clientArea);
    resize(qMin(width(), clientArea.width()), qMin(height(), clientArea.height()));
    const QRect frame = frameGeometry();
    const int x = qBound(available.left(), frame.x(), qMax(available.left(), available.right() + 1 - frame.width()));
    const int y = qBound(available.top(), frame.y(), qMax(available.top(), available.bottom() + 1 - frame.height()));
    if (frame.topLeft() != QPoint(x, y)) move(x, y);
    m_fittingScreen = false;
}

QWidget *SettingsDialog::tabAppearance() {
    QVBoxLayout *l;
    auto *w = page(&l, this);
    const auto s = m_c->settings();
    l->addWidget(section(tr("THEME"), w));
    m_theme = new QComboBox(w);
    l->addWidget(m_theme);
    auto *trow = new ui::ActionRow(w);
    auto *imp = new ui::WrappingButton(tr("Import theme…"), w);
    auto *exp = new ui::WrappingButton(tr("Export current theme…"), w);
    m_removeTheme = new ui::WrappingButton(tr("Remove"), w);
    trow->addButton(imp);
    trow->addButton(exp);
    trow->addButton(m_removeTheme);
    l->addWidget(trow);
    l->addWidget(hint(tr("Import a Hyprnotes theme (.json), a base16 scheme (.yaml) or a VS Code color theme (.json), or drop the file here."), w));
    l->addSpacing(16);
    l->addWidget(section(tr("COLOR SCHEME"), w));
    m_scheme = new QComboBox(w);
    m_scheme->addItems({tr("System"), tr("Light"), tr("Dark")});
    m_scheme->setCurrentIndex(s.colorScheme == "light" ? 1 : s.colorScheme == "dark" ? 2 : 0);
    l->addWidget(m_scheme);
    l->addWidget(hint(tr("Choosing a bundled dark preset switches to Dark. In Light mode the dark presets use Modernist colors."), w));
    l->addSpacing(16);
    l->addWidget(section(tr("TEXT SIZE"), w));
    m_font = new QSpinBox(w);
    m_font->setAccessibleName(tr("Text size in pixels"));
    m_font->findChild<QLineEdit *>()->setAccessibleName(tr("Text size in pixels"));
    m_font->setRange(8, 32);
    m_font->setSuffix(tr(" px"));
    m_font->setValue(m_c->theme().baseSize);
    l->addWidget(m_font);
    auto *reset = new ui::WrappingButton(tr("Use theme text size"), w);
    reset->setObjectName("hnThemeDefaultSize");
    l->addWidget(reset);
    connect(reset, &QPushButton::clicked, this, [this] {
        auto settings = m_c->settings(); settings.fontSize = 0;
        auto prefs = m_c->prefs(); prefs.fontSize = 0;
        m_c->applySettings(settings, prefs);
        syncAppearanceFromController();
    });
    l->addSpacing(16);
    l->addWidget(section(tr("MOTION"), w));
    m_motion = new QCheckBox(tr("Reduce motion"), w);
    m_motion->setChecked(s.reduceMotion);
    l->addWidget(m_motion);
    l->addStretch(1);
    refreshThemes(s.theme);
    connect(m_theme, &QComboBox::currentIndexChanged, this, [this] {
        const QString id = m_theme->currentText();
        m_removeTheme->setEnabled(!isBuiltinTheme(id));
        // A dark-only preset shows its own palette only in Dark mode: picking one while the UI resolves to light switches to Dark.
        const bool systemDark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
        const bool lightNow = m_scheme->currentIndex() == 1 || (m_scheme->currentIndex() == 0 && !systemDark);
        if (isBuiltinTheme(id) && id.compare(QStringLiteral("modernist"), Qt::CaseInsensitive) != 0 && lightNow) {
            QSignalBlocker b(m_scheme);
            m_scheme->setCurrentIndex(2);
        }
        apply();
    });
    connect(imp, &QPushButton::clicked, this, [this] {
        const QString p = ui::chooseOpenFile(this, tr("Import theme"), QString(), tr("Themes (*.json *.yaml *.yml)"));
        if (!p.isEmpty()) importThemeFile(p);
    });
    connect(exp, &QPushButton::clicked, this, [this] {
        const QString p = ui::chooseSaveFile(this, tr("Export theme"), m_theme->currentText() + ".json", tr("Theme (*.json)"));
        if (!p.isEmpty()) exportThemeTo(p);
    });
    connect(m_removeTheme, &QPushButton::clicked, this, [this] {
        QMessageBox box(QMessageBox::Question, tr("Remove theme"),
            tr("Delete the theme \"%1\"?").arg(m_theme->currentText()), QMessageBox::Yes | QMessageBox::No, this);
        box.setDefaultButton(QMessageBox::No);
        box.setEscapeButton(box.button(QMessageBox::No));
        if (box.exec() == QMessageBox::Yes) removeSelectedTheme();
    });
    connect(m_scheme, &QComboBox::currentIndexChanged, this, [this] { apply(); });
    connect(m_font, &QSpinBox::valueChanged, this, [this] { apply(true); });
    connect(m_motion, &QCheckBox::toggled, this, [this] { apply(); });
    return scrollPage(w);
}

QWidget *SettingsDialog::tabNotes() {
    QVBoxLayout *l;
    auto *w = page(&l, this);
    l->addWidget(section(tr("NOTES FOLDER"), w));
    auto *row = new QHBoxLayout;
    m_folder = new QLineEdit(m_c->notesRoot(), w);
    m_folder->setAccessibleName(tr("Notes folder"));
    auto *browse = new QPushButton(tr("Browse…"), w);
    row->addWidget(m_folder, 1);
    row->addWidget(browse);
    l->addLayout(row);
    l->addWidget(hint(tr("Plain .md files. Changing the folder takes effect immediately when no notes are open; otherwise after a restart."), w));
    l->addStretch(1);
    connect(m_folder, &QLineEdit::editingFinished, this, [this] { apply(); });
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString p = ui::chooseDirectory(this, tr("Notes folder"), m_folder->text());
        if (!p.isEmpty()) { m_folder->setText(p); apply(); }
    });
    return scrollPage(w);
}

QWidget *SettingsDialog::tabBehavior() {
    QVBoxLayout *l;
    auto *w = page(&l, this);
    l->addWidget(section(tr("TRAY"), w));
    m_tray = new QCheckBox(tr("Keep in tray"), w);
    m_tray->setChecked(m_c->settings().trayEnabled);
    l->addWidget(m_tray);
    l->addWidget(hint(tr("Keep Hyprnotes in the tray when the organizer is closed. Needs a status-notifier host such as Waybar's tray module. Without one, closing the organizer closes it normally."), w));
    l->addSpacing(16);
    l->addWidget(section(tr("STARTUP"), w));
    m_autostart = new QCheckBox(tr("Start at login"), w);
    l->addWidget(hint(tr("Start Hyprnotes in the background when you log in."), w));
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
            showNote(r.message);
            QSignalBlocker b(m_autostart);
            m_autostart->setChecked(!on);
        } else showNote({});
    });
    return scrollPage(w);
}

QWidget *SettingsDialog::tabKeys() {
    QVBoxLayout *l;
    auto *w = page(&l, this);
    l->setContentsMargins(16, 8, 16, 8);
    m_keys = new QTableWidget(0, 2, w);
    m_keys->setAccessibleName(tr("Keyboard shortcuts"));
    m_keys->installEventFilter(this);
    m_keys->setHorizontalHeaderLabels({tr("Action"), tr("Shortcut")});
    m_keys->verticalHeader()->hide();
    m_keys->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    connect(m_keys->horizontalHeader(), &QHeaderView::sectionResized, this, [this] { resizeKeyRows(); });
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
        ed->setAccessibleName(label);
        ed->findChild<QLineEdit *>()->setAccessibleName(label);
        ed->setMaximumSequenceLength(1);
        ed->installEventFilter(this);
        m_keys->setCellWidget(r, 1, ed);
        connect(ed, &QKeySequenceEdit::editingFinished, this, [this] { apply(); });
    };
    for (const auto &a : ui::actionNames()) addRow(a.first, tr(a.second.toUtf8().constData()), kb.contains(a.first) ? kb.value(a.first) : ui::defaultKey(a.first), {});
    if (m_c->plugins().active()) {   // plugin commands share the same keybinding system
        const auto pk = m_c->plugins().pluginKeys();
        for (const auto &a : m_c->plugins().commands())
            addRow(QStringLiteral("plugin:") + a.qid, tr("Plugin: %1: %2").arg(a.pluginName, a.title), pk.value(QStringLiteral("plugin:") + a.qid), a.key);
    }
    const auto addReference = [this](const QString &label, const QString &shortcut, const QString &context) {
        const int row = m_keys->rowCount();
        m_keys->insertRow(row);
        auto *name = new QTableWidgetItem(label);
        auto *key = new QTableWidgetItem(shortcut);
        for (auto *item : {name, key}) {
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            item->setToolTip(context);
        }
        m_keys->setItem(row, 0, name);
        m_keys->setItem(row, 1, key);
    };
    const auto fixed = tr("Fixed shortcut.");
    addReference(tr("Open Keys settings"), QStringLiteral("F1"), fixed);
    addReference(tr("Rename note (organizer)"), QStringLiteral("F2"), fixed);
    addReference(tr("Hide toolbar"), QStringLiteral("Esc"), fixed);
    const auto typing = tr("Type at the start of a line in visual mode.");
    addReference(tr("Type a heading"), tr("# + Space"), typing);
    addReference(tr("Type a bulleted list"), tr("- + Space"), typing);
    addReference(tr("Type a task"), tr("[ ] + Space"), typing);
    addReference(tr("Type a quote"), tr("> + Space"), typing);
    resizeKeyRows();
    l->addWidget(m_keys, 1);
    l->addWidget(hint(tr("Clear a field to unbind."), w));
    return w;
}

void SettingsDialog::refreshThemes(const QString &select) {
    QSignalBlocker b(m_theme);
    m_theme->clear();
    m_theme->addItems(hn::theme::listThemes());
    const int i = m_theme->findText(select);
    m_theme->setCurrentIndex(i < 0 ? 0 : i);
    m_removeTheme->setEnabled(!isBuiltinTheme(m_theme->currentText()));
}

void SettingsDialog::syncAppearanceFromController() {
    const QSignalBlocker fontBlock(m_font), schemeBlock(m_scheme), motionBlock(m_motion);
    const auto settings = m_c->settings();
    m_font->setValue(m_c->theme().baseSize);
    m_scheme->setCurrentIndex(settings.colorScheme == "light" ? 1 : settings.colorScheme == "dark" ? 2 : 0);
    m_motion->setChecked(settings.reduceMotion);
    refreshThemes(settings.theme);
}

void SettingsDialog::resizeKeyRows() {
    m_keys->resizeRowsToContents();
    for (int r = 0; r < m_keys->rowCount(); ++r)
        if (auto *editor = m_keys->cellWidget(r, 1))
            m_keys->setRowHeight(r, qMax(m_keys->rowHeight(r), editor->sizeHint().height() + 2));
}

bool SettingsDialog::eventFilter(QObject *watched, QEvent *event) {
    if (auto *editor = qobject_cast<QKeySequenceEdit *>(watched); editor && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
            focusNextPrevChild(key->key() != Qt::Key_Backtab && !key->modifiers().testFlag(Qt::ShiftModifier));
            return true;
        }
    }
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange || event->type() == QEvent::Show) {
        // Theme notification precedes sliced QSS/font polish. Measure after the editors receive it.
        if (!m_keysResizePending) {
            m_keysResizePending = true;
            QTimer::singleShot(0, this, [this] { m_keysResizePending = false; resizeKeyRows(); });
        }
    }
    return QDialog::eventFilter(watched, event);
}

QString SettingsDialog::note() const { return m_note->text(); }

void SettingsDialog::showNote(const QString &message) {
    m_note->setText(message);
    m_note->setVisible(!message.isEmpty());
}

bool SettingsDialog::importThemeFile(const QString &path) {
    QString msg;
    const bool ok = m_c->importThemeFile(path, &msg);
    syncAppearanceFromController();
    showNote(msg);
    return ok;
}

bool SettingsDialog::exportThemeTo(const QString &path) {
    QString err;
    const bool ok = hn::theme::exportTheme(m_theme->currentText(), path, &err);
    showNote(ok ? tr("Exported \"%1\" to %2.").arg(m_theme->currentText(), path) : tr("Export failed: %1").arg(err));
    return ok;
}

bool SettingsDialog::removeSelectedTheme() {
    const QString name = m_theme->currentText();
    QString err;
    if (!hn::theme::removeTheme(name, &err)) { showNote(tr("Could not remove: %1").arg(err)); return false; }
    refreshThemes("modernist");   // blocked signals: apply the fallback explicitly
    apply();
    showNote(tr("Removed theme \"%1\".").arg(name));
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

void SettingsDialog::apply(bool fontEdited) {
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
        if (id.isEmpty()) continue; // fixed shortcuts and Markdown typing tips are reference rows
        const QKeySequence ks = static_cast<QKeySequenceEdit *>(m_keys->cellWidget(r, 1))->keySequence();
        if (id.startsWith(QLatin1String("plugin:"))) {   // only rows the user changed are persisted; the plugin default stays a default
            if (ks.toString(QKeySequence::PortableText) != m_keys->item(r, 0)->data(Qt::UserRole + 2).toString()) s.keybindings[id] = ks;
            continue;
        }
        s.keybindings[id] = ks;
    }
    AppPrefs p = m_c->prefs();
    if (fontEdited) s.fontSize = p.fontSize = m_font->value();
    m_c->applySettings(s, p);
}

} // namespace hn::app
