#include "support.h"
#ifdef HN_FUZZ_GUI
#include <QApplication>
using HnApplication = QApplication;
#else
#include <QCoreApplication>
using HnApplication = QCoreApplication;
#endif
#include <QByteArray>
#include <QDir>
#include <QEvent>
#include <QTemporaryDir>
#include <memory>

namespace {
std::unique_ptr<QTemporaryDir> temp;
int argc = 1;
char appName[] = "hyprnotes-fuzz";
char *argv[] = {appName, nullptr};
HnApplication *app = nullptr;
#ifdef HN_FUZZ_LIFECYCLE_DIAGNOSTIC
HnFuzzLifecycleSnapshot lifecycle;
#endif
}

void hnFuzzShutdown() {
  if (app && QCoreApplication::instance() == app)
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  if (app) {
    delete app;
    app = nullptr;
#ifdef HN_FUZZ_LIFECYCLE_DIAGNOSTIC
    ++lifecycle.appDestroyed;
#endif
  }
  if (temp) {
    temp.reset();
#ifdef HN_FUZZ_LIFECYCLE_DIAGNOSTIC
    ++lifecycle.tempDestroyed;
#endif
  }
}

#ifdef HN_FUZZ_LIFECYCLE_DIAGNOSTIC
HnFuzzLifecycleSnapshot hnFuzzLifecycleSnapshot() { return lifecycle; }
#endif

void hnFuzzInit() {
  if (!temp) {
    temp = std::make_unique<QTemporaryDir>();
#ifdef HN_FUZZ_LIFECYCLE_DIAGNOSTIC
    ++lifecycle.tempCreated;
    lifecycle.lastTempPath = temp->path();
#endif
  }
#ifdef HN_FUZZ_LIFECYCLE_DIAGNOSTIC
  ++lifecycle.initCalls;
#endif
  if (!app && !QCoreApplication::instance()) {
    const QByteArray base = temp->path().toUtf8();
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
    app = new HnApplication(argc, argv);
#ifdef HN_FUZZ_LIFECYCLE_DIAGNOSTIC
    ++lifecycle.appCreated;
#endif
  }
}

QString hnFuzzTempPath() {
  hnFuzzInit();
  return temp->path();
}
