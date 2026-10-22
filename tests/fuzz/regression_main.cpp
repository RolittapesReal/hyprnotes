#include "support.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>
#include <array>
#include <cstdio>

extern "C" int hn_fuzz_markdown_roundtrip(const uint8_t *, size_t);
extern "C" int hn_fuzz_frontmatter(const uint8_t *, size_t);
extern "C" int hn_fuzz_links(const uint8_t *, size_t);
extern "C" int hn_fuzz_query_spec(const uint8_t *, size_t);
extern "C" int hn_fuzz_fts_query(const uint8_t *, size_t);
extern "C" int hn_fuzz_manifest(const uint8_t *, size_t);
extern "C" int hn_fuzz_plugin_archive(const uint8_t *, size_t);
extern "C" int hn_fuzz_theme_import(const uint8_t *, size_t);
extern "C" int hn_fuzz_config(const uint8_t *, size_t);
extern "C" int hn_fuzz_ipc_transport(const uint8_t *, size_t);
extern "C" int hn_fuzz_hypr_events(const uint8_t *, size_t);
extern "C" int hn_fuzz_session(const uint8_t *, size_t);
extern "C" int hn_fuzz_market_index(const uint8_t *, size_t);

struct Target { const char *name; HnFuzzFunction run; };
static constexpr std::array targets{
  Target{"markdown_roundtrip", hn_fuzz_markdown_roundtrip}, Target{"frontmatter", hn_fuzz_frontmatter},
  Target{"links", hn_fuzz_links}, Target{"query_spec", hn_fuzz_query_spec},
  Target{"fts_query", hn_fuzz_fts_query}, Target{"manifest", hn_fuzz_manifest},
  Target{"plugin_archive", hn_fuzz_plugin_archive}, Target{"theme_import", hn_fuzz_theme_import},
  Target{"config", hn_fuzz_config}, Target{"ipc_transport", hn_fuzz_ipc_transport},
  Target{"hypr_events", hn_fuzz_hypr_events}, Target{"session", hn_fuzz_session},
  Target{"market_index", hn_fuzz_market_index}};

int main(int argc, char **argv) {
  QString root = QStringLiteral(HN_FUZZ_SOURCE_DIR) + QStringLiteral("/tests/fuzz/regressions");
  if (argc == 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--root")) {
    root = QString::fromLocal8Bit(argv[2]);
  } else if (argc != 1) {
    std::fprintf(stderr, "usage: fuzz_regressions [--root FIXTURE_ROOT]\n");
    return 2;
  }
  QTemporaryDir scratch;
  if (!scratch.isValid()) {
    std::fprintf(stderr, "fuzz_regressions: cannot create scratch directory\n");
    return 1;
  }
  const QByteArray base = scratch.path().toUtf8();
  qputenv("HOME", base);
  qputenv("XDG_CONFIG_HOME", base + "/config");
  qputenv("XDG_DATA_HOME", base + "/data");
  qputenv("XDG_STATE_HOME", base + "/state");
  qputenv("XDG_CACHE_HOME", base + "/cache");
  qputenv("HN_CONFIG_DIR", base + "/config/hyprnotes");
  qputenv("HN_DATA_DIR", base + "/data/hyprnotes");
  qputenv("HN_STATE_DIR", base + "/state/hyprnotes");
  qputenv("HN_CACHE_DIR", base + "/cache/hyprnotes");
  qputenv("HN_NOTES_DIR", base + "/data/hyprnotes/notes");
  qputenv("QT_FORCE_STDERR_LOGGING", "1");
  int appArgc = 1;
  QApplication app(appArgc, argv);
  int failures = 0, replayed = 0;
  if (!QDir(root).exists()) {
    std::fprintf(stderr, "fuzz_regressions: regression root missing: %s\n", qPrintable(root));
    return 1;
  }
  for (const auto &target : targets) {
    QDir dir(root + QLatin1Char('/') + QLatin1String(target.name));
    if (!dir.exists()) continue;
    const auto entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                                           QDir::Name);
    for (const auto &entry : entries) {
      if (entry.isSymLink() || !entry.isFile()) {
        std::fprintf(stderr, "fuzz_regressions: regression is not a regular file: %s\n",
                     qPrintable(entry.filePath()));
        ++failures;
        continue;
      }
      QFile file(entry.filePath());
      if (!file.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "fuzz_regressions: cannot read regression %s\n", qPrintable(entry.filePath()));
        ++failures;
        continue;
      }
      const QByteArray bytes = file.readAll();
      if (file.error() != QFileDevice::NoError || bytes.isEmpty()) {
        std::fprintf(stderr, "fuzz_regressions: empty or unreadable regression %s\n", qPrintable(entry.filePath()));
        ++failures;
        continue;
      }
      QByteArray input = bytes;
      if (entry.fileName().endsWith(QLatin1String(".base64"))) {
        input = QByteArray::fromBase64(bytes.trimmed(), QByteArray::AbortOnBase64DecodingErrors);
        if (input.isEmpty()) {
          std::fprintf(stderr, "fuzz_regressions: invalid or empty base64 regression %s\n",
                       qPrintable(entry.filePath()));
          ++failures;
          continue;
        }
      }
      ++replayed;
      target.run(reinterpret_cast<const uint8_t *>(input.constData()), size_t(input.size()));
    }
  }
  std::fprintf(stderr, "fuzz_regressions: replayed %d files, %d failures\n", replayed, failures);
  return failures || replayed == 0 ? 1 : 0;
}
