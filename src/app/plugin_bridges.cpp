#include "plugin_bridges.h"
#include "hn/core/markdown_codec.h"
#include "controller.h"
#include "plugin_service.h"
#include "ui_common.h"
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QPushButton>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>

using namespace hn::plugins;
using hn::editor::Mode;

namespace hn::app {

// ================================================================ note
QTextCursor PluginNoteBridge::cursor() const {
    return ed()->mode() == Mode::Visual ? ed()->visualEdit()->textCursor() : ed()->sourceEdit()->textCursor();
}
void PluginNoteBridge::setCursor(const QTextCursor &c) {
    if (ed()->mode() == Mode::Visual) ed()->visualEdit()->setTextCursor(c); else ed()->sourceEdit()->setTextCursor(c);
}

QString PluginNoteBridge::text() {
    if (!m_s) return {};
    QByteArray b = ed()->toMarkdownBytes();
    if (b.startsWith("\xEF\xBB\xBF")) b.remove(0, 3);
    QString t = QString::fromUtf8(b);
    t.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    t.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return t;
}

QString PluginNoteBridge::selection() {
    if (!m_s) return {};
    QString t = cursor().selectedText();
    t.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    t.replace(QChar::LineSeparator, QLatin1Char('\n'));
    return t;
}

void PluginNoteBridge::beginTransaction(const QString &) {
    endTransaction();
    if (!m_s || ed()->isReadOnly()) return;
    m_scope = ed()->recorder().begin(hn::editor::TxKind::Format, hn::editor::WindowMode::WholeDoc);   // ponytail: snapshots all blocks; fine up to the 256 KiB visual limit
}

void PluginNoteBridge::replaceSelection(const QString &s) {
    if (!m_s || ed()->isReadOnly()) return;
    QTextCursor c = cursor();
    c.insertText(s);
    setCursor(c);
}

void PluginNoteBridge::insert(const QString &s) {
    if (!m_s || ed()->isReadOnly()) return;
    QTextCursor c = cursor();
    c.clearSelection();
    c.insertText(s);
    setCursor(c);
}

void PluginNoteBridge::setText(const QString &s) {
    if (!m_s || ed()->isReadOnly()) return;
    ed()->insertMarkdown(s, true);
}

// ================================================================ library
bool PluginLibraryBridge::validRel(const QString &rel) const {
    if (rel.isEmpty() || rel.startsWith(QLatin1Char('/')) || rel.contains(QStringLiteral("..")) || !rel.endsWith(QStringLiteral(".md"), Qt::CaseInsensitive)) return false;
    return !m_c->repo().absolutePath(rel).isEmpty();
}

QList<NoteInfo> PluginLibraryBridge::list(const QString &query) {
    QList<NoteInfo> out;
    if (m_c->hasIndex()) {
        hn::core::IndexQuery q;
        q.text = query;
        q.limit = kMaxList;
        for (const auto &r : m_c->index()->searchNow(q).rows) out.append({r.relPath, r.title});
        return out;
    }
    for (const auto &e : m_c->repo().list()) {   // index not built yet: file-name match on the disk listing
        const QString base = QFileInfo(e.relPath).completeBaseName();
        if (!query.isEmpty() && !base.contains(query, Qt::CaseInsensitive)) continue;
        out.append({e.relPath, base});
        if (out.size() >= kMaxList) break;
    }
    return out;
}

bool PluginLibraryBridge::read(const QString &path, QString *text) {
    if (!validRel(path) || !text) return false;
    if (NoteSession *s = m_c->session(path)) { PluginNoteBridge b(s); *text = b.text(); return true; }   // unsaved edits are the truth
    const auto snap = m_c->repo().read(path);
    if (!snap.ok || snap.bytes.size() > kMaxText || !hn::core::isValidUtf8(snap.bytes)) return false;
    *text = QString::fromUtf8(snap.bytes);
    return true;
}

QString PluginLibraryBridge::create(const QString &title, const QString &text) {
    if (title.trimmed().isEmpty() || text.toUtf8().size() > kMaxText) return {};
    QString err;
    const QString rel = m_c->repo().create(QString(), title.trimmed(), text.toUtf8(), &err);
    if (rel.isEmpty()) return {};
    if (m_c->hasIndex()) m_c->index()->updatePath(rel);
    emit m_c->notesChanged();
    return rel;
}

bool PluginLibraryBridge::write(const QString &path, const QString &text) {
    if (!validRel(path) || text.toUtf8().size() > kMaxText) return false;
    if (NoteSession *s = m_c->session(path)) {   // through the open editor: one undo step, normal autosave, never a conflict
        PluginNoteBridge b(s);
        b.beginTransaction(QStringLiteral("plugin write"));
        b.setText(text);
        b.endTransaction();
        return true;
    }
    auto &repo = m_c->repo();
    if (!QFileInfo::exists(repo.absolutePath(path))) return false;   // create() is the way to make notes
    const auto snap = repo.read(path);   // sets the baseline: an external change since now is detected as a conflict
    if (!snap.ok) return false;
    const quint64 rev = repo.save(path, text.toUtf8(), snap.revision);
    bool done = false, ok = false;
    QEventLoop loop;
    QTimer cap;
    cap.setSingleShot(true);
    auto finish = [&](bool good) { done = true; ok = good; loop.quit(); };
    const auto c1 = QObject::connect(&repo, &hn::core::NoteRepository::saved, &loop, [&](const QString &r, quint64 v) { if (r == path && v == rev) finish(true); });
    const auto c2 = QObject::connect(&repo, &hn::core::NoteRepository::saveFailed, &loop, [&](const QString &r, const QString &, quint64 v) { if (r == path && v == rev) finish(false); });
    const auto c3 = QObject::connect(&repo, &hn::core::NoteRepository::conflict, &loop, [&](const QString &r, const QByteArray &, const QByteArray &, quint64 v) { if (r == path && v == rev) finish(false); });
    QObject::connect(&cap, &QTimer::timeout, &loop, &QEventLoop::quit);
    cap.start(5000);
    if (!done) loop.exec(QEventLoop::ExcludeUserInputEvents);
    QObject::disconnect(c1); QObject::disconnect(c2); QObject::disconnect(c3);
    if (ok) { if (m_c->hasIndex()) m_c->index()->updatePath(path); emit m_c->notesChanged(); }
    return ok;
}

bool PluginLibraryBridge::remove(const QString &path) {
    if (!validRel(path)) return false;
    return m_c->deleteNote(path, nullptr);
}

// ================================================================ ui
namespace {
// Flat modernist plugin dialog: accent bar + "PLUGIN - NAME" kicker, bold title, body, Cancel / OK. Square, themed.
class FlatDialog : public QDialog {
public:
    QVBoxLayout *body;
    QPushButton *ok, *cancel;
    FlatDialog(QWidget *parent, const QString &plugin, const QString &title, const QString &okText, bool okDefault) : QDialog(parent) {
        const auto &t = ui::theme();
        setWindowTitle(title);
        setWindowIcon(ui::appIcon());
        setModal(true);
        setMinimumWidth(420);
        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);
        auto *head = new QWidget(this);
        head->setObjectName("hnPlHead");
        head->setStyleSheet(QString("QWidget#hnPlHead { border-left: 6px solid %1; border-bottom: 1px solid %2; }").arg(t.accent.name(), t.border.name()));
        auto *hl = new QVBoxLayout(head);
        hl->setContentsMargins(20, 12, 20, 12);
        hl->setSpacing(2);
        auto *k = new QLabel(tr("PLUGIN  -  %1").arg(plugin.toUpper()), head);
        k->setFont(ui::labelFont(10));
        k->setStyleSheet(QString("color: %1;").arg(t.muted.name()));
        auto *ti = new QLabel(title, head);
        ti->setFont(ui::uiFont(16, QFont::Bold));
        ti->setWordWrap(true);
        hl->addWidget(k);
        hl->addWidget(ti);
        root->addWidget(head);
        auto *bw = new QWidget(this);
        body = new QVBoxLayout(bw);
        body->setContentsMargins(20, 16, 20, 8);
        body->setSpacing(8);
        root->addWidget(bw, 1);
        auto *bar = new QWidget(this);
        auto *bh = new QHBoxLayout(bar);
        bh->setContentsMargins(20, 8, 20, 16);
        bh->addStretch(1);
        cancel = new QPushButton(tr("Cancel"), bar);
        ok = new QPushButton(okText, bar);
        cancel->setMinimumHeight(36);
        ok->setMinimumHeight(36);
        cancel->setAutoDefault(!okDefault);
        cancel->setDefault(!okDefault);
        ok->setAutoDefault(okDefault);
        ok->setDefault(okDefault);
        if (okDefault) ok->setStyleSheet(ui::accentButtonStyle() + "QPushButton { text-align: center; min-height: 36px; }");
        bh->addWidget(cancel);
        bh->addWidget(ok);
        root->addWidget(bar);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        connect(ok, &QPushButton::clicked, this, &QDialog::accept);
    }
};
}  // namespace

void PluginUiBridge::notify(const QString &pluginId, const QString &msg) {
    m_s->controller()->announce(QStringLiteral("%1: %2").arg(m_s->nameOf(pluginId), msg));
}

std::optional<QString> PluginUiBridge::prompt(const QString &pluginId, const QString &title, const QString &label, const QString &def) {
    if (m_s->hooks().prompt) return m_s->hooks().prompt(m_s->nameOf(pluginId), title, label, def);
    FlatDialog d(QApplication::activeWindow(), m_s->nameOf(pluginId), title, QObject::tr("OK"), true);
    auto *l = new QLabel(label, &d);
    l->setWordWrap(true);
    auto *e = new QLineEdit(def, &d);
    e->setMinimumHeight(32);
    d.body->addWidget(l);
    d.body->addWidget(e);
    e->setFocus();
    e->selectAll();
    if (d.exec() != QDialog::Accepted) return std::nullopt;
    return e->text();
}

bool PluginUiBridge::confirm(const QString &pluginId, const QString &msg) {
    if (m_s->hooks().confirm) return m_s->hooks().confirm(m_s->nameOf(pluginId), msg);
    FlatDialog d(QApplication::activeWindow(), m_s->nameOf(pluginId), QObject::tr("Confirm"), QObject::tr("Continue"), false);
    auto *l = new QLabel(msg, &d);
    l->setWordWrap(true);
    d.body->addWidget(l);
    return d.exec() == QDialog::Accepted;
}

int PluginUiBridge::pick(const QString &pluginId, const QString &title, const QStringList &items) {
    if (m_s->hooks().pick) return m_s->hooks().pick(m_s->nameOf(pluginId), title, items);
    FlatDialog d(QApplication::activeWindow(), m_s->nameOf(pluginId), title, QObject::tr("Select"), true);
    auto *list = new QListWidget(&d);
    list->addItems(items);
    if (!items.isEmpty()) list->setCurrentRow(0);
    list->setMinimumHeight(qMin(240, 28 * int(items.size()) + 8));
    d.body->addWidget(list);
    QObject::connect(list, &QListWidget::itemActivated, &d, &QDialog::accept);
    list->setFocus();
    if (d.exec() != QDialog::Accepted) return -1;
    return list->currentRow();
}

// ================================================================ clipboard / theme
QString PluginClipboardBridge::get() { return QGuiApplication::clipboard()->text(); }
void PluginClipboardBridge::set(const QString &s) { QGuiApplication::clipboard()->setText(s); }

bool PluginThemeBridge::setToken(const QString &pluginId, const QString &token, const QString &value) {
    return m_c->setThemeOverride(pluginId, token, value);
}

// ================================================================ network
namespace {
class NoCookies : public QNetworkCookieJar {   // never stores or sends a cookie
public:
    using QNetworkCookieJar::QNetworkCookieJar;
    QList<QNetworkCookie> cookiesForUrl(const QUrl &) const override { return {}; }
    bool setCookiesFromUrl(const QList<QNetworkCookie> &, const QUrl &) override { return false; }
};
}  // namespace

PluginNetBridge::PluginNetBridge() : m_validate(&PluginNetBridge::defaultValidator) {}
PluginNetBridge::~PluginNetBridge() { delete m_nam; }

bool PluginNetBridge::defaultValidator(const QUrl &url, const QStringList &allowedHosts, QString *why) {
    QString host;
    return checkHttpUrl(url.toString(QUrl::FullyEncoded), allowedHosts, &host, why);
}

QNetworkAccessManager *PluginNetBridge::nam() {
    if (!m_nam) {
        m_nam = new QNetworkAccessManager;
        m_nam->setCookieJar(new NoCookies);
        m_nam->setCache(nullptr);
        ++s_created;
    }
    return m_nam;
}

HttpResponse PluginNetBridge::perform(const HttpRequest &req) {
    HttpResponse res;
    QUrl url(req.url, QUrl::StrictMode);
    QString method = req.method.toUpper();
    QByteArray body = req.body;
    QElapsedTimer total;
    total.start();
    for (int hop = 0; hop <= kMaxRedirects; ++hop) {
        QString why;
        if (!url.isValid() || !m_validate(url, req.allowedHosts, &why)) {
            res.error = hop == 0 ? (why.isEmpty() ? QStringLiteral("invalid URL") : why)
                                 : QStringLiteral("redirect refused (%1)").arg(why.isEmpty() ? QStringLiteral("invalid target") : why);
            res.status = 0;
            res.body.clear();
            return res;
        }
        const qint64 left = qint64(req.timeoutMs) - total.elapsed();
        if (left <= 0) { res.error = QStringLiteral("timed out"); return res; }
        QNetworkRequest nr(url);
        nr.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);   // every hop is ours to check
        nr.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
        nr.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
        nr.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        nr.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        nr.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
        nr.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Hyprnotes-plugin/") + QString::fromLatin1(kAppVersion));
        for (const auto &h : req.headers) nr.setRawHeader(h.first.toLatin1(), h.second.toLatin1());
        QNetworkReply *rep = method == QLatin1String("GET") ? nam()->get(nr)
                           : method == QLatin1String("POST") ? nam()->post(nr, body)
                           : nam()->sendCustomRequest(nr, method.toLatin1(), body);
        QByteArray data;
        bool tooBig = false, timedOut = false;
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        auto drain = [&] {
            if (rep->isOpen()) data += rep->readAll();
            if (data.size() > req.maxResponseBytes && !tooBig) { tooBig = true; rep->abort(); }
        };
        QObject::connect(rep, &QNetworkReply::readyRead, &loop, drain);
        QObject::connect(rep, &QNetworkReply::downloadProgress, &loop, [&](qint64 got, qint64 tot) {
            if ((got > req.maxResponseBytes || tot > req.maxResponseBytes) && !tooBig) { tooBig = true; rep->abort(); }
        });
        QObject::connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(&timer, &QTimer::timeout, &loop, [&] { timedOut = true; rep->abort(); });
        timer.start(int(left));
        loop.exec(QEventLoop::ExcludeUserInputEvents);
        timer.stop();
        drain();
        const int status = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QVariant loc = rep->header(QNetworkRequest::LocationHeader);
        const QString errStr = rep->errorString();
        const bool failed = rep->error() != QNetworkReply::NoError && status == 0;
        rep->deleteLater();
        if (tooBig) { res.error = QStringLiteral("response larger than %1 bytes").arg(req.maxResponseBytes); return res; }
        if (timedOut) { res.error = QStringLiteral("timed out"); return res; }
        if (failed) { res.error = errStr; return res; }
        const bool redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
        if (redirect && loc.isValid() && !loc.toString().isEmpty()) {
            url = url.resolved(QUrl(loc.toString(), QUrl::StrictMode));   // validated at the top of the next hop
            if (status == 303 || ((status == 301 || status == 302) && method == QLatin1String("POST"))) { method = QStringLiteral("GET"); body.clear(); }
            if (hop == kMaxRedirects) { res.error = QStringLiteral("too many redirects"); return res; }
            continue;
        }
        res.status = status;
        res.body = data;
        return res;
    }
    res.error = QStringLiteral("too many redirects");
    return res;
}

} // namespace hn::app
