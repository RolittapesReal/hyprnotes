#include <hn/theme/theme.h>
#include <QGuiApplication>
#include <QHash>
#include <QPainter>
#include <QScreen>
#include <QPixmap>
#include <QSvgRenderer>

namespace hn::theme {
namespace {
// 16px grid, stroke drawn in @C@, square caps, mitred joins, no fill.
const QHash<QString, QString> &paths() {
    static const QHash<QString, QString> p{
        {"bold", "M4 2v12 M4 2h4.5a2.75 2.75 0 010 5.5H4 M4 7.5h5.5a3 3 0 010 6.5H4"},
        {"italic", "M6 2h6 M4 14h6 M10 2L6 14"},
        {"strike", "M2 8h12 M11.5 5C11 3.5 9.5 2.5 8 2.5c-2 0-3.5 1-3.5 2.5 0 1 .8 1.7 2 2.2 M4.5 11c.5 1.5 2 2.5 3.5 2.5 2 0 3.5-1 3.5-2.5"},
        {"code", "M5 4L1.5 8 5 12 M11 4l3.5 4L11 12"},
        {"h1", "M2.5 3v10 M2.5 8h6 M8.5 3v10 M12 6l1.5-1V13"},
        {"list-ul", "M6 4h8 M6 8h8 M6 12h8 M2 4h1.5 M2 8h1.5 M2 12h1.5"},
        {"list-ol", "M7 4h7 M7 8h7 M7 12h7 M2.5 3l1-.5V6 M2 10h2v1L2 13h2"},
        {"check", "M2.5 8.5l3.5 3.5 7.5-8"},
        {"quote", "M3 3v10 M6.5 4h7 M6.5 8h7 M6.5 12h4"},
        {"link", "M6.5 9.5l3-3 M7.5 4.5l1-1a2.5 2.5 0 013.5 3.5l-1 1 M8.5 11.5l-1 1A2.5 2.5 0 014 9l1-1"},
        {"close", "M3.5 3.5l9 9 M12.5 3.5l-9 9"},
        {"pin", "M6 2h4 M7 2v5L4.5 9.5h7L9 7V2 M8 9.5V14"},
        {"popout", "M9 2h5v5 M14 2L7.5 8.5 M12 10v4H2V4h4"},
        {"popin", "M2 9v5h5 M2 14l6.5-6.5 M6 2h8v8 M6 2v2 M14 10h-2"},
        {"search", "M7 2.5a4.5 4.5 0 100 9 4.5 4.5 0 000-9z M10.5 10.5L14 14"},
        {"plus", "M8 2.5v11 M2.5 8h11"},
        {"more", "M2.5 8h1.5 M7.25 8h1.5 M12 8h1.5"},
        {"tag", "M2 2h6l6 6-6 6-6-6z M5 5h1"},
        {"folder", "M2 3.5h4.5l1.5 2H14v8H2z"},
        {"source", "M6 2.5H4.5v4L3 8l1.5 1.5v4H6 M10 2.5h1.5v4L13 8l-1.5 1.5v4H10"},
        {"visual", "M1.5 8S4 3.5 8 3.5 14.5 8 14.5 8 12 12.5 8 12.5 1.5 8 1.5 8z M6.5 6.5h3v3h-3z"},
    };
    return p;
}

QString svgFor(const QString &name, const QColor &c) {
    return QString("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16' width='16' height='16'>"
                   "<path d='%1' fill='none' stroke='%2' stroke-width='1.5' stroke-linecap='square' stroke-linejoin='miter'/></svg>")
        .arg(paths().value(name), c.name(QColor::HexRgb));
}
} // namespace

QIcon icon(const QString &name, const QColor &tint, int px) {
    static QHash<QString, QIcon> cache;
    const QString key = QString("%1|%2|%3").arg(name, tint.name(QColor::HexArgb)).arg(px);
    if (auto it = cache.constFind(key); it != cache.constEnd()) return *it;
    QIcon ic;
    if (paths().contains(name)) {
        QSvgRenderer r(svgFor(name, tint).toUtf8());
        QList<qreal> dprs{1.0, 1.25, 1.5, 2.0};
        if (qGuiApp) for (auto *s : QGuiApplication::screens()) if (!dprs.contains(s->devicePixelRatio())) dprs << s->devicePixelRatio();
        for (qreal d : dprs) {
            QPixmap pm(qRound(px * d), qRound(px * d));
            pm.fill(Qt::transparent);
            pm.setDevicePixelRatio(d);
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            r.render(&p, QRectF(0, 0, px, px));
            p.end();
            ic.addPixmap(pm);
        }
    }
    cache.insert(key, ic);
    return ic;
}

} // namespace hn::theme
