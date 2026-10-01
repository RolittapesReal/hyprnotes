#pragma once
#include <QWidget>

#include "hn/editor/note_editor.h"

class QToolButton;
class QMenu;
class QAction;

namespace hn::editor {

// Fixed-height strip: buttons never take focus, so the editor selection/caret stay untouched.
class FormattingToolbar : public QWidget {
    Q_OBJECT
public:
    static constexpr int kHeight = 36;
    explicit FormattingToolbar(NoteEditor *editor, QWidget *parent = nullptr);
    void setTheme(const hn::theme::Theme &t);
    QSize sizeHint() const override;

    // Additive: plugin buttons live in a "PLUGINS" section at the end of the overflow ("more") menu, so the strip itself never
    // changes size. Replaces the previous set; empty removes the section.
    struct PluginButton { QString id, title, icon; };
    void setPluginButtons(const QList<PluginButton> &buttons);
    QMenu *moreMenu() const;
signals:
    void pluginButtonTriggered(const QString &id);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QToolButton *add(const QString &name, const QString &icon, const QString &tip, bool checkable);
    void updateState();
    void retint();
    NoteEditor *m_ed;
    hn::theme::Theme m_theme;
    QToolButton *m_bold, *m_italic, *m_style, *m_bullet, *m_numbered, *m_check, *m_link, *m_more;
    QAction *m_aStrike, *m_aCode, *m_aCodeBlock, *m_aModeToggle;
    QList<QToolButton *> m_buttons;
    QList<QAction *> m_pluginActions;
    QList<PluginButton> m_pluginButtons;
};

} // namespace hn::editor
