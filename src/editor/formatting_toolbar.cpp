#include "hn/editor/formatting_toolbar.h"

#include <QHBoxLayout>
#include <QMenu>
#include <QPainter>
#include <QToolButton>

namespace hn::editor {

QToolButton *FormattingToolbar::add(const QString &name, const QString &icon, const QString &tip, bool checkable)
{
    auto *b = new QToolButton(this);
    b->setObjectName(name);
    b->setProperty("iconName", icon);
    b->setToolTip(tip);
    b->setAccessibleName(tip.section(QLatin1String(" ("), 0, 0));
    b->setFocusPolicy(Qt::NoFocus);   // clicking must not move focus away from the editor
    b->setCheckable(checkable);
    b->setFixedSize(32, 28);
    b->setIconSize(QSize(16, 16));
    b->setAutoRaise(false);
    m_buttons.append(b);
    layout()->addWidget(b);
    return b;
}

FormattingToolbar::FormattingToolbar(NoteEditor *editor, QWidget *parent) : QWidget(parent), m_ed(editor)
{
    setFixedHeight(kHeight);
    setFocusPolicy(Qt::NoFocus);
    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(8, 0, 8, 1);
    lay->setSpacing(2);

    m_bold = add(QStringLiteral("bold"), QStringLiteral("bold"), tr("Bold (Ctrl+B)"), true);
    m_italic = add(QStringLiteral("italic"), QStringLiteral("italic"), tr("Italic (Ctrl+I)"), true);
    m_style = add(QStringLiteral("style"), QStringLiteral("h1"), tr("Text style"), false);
    m_style->setFixedWidth(44);
    m_style->setPopupMode(QToolButton::InstantPopup);
    m_bullet = add(QStringLiteral("bullet"), QStringLiteral("list-ul"), tr("Bulleted list"), true);
    m_numbered = add(QStringLiteral("numbered"), QStringLiteral("list-ol"), tr("Numbered list"), true);
    m_check = add(QStringLiteral("checklist"), QStringLiteral("check"), tr("Checklist"), true);
    m_link = add(QStringLiteral("link"), QStringLiteral("link"), tr("Link (Ctrl+K)"), false);
    lay->addStretch(1);
    m_more = add(QStringLiteral("more"), QStringLiteral("more"), tr("More formatting"), false);
    m_more->setPopupMode(QToolButton::InstantPopup);

    auto done = [this] { updateState(); m_ed->focusEditor(); };
    connect(m_bold, &QToolButton::clicked, this, [=, this] { m_ed->toggleInline(InlineStyle::Bold); done(); });
    connect(m_italic, &QToolButton::clicked, this, [=, this] { m_ed->toggleInline(InlineStyle::Italic); done(); });
    connect(m_bullet, &QToolButton::clicked, this, [=, this] { m_ed->toggleList(ListKind::Bullet); done(); });
    connect(m_numbered, &QToolButton::clicked, this, [=, this] { m_ed->toggleList(ListKind::Ordered); done(); });
    connect(m_check, &QToolButton::clicked, this, [=, this] { m_ed->toggleList(ListKind::Check); done(); });
    connect(m_link, &QToolButton::clicked, this, [=, this] { m_ed->editLink(); done(); });

    auto *sm = new QMenu(m_style);
    sm->setObjectName(QStringLiteral("styleMenu"));
    const struct { const char *text; BlockStyle s; } styles[] = {
        {"Paragraph", BlockStyle::Paragraph}, {"Heading 1", BlockStyle::H1}, {"Heading 2", BlockStyle::H2},
        {"Heading 3", BlockStyle::H3}, {"Quote", BlockStyle::Quote}, {"Code block", BlockStyle::Code}};
    for (const auto &st : styles) {
        auto *a = sm->addAction(tr(st.text));
        connect(a, &QAction::triggered, this, [=, this, s = st.s] { m_ed->setBlockStyle(s); done(); });
    }
    m_style->setMenu(sm);

    auto *mm = new QMenu(m_more);
    mm->setObjectName(QStringLiteral("moreMenu"));
    m_aStrike = mm->addAction(tr("Strikethrough"));
    m_aCode = mm->addAction(tr("Inline code"));
    m_aCodeBlock = mm->addAction(tr("Code block"));
    connect(m_aStrike, &QAction::triggered, this, [=, this] { m_ed->toggleInline(InlineStyle::Strike); done(); });
    connect(m_aCode, &QAction::triggered, this, [=, this] { m_ed->toggleInline(InlineStyle::Code); done(); });
    connect(m_aCodeBlock, &QAction::triggered, this, [=, this] { m_ed->setBlockStyle(BlockStyle::Code); done(); });
    const struct { const char *text; BlockStyle s; } more[] = {{"Heading 4", BlockStyle::H4}, {"Heading 5", BlockStyle::H5}, {"Heading 6", BlockStyle::H6}};
    for (const auto &st : more) {
        auto *a = mm->addAction(tr(st.text));
        connect(a, &QAction::triggered, this, [=, this, s = st.s] { m_ed->setBlockStyle(s); done(); });
    }
    mm->addSeparator();
    m_aModeToggle = mm->addAction(tr("Switch to source mode"));
    connect(m_aModeToggle, &QAction::triggered, this, [=, this] {
        m_ed->setMode(m_ed->mode() == Mode::Visual ? Mode::Source : Mode::Visual);
        done();
    });
    m_more->setMenu(mm);

    connect(m_ed, &NoteEditor::formatStateChanged, this, &FormattingToolbar::updateState);
    connect(m_ed, &NoteEditor::modeChanged, this, [this] { updateState(); });
    setTheme(hn::theme::Theme());
}

void FormattingToolbar::setTheme(const hn::theme::Theme &t)
{
    m_theme = t;
    retint();
}

void FormattingToolbar::retint()
{
    const QColor tint = m_theme.text.isValid() ? m_theme.text : palette().color(QPalette::WindowText);
    for (auto *b : m_buttons) b->setIcon(hn::theme::icon(b->property("iconName").toString(), tint));
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
        connect(a, &QAction::triggered, this, [this, id = b.id] { emit pluginButtonTriggered(id); m_ed->focusEditor(); });
        m_pluginActions << a;
    }
}

QSize FormattingToolbar::sizeHint() const { return QSize(320, kHeight); }

void FormattingToolbar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), m_theme.bg.isValid() ? m_theme.bg : palette().window().color());
    p.setPen(m_theme.border.isValid() ? m_theme.border : palette().mid().color());
    p.drawLine(0, height() - 1, width(), height() - 1);   // hairline, square, flat
}

} // namespace hn::editor
