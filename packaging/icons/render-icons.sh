#!/bin/sh
# Renders hyprnotes.svg to hicolor PNGs with Qt (QSvgRenderer). Needs Qt6 Svg/Gui dev files and g++.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/r.cpp" <<'CPP'
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>
int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QSvgRenderer r{QString::fromLocal8Bit(argv[1])};
    if (!r.isValid()) return 1;
    for (int s : {16, 32, 48, 128, 256}) {
        QImage img(s, s, QImage::Format_ARGB32);
        img.fill(Qt::transparent);
        QPainter p(&img);
        r.render(&p);
        p.end();
        if (!img.save(QString("%1/hyprnotes-%2.png").arg(argv[2]).arg(s))) return 2;
    }
}
CPP
g++ -std=c++20 -fPIC "$tmp/r.cpp" -o "$tmp/r" $(pkg-config --cflags --libs Qt6Gui Qt6Svg)
"$tmp/r" "$here/hyprnotes.svg" "$here"
