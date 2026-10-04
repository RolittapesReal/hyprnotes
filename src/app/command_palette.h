#pragma once
// Flat modernist command palette (Ctrl+Shift+P): fuzzy filter over plugin commands, menu items and toolbar buttons, each row
// shows the owning plugin. Fully keyboard driven (type, Up/Down, Enter, Esc); a click on a row also runs it.
#include <QWidget>
#include <functional>

class QLineEdit;

namespace hn::app {

class CommandPalette : public QWidget {
    Q_OBJECT
public:
    struct Item { QString id, title, plugin, keyText; };
    static constexpr int kRow = 32, kMaxRows = 8, kInput = 44;
    // Subsequence match (case-insensitive). -1 = no match; higher is better (consecutive runs and word starts score more).
    static int fuzzyScore(const QString &query, const QString &text);
    // Opens (or closes, when already open on `host`) the palette. `run` receives the chosen id after the palette has closed.
    static CommandPalette *toggle(QWidget *host, const QList<Item> &items, std::function<void(const QString &id)> run);
    static CommandPalette *openOn(QWidget *host);
    QStringList visibleIds() const;
    QString currentId() const;
    QLineEdit *input() const { return m_input; }
    void setFilter(const QString &q);
    void activateCurrent();
    void dismiss();
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    bool eventFilter(QObject *o, QEvent *e) override;
private:
    CommandPalette(QWidget *host, const QList<Item> &items, std::function<void(const QString &)> run);
    void refilter();
    void place();
    void restyle();
    QList<Item> m_all, m_shown;
    QLineEdit *m_input;
    std::function<void(const QString &)> m_run;
    QWidget *m_host;
    QWidget *m_prevFocus = nullptr;
    int m_sel = 0, m_top = 0;
    int m_inputH = kInput, m_rowH = kRow, m_visibleRows = kMaxRows;
};

} // namespace hn::app
