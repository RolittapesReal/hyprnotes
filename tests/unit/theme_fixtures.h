#pragma once
#include <QByteArray>
// Raw-string fixtures live here: moc cannot parse them reliably inside a test class file.
static const QByteArray kBase16 = R"(scheme: "Test Dark"
author: "x"
base00: "181818"
base01: "282828"
base02: "383838"
base03: "585858"
base04: "b8b8b8"
base05: "d8d8d8"
base06: "e8e8e8"
base07: "f8f8f8"
base08: "ab4642"
base09: "dc9656"
base0A: "f7ca88"
base0B: "a1b56c"
base0C: "86c1b9"
base0D: "7cafc2"
base0E: "ba8baf"
base0F: "a16946"
)";
static const QByteArray kVsCode = R"({"name":"My VS","type":"light","colors":{"editor.background":"#FFFFFF","editor.foreground":"#333333",
 "editor.selectionBackground":"#ADD6FF80","focusBorder":"#007FD4","sideBar.background":"#F3F3F3"}})";
// Single-quoted pseudo-JSON for test literals (moc chokes on raw strings with quotes in test class files).
inline QByteArray J(const char *s) { return QByteArray(s).replace('\'', '"'); }
