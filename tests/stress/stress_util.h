// Shared helpers for the P8 stress executables: metrics, checks, PSS/CPU sampling, fixtures, sandbox env.
#pragma once
#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <vector>

namespace st {

// ---------------------------------------------------------------- reporting
inline int g_fail = 0, g_xfail = 0, g_warn = 0, g_skip = 0, g_pass = 0;
inline void metric(const char *name, double v, const char *unit = "") {
    std::printf("METRIC %-52s %14.3f %s\n", name, v, unit);
    std::fflush(stdout);
}
inline void note(const QString &s) { std::printf("NOTE   %s\n", qPrintable(s)); std::fflush(stdout); }
inline void check(bool ok, const QString &what) {
    std::printf("%s %s\n", ok ? "PASS  " : "FAIL  ", qPrintable(what));
    std::fflush(stdout);
    ok ? ++g_pass : ++g_fail;
}
// Known defect: failing is recorded (not counted as a failure); passing prints XPASS (defect fixed).
inline void xfail(bool ok, const QString &defectId, const QString &what) {
    if (ok) { std::printf("XPASS  [%s] %s\n", qPrintable(defectId), qPrintable(what)); ++g_pass; }
    else { std::printf("XFAIL  [%s] %s\n", qPrintable(defectId), qPrintable(what)); ++g_xfail; }
    std::fflush(stdout);
}
// Aspirational / informational threshold that is not a spec target.
inline void soft(bool ok, const QString &what) {
    std::printf("%s %s\n", ok ? "OK    " : "WARN  ", qPrintable(what));
    std::fflush(stdout);
    if (!ok) ++g_warn;
}
inline void skip(const QString &what) { std::printf("SKIP   %s\n", qPrintable(what)); ++g_skip; std::fflush(stdout); }
inline int finish(const char *name) {
    std::printf("SUMMARY %s: pass=%d fail=%d xfail=%d warn=%d skip=%d\n", name, g_pass, g_fail, g_xfail, g_warn, g_skip);
    return g_fail ? 1 : 0;
}

// ---------------------------------------------------------------- statistics
struct Dist {
    std::vector<double> v;
    void add(double x) { v.push_back(x); }
    size_t n() const { return v.size(); }
    double pct(double p) const {
        if (v.empty()) return 0;
        auto s = v; std::sort(s.begin(), s.end());
        return s[std::min(s.size() - 1, size_t(p * s.size()))];
    }
    double mx() const { return v.empty() ? 0 : *std::max_element(v.begin(), v.end()); }
    double mean() const { double a = 0; for (double x : v) a += x; return v.empty() ? 0 : a / v.size(); }
    void report(const QString &name, const char *unit = "ms") const {
        metric(qPrintable(name + ".n"), double(n()), "");
        metric(qPrintable(name + ".p50"), pct(.5), unit);
        metric(qPrintable(name + ".p95"), pct(.95), unit);
        metric(qPrintable(name + ".p99"), pct(.99), unit);
        metric(qPrintable(name + ".max"), mx(), unit);
    }
};
struct Stopwatch {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    double ms() const { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
    void reset() { t0 = std::chrono::steady_clock::now(); }
};

// ---------------------------------------------------------------- process memory / cpu (Linux)
struct Mem { double rssMiB = 0, pssMiB = 0, swapPssMiB = 0, privateMiB = 0, anonMiB = 0; double total() const { return pssMiB + swapPssMiB; } };
inline Mem memOf(long pid) {
    Mem m;
    QFile f(QString("/proc/%1/smaps_rollup").arg(pid));
    if (!f.open(QIODevice::ReadOnly)) return m;
    double priv = 0;
    for (const QByteArray &l : f.readAll().split('\n')) {
        const QList<QByteArray> p = l.simplified().split(' ');
        if (p.size() < 2) continue;
        const double mib = p[1].toDouble() / 1024.0;
        if (p[0] == "Rss:") m.rssMiB = mib;
        else if (p[0] == "Pss:") m.pssMiB = mib;
        else if (p[0] == "SwapPss:") m.swapPssMiB = mib;
        else if (p[0] == "Pss_Anon:") m.anonMiB = mib;
        else if (p[0] == "Private_Clean:" || p[0] == "Private_Dirty:") priv += mib;
    }
    m.privateMiB = priv;
    return m;
}
inline Mem mem() { return memOf(getpid()); }
// utime+stime seconds of a pid (all threads), from /proc/<pid>/stat.
inline double cpuSecondsOf(long pid) {
    QFile f(QString("/proc/%1/stat").arg(pid));
    if (!f.open(QIODevice::ReadOnly)) return 0;
    const QByteArray b = f.readAll();
    const QList<QByteArray> p = b.mid(b.lastIndexOf(')') + 2).split(' ');   // field 3 = p[0]; utime=14 -> p[11]
    if (p.size() < 13) return 0;
    return (p[11].toDouble() + p[12].toDouble()) / double(sysconf(_SC_CLK_TCK));
}
inline double cpuSeconds() { return cpuSecondsOf(getpid()); }
inline int threadCount() {
    QFile f("/proc/self/status");
    if (!f.open(QIODevice::ReadOnly)) return -1;
    for (const QByteArray &l : f.readAll().split('\n')) if (l.startsWith("Threads:")) return l.mid(8).trimmed().toInt();
    return -1;
}
inline int openFds() { return QDir("/proc/self/fd").entryList(QDir::NoDotAndDotDot).size(); }

// ---------------------------------------------------------------- sandbox (never touches the user's files)
struct Sandbox {
    QTemporaryDir dir;
    QString notes, config, state, cache, data, runtime;
    explicit Sandbox(const QString &tag = "p8") : dir(qEnvironmentVariable("HN_STRESS_TMPDIR", QDir::tempPath()) + "/hn-" + tag + "-XXXXXX") {
        notes = dir.filePath("notes"); config = dir.filePath("config"); state = dir.filePath("state");
        cache = dir.filePath("cache"); data = dir.filePath("data"); runtime = dir.filePath("run");
        for (auto *p : {&notes, &config, &state, &cache, &data, &runtime}) QDir().mkpath(*p);
        QFile(runtime).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        qputenv("HN_NOTES_DIR", notes.toUtf8()); qputenv("HN_CONFIG_DIR", config.toUtf8());
        qputenv("HN_STATE_DIR", state.toUtf8()); qputenv("HN_CACHE_DIR", cache.toUtf8()); qputenv("HN_DATA_DIR", data.toUtf8());
        qputenv("XDG_CONFIG_HOME", dir.filePath("xdg-config").toUtf8());   // theme loader honours XDG_CONFIG_HOME only
        qputenv("XDG_DATA_HOME", dir.filePath("xdg-data").toUtf8());
        qputenv("XDG_STATE_HOME", dir.filePath("xdg-state").toUtf8());
        qputenv("XDG_CACHE_HOME", dir.filePath("xdg-cache").toUtf8());
        qputenv("XDG_RUNTIME_DIR", runtime.toUtf8());
        qputenv("WAYLAND_DISPLAY", ("hnstress-" + tag).toUtf8());
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
};

// ---------------------------------------------------------------- fixtures (reproducible, no personal notes)
inline QByteArray mixedBlock(int i) {
    return QByteArray("## Section ") + QByteArray::number(i) + "\n\n"
        "Paragraph with **bold**, *italic*, ~~strike~~ and `code`, plus a [link](https://example.org/" + QByteArray::number(i) + ") and "
        "enough plain words to wrap across a couple of lines in a narrow window so layout has real work to do.\n\n"
        "- first item\n  - nested item\n- [ ] pending\n- [x] complete\n\n1. numbered item\n2. another item\n\n"
        "> A quoted paragraph " + QByteArray::number(i) + ".\n\n```cpp\nint answer = " + QByteArray::number(i) + ";\n```\n\n"
        "Unicode: na\xC3\xAFve, \xCE\x95\xCE\xBB\xCE\xBB\xCE\xB7\xCE\xBD\xCE\xB9\xCE\xBA\xCE\xAC, \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E, "
        "\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9, \xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB.\n\n";
}
// Deterministic mixed-content note of exactly ~bytes bytes (seed makes notes distinct; title is unique for search).
inline QByteArray mixedNote(qsizetype bytes, int seed = 0) {
    QByteArray md = "# Note " + QByteArray::number(seed) + " uniq" + QByteArray::number(seed) + "\n\n#tag" + QByteArray::number(seed % 20) + " #stress\n\n";
    int i = seed * 7919;
    while (md.size() < bytes) md += mixedBlock(i++);
    md.truncate(bytes - 1);
    int cut = md.lastIndexOf('\n'); if (cut > 0) md.truncate(cut);   // never cut mid-UTF-8 sequence
    return md + "\n";
}
inline bool writeFile(const QString &p, const QByteArray &b) {
    QDir().mkpath(QFileInfo(p).absolutePath());
    QFile f(p);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(b) == b.size();
}
// Editor-style atomic replace (write temp, rename(2) over target; QFile::rename refuses to overwrite).
inline bool replaceFile(const QString &p, const QByteArray &b) { return writeFile(p + ".x", b) && ::rename(QFile::encodeName(p + ".x").constData(), QFile::encodeName(p).constData()) == 0; }
inline QByteArray readFile(const QString &p) { QFile f(p); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }

// xorshift RNG so runs are reproducible (seed printed by callers)
struct Rng {
    quint64 s;
    explicit Rng(quint64 seed = 0x9E3779B97F4A7C15ull) : s(seed ? seed : 1) {}
    quint64 next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    int bounded(int n) { return int(next() % quint64(n)); }
    bool chance(int pct) { return bounded(100) < pct; }
};

} // namespace st
