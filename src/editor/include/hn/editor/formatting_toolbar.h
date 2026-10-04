#pragma once
#include <QPointer>
#include <QSet>
#include <QWidget>

#include "hn/editor/note_editor.h"

class QToolButton;
class QMenu;
class QAction;

namespace hn::editor {

// Fixed-height strip: mouse formatting keeps the caret; keyboard users can enter and traverse the controls.
class FormattingToolbar : public QWidget {
    Q_OBJECT
public:
    static constexpr int kHeight = 36;
    explicit FormattingToolbar(NoteEditor *editor, QWidget *parent = nullptr);
    void setTheme(const hn::theme::Theme &t);
    void focusFirstControl();
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    // Additive: plugin buttons live in a "PLUGINS" section at the end of the overflow ("more") menu, so the strip itself never
    // changes size. Replaces the previous set; empty removes the section.
    struct PluginButton { QString id, title, icon; };
    void setPluginButtons(const QList<PluginButton> &buttons);
    QMenu *moreMenu() const;
signals:
    void pluginButtonTriggered(const QString &id);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    bool eventFilter(QObject *, QEvent *) override;

private:
    QToolButton *add(const QString &name, const QString &icon, const QString &tip, bool checkable);
    void updateState();
    void retint();
    void updateOverflow();
    void runCommand(const std::function<void()> &command);
    void restoreFocus(QToolButton *control);
    void watchMenu(QMenu *menu);
    NoteEditor *m_ed;
    hn::theme::Theme m_theme;
    QToolButton *m_bold, *m_italic, *m_style, *m_bullet, *m_numbered, *m_check, *m_link, *m_more;
    QAction *m_aStrike, *m_aCode, *m_aCodeBlock, *m_aModeToggle;
    QList<QToolButton *> m_buttons;
    QList<QAction *> m_pluginActions;
    QList<PluginButton> m_pluginButtons;
    QList<QAction *> m_overflowActions;
    QPointer<QToolButton> m_keyboardControl;
    QPointer<QToolButton> m_menuControl;
    QSet<QMenu *> m_openMenus;
    int m_menuDepth = 0;
    bool m_inCommand = false;
};

} // namespace hn::editor
