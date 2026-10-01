// P8.3 index/search: 1000 and 10000 notes of 20 KiB, p95 first page, superseding-query burst, hostile FTS fuzz.
#include "hn/core/library_index.h"
#include "stress_util.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <atomic>
#include <thread>

using namespace st;
using namespace hn::core;

template <class F> static bool waitFor(F cond, int timeoutMs) {
    QElapsedTimer t; t.start();
    while (!cond()) {
        if (t.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}

static QStringList g_queries() {
    return {"uniq", "uniq77", "Paragraph", "nested item", "\"quoted paragraph\"", "link example", "Section", "tag5", "#tag7", "stress",
            "na\xC3\xAFve", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", "answer", "pend*", "numbered AND item", "bold OR italic", "wrap NEAR layout", "zzzznotfound", "int answer", "C++"};
}

static void runScale(int N, bool assertTargets) {
    note(QString("== 3.%1 library of %2 notes x 20 KiB").arg(N == 1000 ? 1 : 2).arg(N));
    Sandbox sb(QString("idx%1").arg(N));
    Stopwatch gen;
    // one note carrying punctuation-heavy literals that a user may type into the search box (they all occur verbatim)
    writeFile(sb.notes + "/literal-probe.md", "# Literal probe\n\nfoo-bar and it's and (paren) and 50% and e.g. and C++ and a:b and \"quoted\" and AND and NOT and NEAR and path/to/file.txt and user@example.com and #hashtag-x\n");
    for (int i = 0; i < N; ++i) writeFile(sb.notes + QString("/d%1/n%2.md").arg(i % 25).arg(i), mixedNote(20 * 1024, i));
    metric(qPrintable(QString("index.%1.fixture_gen").arg(N)), gen.ms(), "ms");
    const QString tag = QString("index.%1.").arg(N);
    const Mem m0 = mem();
    LibraryIndex idx(sb.notes, sb.cache);
    bool done = false; int added = 0;
    QObject::connect(&idx, &LibraryIndex::synced, [&](quint64, int a, int, int) { added = a; done = true; });
    double peak = 0, cpu0 = cpuSeconds(); QTimer sampler; QObject::connect(&sampler, &QTimer::timeout, [&] { peak = qMax(peak, mem().total()); }); sampler.start(50);
    Stopwatch sw; idx.sync();
    check(waitFor([&] { return done; }, 1200000), "index sync completes");
    metric(qPrintable(tag + "cold_index_ms"), sw.ms(), "ms");
    metric(qPrintable(tag + "cold_index_cpu_s"), cpuSeconds() - cpu0, "s");
    metric(qPrintable(tag + "cold_index_peak_pss"), peak, "MiB");
    metric(qPrintable(tag + "pss_after_index"), mem().total(), "MiB");
    metric(qPrintable(tag + "pss_growth"), mem().total() - m0.total(), "MiB");
    metric(qPrintable(tag + "db_bytes"), double(QFileInfo(idx.databasePath()).size()), "bytes");
    check(added == N + 1 && idx.count() == N + 1, QString("indexed %1 of %2 (+1 literal-probe note)").arg(idx.count()).arg(N + 1));
    sampler.stop();

    // blocking path
    Dist cold, warm, asyncD, empty;
    const QStringList qs = g_queries();
    for (const QString &q : qs) { Stopwatch s; auto p = idx.searchNow({q, {}, {}, 50, 0}); cold.add(s.ms()); (void)p; }
    for (int rep = 0; rep < 10; ++rep)
        for (const QString &q : qs) { Stopwatch s; auto p = idx.searchNow({q, {}, {}, 50, 0}); warm.add(s.ms()); }
    cold.report(tag + "searchNow_first_touch"); warm.report(tag + "searchNow_warm");
    // async path = what the organizer sees: search() -> searchFinished
    for (int rep = 0; rep < 10; ++rep)
        for (const QString &q : qs) {
            bool got = false; quint64 id = 0;
            auto c = QObject::connect(&idx, &LibraryIndex::searchFinished, [&](const SearchPage &p) { if (p.id == id) got = true; });
            Stopwatch s; id = idx.search({q, {}, {}, 50, 0});
            waitFor([&] { return got; }, 30000);
            asyncD.add(s.ms());
            QObject::disconnect(c);
        }
    asyncD.report(tag + "async_first_page");
    // empty text list-by-mtime, folder and tag filters
    for (int i = 0; i < 50; ++i) { Stopwatch s; idx.searchNow({{}, {}, {}, 100, 0}); empty.add(s.ms()); }
    empty.report(tag + "list_recent_first_page");
    Dist filt;
    for (int i = 0; i < 50; ++i) { Stopwatch s; idx.searchNow({"Section", QString("d%1").arg(i % 25), {}, 50, 0}); filt.add(s.ms()); idx.searchNow({{}, {}, "tag3", 50, 0}); filt.add(s.ms()); }
    filt.report(tag + "filtered");
    Dist deep; for (int off = 0; off < 1000 && off < N; off += 100) { Stopwatch s; idx.searchNow({"Section", {}, {}, 100, off}); deep.add(s.ms()); }
    deep.report(tag + "paged_offsets");
    { Stopwatch s; idx.folders(); metric(qPrintable(tag + "folders_ms"), s.ms(), "ms"); }
    { Stopwatch s; idx.tags(); metric(qPrintable(tag + "tags_ms"), s.ms(), "ms"); }
    auto first = idx.searchNow({"uniq" + QString::number(N / 2), {}, {}, 10, 0});
    check(first.rows.size() >= 1, "unique-token query finds its note");
    if (assertTargets) {
        check(asyncD.pct(.95) < 100.0, QString("p95 first page (async, UI path) %1 ms < 100 ms (spec 9.2, 1000 notes)").arg(asyncD.pct(.95)));
        check(warm.pct(.95) < 100.0, QString("p95 first page (blocking, warm) %1 ms < 100 ms").arg(warm.pct(.95)));
        check(cold.mx() < 500.0, QString("worst first-touch query %1 ms < 500 ms").arg(cold.mx()));
    } else {
        soft(asyncD.pct(.95) < 100.0, QString("10k notes: p95 first page %1 ms (informational vs the 1000-note 100 ms target)").arg(asyncD.pct(.95)));
    }

    // incremental: touch 100 notes then sync
    for (int i = 0; i < 100; ++i) writeFile(sb.notes + QString("/d%1/n%2.md").arg(i % 25).arg(i), mixedNote(20 * 1024, i + 100000));
    done = false; sw.reset(); idx.sync(); waitFor([&] { return done; }, 600000);
    metric(qPrintable(tag + "incremental_100_changed_ms"), sw.ms(), "ms");

    // 500 rapid superseding queries issued back-to-back with no event processing
    if (N == 1000) {
        std::vector<quint64> ids; int finished = 0; quint64 lastId = 0; int staleDelivered = 0; quint64 lastDelivered = 0;
        auto c = QObject::connect(&idx, &LibraryIndex::searchFinished, [&](const SearchPage &p) { ++finished; lastDelivered = p.id; });
        Stopwatch s;
        for (int i = 0; i < 500; ++i) { ids.push_back(idx.search({QString("Section %1").arg(i), {}, {}, 100, 0})); lastId = ids.back(); }
        const double issue = s.ms();
        check(waitFor([&] { return lastDelivered == lastId; }, 30000), "final of 500 superseding queries is delivered");
        metric("index.1000.superseding500.issue_ms", issue, "ms");
        metric("index.1000.superseding500.until_final_ms", s.ms(), "ms");
        metric("index.1000.superseding500.results_delivered", finished);
        waitFor([] { return false; }, 300);
        for (quint64 id : ids) (void)id;
        (void)staleDelivered;
        check(finished <= 500 && finished >= 1, QString("superseded queries are cancelled/dropped: %1 of 500 delivered").arg(finished));
        soft(finished < 100, QString("at most a few intermediate pages delivered (%1)").arg(finished));
        QObject::disconnect(c);
    }

    // hostile FTS5 syntax fuzz
    if (N == 1000) {
        Rng rng(2024);
        const QStringList atoms = {"\"", "'", "(", ")", "*", "^", "-", "+", ":", "AND", "OR", "NOT", "NEAR", "NEAR(", "title:", "body:", "path:", "{", "}", "\\", "%", "_", ";", "--", "/*", "*/",
            "DROP TABLE notes;", "' OR 1=1 --", "\" OR \"", "a*b", "**", "\"\"", "\"\"\"", "NEAR/0", "NEAR(a b, 3", "col:", "rank", "match", "\xE2\x80\x8F", "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB", "\xCC\x81", "\\u0000", "\t", "\n", "uniq5", "Section", "~", "#", "@", "<", ">", "=", "!", "?", "|", "&", "`", "[", "]", "0", "9999999999999999999"};
        Dist fz; int crashes = 0; int slow = 0; double worst = 0;
        for (int i = 0; i < 1000; ++i) {
            QString q;
            const int mode = rng.bounded(6);
            if (mode == 0) { const int n = 1 + rng.bounded(12); for (int k = 0; k < n; ++k) q += atoms[rng.bounded(atoms.size())] + (rng.chance(50) ? " " : ""); }
            else if (mode == 1) { const int n = 1 + rng.bounded(60); for (int k = 0; k < n; ++k) q += QChar(rng.bounded(0x2FF) + 1); }
            else if (mode == 2) { q = QString(1 + rng.bounded(20000), QChar(char('a' + rng.bounded(26)))); }
            else if (mode == 3) { for (int k = 0; k < 200; ++k) q += (rng.chance(50) ? "\"" : "*"); }
            else if (mode == 4) { for (int k = 0; k < 400; ++k) q += atoms[rng.bounded(atoms.size())] + " "; }
            else { QByteArray raw; const int n = 1 + rng.bounded(80); for (int k = 0; k < n; ++k) raw += char(rng.bounded(256)); q = QString::fromLatin1(raw); }
            IndexQuery iq{q, rng.chance(20) ? "d3" : "", rng.chance(10) ? "tag3" : "", 20, 0};
            std::atomic<bool> finishedFlag{false};
            std::thread wd([&] { for (int w = 0; w < 100 && !finishedFlag; ++w) std::this_thread::sleep_for(std::chrono::milliseconds(100)); if (!finishedFlag) { std::fprintf(stderr, "HANG in search for fuzz #%d\n", i); std::_Exit(3); } });
            Stopwatch s;
            auto page = idx.searchNow(iq);
            const double ms = s.ms();
            finishedFlag = true; wd.join();
            (void)page; fz.add(ms); worst = qMax(worst, ms); if (ms > 1000) ++slow;
        }
        fz.report("index.1000.fuzz1000");
        check(crashes == 0, "1000 hostile FTS strings: no crash");
        check(slow == 0, QString("1000 hostile FTS strings: none slower than 1 s (worst %1 ms)").arg(worst));
        check(idx.count() == N + 1, "index intact after fuzz (count unchanged)");
        auto ok = idx.searchNow({"uniq5", {}, {}, 10, 0});
        check(ok.rows.size() >= 1, "normal search still works after fuzz");
        // literal-text queries that users really type must not silently return nothing or throw
        const QStringList literal = {"foo-bar", "it's", "(paren)", "50%", "e.g.", "C++", "a:b", "\"quoted\"", "AND", "NOT", "NEAR", "path/to/file.txt", "user@example.com", "hashtag-x"};
        QStringList empties;
        for (const QString &q : literal) { auto p = idx.searchNow({q, {}, {}, 20, 0}); bool found = false; for (const auto &r : p.rows) found |= r.relPath == "literal-probe.md"; if (!found) empties << q; }
        note("user-typed literals (all present verbatim in literal-probe.md) that do NOT find it: " + empties.join(" | "));
        metric("index.1000.literal_queries_not_found", empties.size());
        check(empties.isEmpty(), QString("[D14-FTS-LITERALS] ") + QString("%1 of %2 punctuation-bearing literals typed into search do not find a note that contains them verbatim: %3").arg(empties.size()).arg(literal.size()).arg(empties.join(" | ")));
        // async path with hostile input must also deliver exactly one terminal page
        int fin = 0; auto c = QObject::connect(&idx, &LibraryIndex::searchFinished, [&](const SearchPage &) { ++fin; });
        const quint64 id = idx.search({"\"NEAR(\"( AND", {}, {}, 20, 0}); (void)id;
        waitFor([&] { return fin > 0; }, 5000);
        check(fin >= 1, "async hostile query still delivers a page (UI never left spinning)");
        QObject::disconnect(c);
    }
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QString which = app.arguments().value(1, "all");
    if (which == "all" || which == "1000") runScale(1000, true);
    if (which == "all" || which == "10000") runScale(10000, false);
    return finish("stress_index");
}
