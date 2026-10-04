#include "hn/editor/formatting_toolbar.h"

#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QShortcut>
#include <QTimer>
#include <QToolButton>

namespace hn::editor {

QToolButton *FormattingToolbar::add(const QString &name, const QString &icon, const QString &tip, bool checkable)
{
    auto *b = new QToolButton(this);
    b->setObjectName(name);
    b->setProperty("iconName", icon);
    b->setToolTip(tip);
    b->setAccessibleName(tip.section(QLatin1String(" ("), 0, 0));
    b->setFocusPolicy(Qt::TabFocus);   // keyboard reachability without click-to-focus
    b->installEventFilter(this);
    b->setCheckable(checkable);
    b->setFixedSize(32, 28);
    b->setIconSize(QSize(16, 16));
    b->setAutoRaise(false);
    m_buttons.append(b);
    return b;
}

FormattingToolbar::FormattingToolbar(NoteEditor *editor, QWidget *parent) : QWidget(parent), m_ed(editor)
{
    setFixedHeight(kHeight);
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    m_bold = add(QStringLiteral("bold"), QStringLiteral("bold"), tr("Bold (Ctrl+B)"), true);
    m_italic = add(QStringLiteral("italic"), QStringLiteral("italic"), tr("Italic (Ctrl+I)"), true);
    m_style = add(QStringLiteral("style"), QStringLiteral("h1"), tr("Text style"), false);
    m_style->setFixedWidth(44);
    m_style->setPopupMode(QToolButton::InstantPopup);
    m_bullet = add(QStringLiteral("bullet"), QStringLiteral("list-ul"), tr("Bulleted list"), true);
    m_numbered = add(QStringLiteral("numbered"), QStringLiteral("list-ol"), tr("Numbered list"), true);
    m_check = add(QStringLiteral("checklist"), QStringLiteral("check"), tr("Checklist"), true);
    m_link = add(QStringLiteral("link"), QStringLiteral("link"), tr("Link (Ctrl+K)"), false);
    m_more = add(QStringLiteral("more"), QStringLiteral("more"), tr("More formatting"), false);
    m_more->setPopupMode(QToolButton::InstantPopup);

    connect(m_bold, &QToolButton::clicked, this, [this] { runCommand([this] { m_ed->toggleInline(InlineStyle::Bold); }); });
    connect(m_italic, &QToolButton::clicked, this, [this] { runCommand([this] { m_ed->toggleInline(InlineStyle::Italic); }); });
    connect(m_bullet, &QToolButton::clicked, this, [this] { runCommand([this] { m_ed->toggleList(ListKind::Bullet); }); });
    connect(m_numbered, &QToolButton::clicked, this, [this] { runCommand([this] { m_ed->toggleList(ListKind::Ordered); }); });
    connect(m_check, &QToolButton::clicked, this, [this] { runCommand([this] { m_ed->toggleList(ListKind::Check); }); });
    connect(m_link, &QToolButton::clicked, this, [this] { runCommand([this] { m_ed->editLink(); }); });

    auto *sm = new QMenu(m_style);
    sm->setTitle(tr("Text style"));
    sm->setObjectName(QStringLiteral("styleMenu"));
    const struct { const char *text; BlockStyle s; } styles[] = {
        {"Paragraph", BlockStyle::Paragraph}, {"Heading 1", BlockStyle::H1}, {"Heading 2", BlockStyle::H2},
        {"Heading 3", BlockStyle::H3}, {"Quote", BlockStyle::Quote}, {"Code block", BlockStyle::Code}};
    for (const auto &st : styles) {
        auto *a = sm->addAction(tr(st.text));
        a->setCheckable(true);
        a->setData(int(st.s));
        connect(a, &QAction::triggered, this, [this, s = st.s] { runCommand([this, s] { m_ed->setBlockStyle(s); }); });
    }
    m_style->setMenu(sm);

    auto *mm = new QMenu(m_more);
    mm->setObjectName(QStringLiteral("moreMenu"));
    for (auto *button : {m_style, m_bullet, m_numbered, m_check, m_link}) {
        auto *a = button == m_style ? mm->addMenu(sm) : mm->addAction(button->accessibleName());
        a->setCheckable(button->isCheckable());
        if (button == m_link) {
            a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_K));
            a->setShortcutContext(Qt::WidgetShortcut);
        }
        if (button != m_style) connect(a, &QAction::triggered, button, &QToolButton::click);
        m_overflowActions << a;
    }
    m_aStrike = mm->addAction(tr("Strikethrough"));
    m_aCode = mm->addAction(tr("Inline code"));
    m_aCodeBlock = mm->addAction(tr("Code block"));
    connect(m_aStrike, &QAction::triggered, this, [this] { runCommand([this] { m_ed->toggleInline(InlineStyle::Strike); }); });
    connect(m_aCode, &QAction::triggered, this, [this] { runCommand([this] { m_ed->toggleInline(InlineStyle::Code); }); });
    connect(m_aCodeBlock, &QAction::triggered, this, [this] { runCommand([this] { m_ed->setBlockStyle(BlockStyle::Code); }); });
    const struct { const char *text; BlockStyle s; } more[] = {{"Heading 4", BlockStyle::H4}, {"Heading 5", BlockStyle::H5}, {"Heading 6", BlockStyle::H6}};
    for (const auto &st : more) {
        auto *a = mm->addAction(tr(st.text));
        connect(a, &QAction::triggered, this, [this, s = st.s] { runCommand([this, s] { m_ed->setBlockStyle(s); }); });
    }
    mm->addSeparator();
    m_aModeToggle = mm->addAction(tr("Switch to source mode"));
    connect(m_aModeToggle, &QAction::triggered, this, [this] {
        runCommand([this] { m_ed->setMode(m_ed->mode() == Mode::Visual ? Mode::Source : Mode::Visual); });
    });
    m_more->setMenu(mm);
    watchMenu(sm);
    watchMenu(mm);
    for (auto *button : m_buttons) {
        connect(button, &QToolButton::pressed, this, &FormattingToolbar::retint);
        connect(button, &QToolButton::released, this, &FormattingToolbar::retint);
    }
    m_ed->visualEdit()->installEventFilter(this);
    m_ed->sourceEdit()->installEventFilter(this);
    auto *entry = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_F10), m_ed);
    entry->setContext(Qt::WidgetWithChildrenShortcut);
    connect(entry, &QShortcut::activated, this, &FormattingToolbar::focusFirstControl);

    connect(m_ed, &NoteEditor::formatStateChanged, this, &FormattingToolbar::updateState);
    connect(m_ed, &NoteEditor::modeChanged, this, [this] { updateState(); });
    setTheme(hn::theme::Theme());
    updateOverflow();
}

void FormattingToolbar::setTheme(const hn::theme::Theme &t)
{
    m_theme = t;
    // Local rules also serve standalone toolbars. Keep geometry constant in every state.
    const QColor text = t.text.isValid() ? t.text : palette().color(QPalette::WindowText);
    const QColor selection = t.selection.isValid() ? t.selection : palette().color(QPalette::Midlight);
    const QColor accent = t.accent.isValid() ? t.accent : palette().color(QPalette::Highlight);
    const QColor accentText = t.accentText.isValid() ? t.accentText : palette().color(QPalette::HighlightedText);
    setStyleSheet(QStringLiteral(
        "QToolButton { background: transparent; color: %1; border: 1px solid transparent; border-radius: %5px;"
        " padding: 0; min-width: 0; min-height: 0; }"
        "QToolButton:hover, QToolButton:checked { background: %2; color: %1; }"
        "QToolButton:checked { border-color: %3; }"
        "QToolButton:pressed { background: %3; color: %4; }"
        "QToolButton:focus { border: 2px solid %3; padding: 0; }"
        "QToolButton:pressed:focus { border-color: %4; }"
        "QToolButton::menu-indicator { image: none; }")
        .arg(text.name(), selection.name(), accent.name(), accentText.name()).arg(t.radius));
    retint();
    updateOverflow();
    update();
}

void FormattingToolbar::retint()
{
    const QColor tint = m_theme.text.isValid() ? m_theme.text : palette().color(QPalette::WindowText);
    for (auto *b : m_buttons) {
        const QColor pressed = m_theme.accentText.isValid() ? m_theme.accentText : palette().color(QPalette::HighlightedText);
        const bool down = b->isDown() || m_openMenus.contains(b->menu());
        b->setIcon(hn::theme::icon(b->property("iconName").toString(), down ? pressed : tint));
    }
    m_aStrike->setIcon(hn::theme::icon(QStringLiteral("strike"), tint));
    m_aCode->setIcon(hn::theme::icon(QStringLiteral("code"), tint));
    m_aCodeBlock->setIcon(hn::theme::icon(QStringLiteral("code"), tint));
    updateState();
}

void FormattingToolbar::updateState()
{
    const bool vis = m_ed->mode() == Mode::Visual && !m_ed->isReadOnly();
    for (auto *b : {m_bold, m_italic, m_style, m_bullet, m_numbered, m_check, m_link}) b->setEnabled(vis);
    for (auto *a : {m_aStrike, m_aCode, m_aCodeBlock}) a->setEnabled(vis);
    for (auto *a : m_more->menu()->actions()) if (a != m_aModeToggle && !a->isSeparator() && !a->property("hnPlugin").isValid()) a->setEnabled(vis);
    m_aModeToggle->setEnabled(!m_ed->isReadOnly());
    m_aModeToggle->setText(m_ed->mode() == Mode::Visual ? tr("Switch to source mode") : tr("Switch to visual mode"));
    const QColor tint = m_theme.text.isValid() ? m_theme.text : palette().color(QPalette::WindowText);
    for (auto *a : std::as_const(m_pluginActions)) if (!a->icon().isNull()) a->setIcon(hn::theme::icon(m_pluginButtons.value(m_pluginActions.indexOf(a) - 1).icon, tint));
    m_aModeToggle->setIcon(hn::theme::icon(m_ed->mode() == Mode::Visual ? QStringLiteral("source") : QStringLiteral("visual"), tint));
    m_bold->setChecked(vis && m_ed->inlineActive(InlineStyle::Bold));
    m_italic->setChecked(vis && m_ed->inlineActive(InlineStyle::Italic));
    const ListKind k = vis ? m_ed->listKind() : ListKind::None;
    m_bullet->setChecked(k == ListKind::Bullet);
    m_numbered->setChecked(k == ListKind::Ordered);
    m_check->setChecked(k == ListKind::Check);
    for (auto *a : m_style->menu()->actions()) a->setChecked(vis && int(m_ed->blockStyle()) == a->data().toInt());
    const QList<QToolButton *> secondary{m_style, m_bullet, m_numbered, m_check, m_link};
    for (int i = 0; i < secondary.size(); ++i) {
        m_overflowActions[i]->setEnabled(secondary[i]->isEnabled());
        m_overflowActions[i]->setChecked(secondary[i]->isChecked());
    }
}

QMenu *FormattingToolbar::moreMenu() const { return m_more->menu(); }

void FormattingToolbar::setPluginButtons(const QList<PluginButton> &buttons)
{
    m_pluginButtons = buttons;
    QMenu *mm = m_more->menu();
    for (auto *a : std::as_const(m_pluginActions)) { mm->removeAction(a); delete a; }
    m_pluginActions.clear();
    if (buttons.isEmpty()) return;
    auto *sep = mm->addSection(tr("PLUGINS"));   // a labelled separator: marks the section, does not trigger
    sep->setProperty("hnPlugin", true);
    m_pluginActions << sep;
    const QColor tint = m_theme.text.isValid() ? m_theme.text : palette().color(QPalette::WindowText);
    for (const auto &b : buttons) {
        auto *a = mm->addAction(b.title);
        a->setProperty("hnPlugin", b.id);
        if (!b.icon.isEmpty()) a->setIcon(hn::theme::icon(b.icon, tint));
        connect(a, &QAction::triggered, this, [this, id = b.id] { runCommand([this, id] { emit pluginButtonTriggered(id); }); });
        m_pluginActions << a;
    }
}

QSize FormattingToolbar::sizeHint() const { return QSize(320, kHeight); }
QSize FormattingToolbar::minimumSizeHint() const { return QSize(116, kHeight); }

void FormattingToolbar::focusFirstControl()
{
    if (!isVisible()) return;
    for (auto *b : m_buttons) if (b->isVisible() && b->isEnabled()) {
        m_keyboardControl = b;
        b->setFocus(Qt::ShortcutFocusReason);
        return;
    }
}

void FormattingToolbar::restoreFocus(QToolButton *control)
{
    if (control) {
        if (control->isVisible() && control->isEnabled()) {
            m_keyboardControl = control;
            control->setFocus(Qt::ShortcutFocusReason);
        } else focusFirstControl();
    } else m_ed->focusEditor();
}

void FormattingToolbar::runCommand(const std::function<void()> &command)
{
    const QPointer<QToolButton> control = m_menuControl ? m_menuControl : m_keyboardControl;
    m_inCommand = true;
    command();
    m_inCommand = false;
    updateState();
    restoreFocus(control);
}

void FormattingToolbar::watchMenu(QMenu *menu)
{
    connect(menu, &QMenu::aboutToShow, this, [this, menu] {
        if (m_menuDepth++ == 0) m_menuControl = m_keyboardControl;
        // InstantPopup enters its menu loop before our later pressed connection runs.
        m_openMenus.insert(menu);
        retint();
    });
    connect(menu, &QMenu::aboutToHide, this, [this, menu] {
        m_openMenus.remove(menu);
        --m_menuDepth;
        const QPointer<QToolButton> control = m_menuControl;
        // QToolButton finishes its popup focus handling after aboutToHide/triggered.
        QTimer::singleShot(0, this, [this, control] {
            retint(); // Qt has now cleared its popup pressed state, even if the focus target changed.
            if (m_menuDepth || m_menuControl != control || m_keyboardControl != control) return;
            m_menuControl.clear();
            restoreFocus(control);
        });
    });
}

bool FormattingToolbar::eventFilter(QObject *object, QEvent *event)
{
    auto *button = qobject_cast<QToolButton *>(object);
    if (!button) {
        if (!m_inCommand && !m_menuDepth) {
            const bool input = event->type() == QEvent::MouseButtonPress || event->type() == QEvent::KeyPress;
            const bool focus = event->type() == QEvent::FocusIn
                               && static_cast<QFocusEvent *>(event)->reason() != Qt::PopupFocusReason;
            if (input || focus) { m_keyboardControl.clear(); m_menuControl.clear(); }
        }
        return QWidget::eventFilter(object, event);
    }
    if (event->type() == QEvent::MouseButtonPress) { m_keyboardControl.clear(); m_menuControl.clear(); }
    if (event->type() == QEvent::FocusIn) {
        if (!m_menuDepth) m_menuControl.clear(); // a new target supersedes a queued menu return
        m_keyboardControl = button;
    }
    if (event->type() == QEvent::KeyPress) {
        m_keyboardControl = button;
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape) {
            m_keyboardControl.clear();
            m_ed->focusEditor();
            return true;
        }
        if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
            const int step = key->key() == Qt::Key_Backtab || (key->modifiers() & Qt::ShiftModifier) ? -1 : 1;
            const int count = m_buttons.size(), start = m_buttons.indexOf(button);
            for (int i = 1; i <= count; ++i) {
                auto *next = m_buttons[(start + count + i * step) % count];
                if (next->isVisible() && next->isEnabled()) {
                    next->setFocus(step > 0 ? Qt::TabFocusReason : Qt::BacktabFocusReason);
                    return true;
                }
            }
        }
    }
    return QWidget::eventFilter(object, event);
}

void FormattingToolbar::updateOverflow()
{
    const QList<QToolButton *> secondary{m_style, m_bullet, m_numbered, m_check, m_link};
    const auto controlWidth = [this](QToolButton *b) { return b == m_style ? 44 : 32; };
    int used = 16 + (m_buttons.size() - 1) * 2;
    for (auto *b : m_buttons) used += controlWidth(b);
    int keep = secondary.size();
    while (keep > 0 && used > width()) used -= controlWidth(secondary[--keep]) + 2;
    for (int i = 0; i < secondary.size(); ++i) {
        const bool moveFocus = i >= keep && secondary[i]->hasFocus();
        secondary[i]->setVisible(i < keep);
        m_overflowActions[i]->setVisible(i >= keep);
        if (moveFocus) { m_keyboardControl = m_more; m_more->setFocus(Qt::TabFocusReason); }
    }
    // Fixed hit targets must not shrink to the icon's size hint under inherited QSS.
    int x = 8;
    for (auto *b : m_buttons) {
        if (b == m_more) b->setGeometry(width() - 40, 3, 32, 28);
        else if (!b->isHidden()) {
            b->setGeometry(x, 3, controlWidth(b), 28);
            x += controlWidth(b) + 2;
        }
    }
}

void FormattingToolbar::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateOverflow();
}

void FormattingToolbar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), m_theme.bg.isValid() ? m_theme.bg : palette().window().color());
    p.setPen(m_theme.border.isValid() ? m_theme.border : palette().mid().color());
    p.drawLine(0, height() - 1, width(), height() - 1);   // hairline, square, flat
}

} // namespace hn::editor
