#pragma once
#include <QPointer>
#include <QPushButton>

namespace hn::app::ui {

class WrappingButton : public QPushButton {
public:
    explicit WrappingButton(const QString &text, QWidget *parent = nullptr);
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
protected:
    void paintEvent(QPaintEvent *) override;
};

class ActionRow : public QWidget {
public:
    explicit ActionRow(QWidget *parent = nullptr);
    void addButton(QPushButton *button);
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
protected:
    void resizeEvent(QResizeEvent *) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    int arrange(int width, bool apply) const;
    QList<QPointer<QPushButton>> m_buttons;
};

} // namespace hn::app::ui
