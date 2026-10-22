#include "support.h"
#include "md_io.h"
#include "hn/editor/note_editor.h"
#include <QTextDocument>
#include <cstdlib>
#include <cstdio>

namespace {
struct FuzzInputScope {
  ~FuzzInputScope() { hnFuzzShutdown(); }
};
}

extern "C" int hn_fuzz_markdown_roundtrip(const uint8_t *data, size_t size) {
  FuzzInputScope scope;
  hnFuzzInit();
  const QByteArray raw(reinterpret_cast<const char *>(data), qsizetype(size));
  const QString input = QString::fromUtf8(raw);
  QTextDocument first, second;
  hn::editor::importMarkdown(&first, input);
  const QString once = hn::editor::exportMarkdown(&first);
  hn::editor::importMarkdown(&second, once);
  const QString twice = hn::editor::exportMarkdown(&second);
  if (twice != once) {
    std::fprintf(stderr, "markdown roundtrip mismatch; input=%s; once=%s; twice=%s\n",
                 qPrintable(input.toUtf8().toHex()), qPrintable(once.toUtf8().toHex()),
                 qPrintable(twice.toUtf8().toHex()));
    std::fflush(stderr);
    std::abort();
  }
  // Source-mode byte preservation is independent of the low-level serializer property above.
  hn::editor::NoteEditor admission;
  admission.load(raw);
  if (admission.mode() != hn::editor::Mode::Visual && admission.toMarkdownBytes() != raw) {
    std::fprintf(stderr, "Source-mode byte preservation mismatch; input=%s; output=%s\n",
                 qPrintable(raw.toHex()), qPrintable(admission.toMarkdownBytes().toHex()));
    std::fflush(stderr);
    std::abort();
  }
  return 0;
}
#ifndef HN_FUZZ_REGRESSION_BUILD
extern "C" int LLVMFuzzerInitialize(int *, char ***) { return 0; }
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  return hn_fuzz_markdown_roundtrip(data, size);
}
#endif
