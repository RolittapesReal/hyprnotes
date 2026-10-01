#include "hn/editor/history.h"

#include "md_io.h"

#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QTextBlock>
#include <QTextList>

namespace hn::editor {

qint64 regionBytes(const Region &r)
{
    // measured sizeof-based estimate: BlockSnap ~96 B (3 shared format handles, list id, vector header), Frag ~56 B + UTF-16 text
    qint64 n = 64;
    for (const auto &b : r.blocks) {
        n += 96;
        for (const auto &f : b.frags) n += qint64(f.text.size()) * 2 + 56;
    }
    return n;
}

qint64 txBytes(const Tx &t)
{
    return regionBytes(t.before) + regionBytes(t.after) + qint64(t.textBefore.size() + t.textAfter.size()) * 2 + 256;
}

Region captureRegion(QTextDocument *doc, int startBlock, int count, int epoch)
{
    Region r;
    r.epoch = epoch;
    startBlock = qBound(0, startBlock, doc->blockCount() - 1);
    count = qBound(0, count, doc->blockCount() - startBlock);
    r.start = startBlock;
    QTextBlock b = doc->findBlockByNumber(startBlock);
    for (int i = 0; i < count && b.isValid(); ++i, b = b.next()) {
        BlockSnap s;
        s.bf = b.blockFormat();
        s.bf.clearProperty(QTextFormat::ObjectIndex);
        s.cf = b.charFormat();
        if (QTextList *l = b.textList()) {
            s.listId = l->objectIndex();
            s.lf = l->format();
            s.lf.clearProperty(QTextFormat::ObjectIndex);
        }
        for (auto it = b.begin(); !it.atEnd(); ++it)
            s.frags.append({it.fragment().text(), it.fragment().charFormat()});
        r.blocks.append(std::move(s));
    }
    return r;
}

bool regionEqual(const Region &a, const Region &b)
{
    if (a.start != b.start || a.blocks.size() != b.blocks.size()) return false;
    for (int i = 0; i < a.blocks.size(); ++i) {
        const auto &x = a.blocks[i];
        const auto &y = b.blocks[i];
        if (x.listId != y.listId || x.bf != y.bf || x.cf != y.cf || x.frags.size() != y.frags.size()) return false;
        if (x.listId >= 0 && x.lf != y.lf) return false;
        for (int j = 0; j < x.frags.size(); ++j)
            if (x.frags[j].text != y.frags[j].text || x.frags[j].fmt != y.frags[j].fmt) return false;
    }
    return true;
}

// ---- spill (payload on disk) ----
static void writeRegion(QDataStream &s, const Region &r)
{
    s << r.start << r.epoch << qint32(r.blocks.size());
    for (const auto &b : r.blocks) {
        s << QTextFormat(b.bf) << QTextFormat(b.cf) << qint32(b.listId) << QTextFormat(b.lf) << qint32(b.frags.size());
        for (const auto &f : b.frags) s << f.text << QTextFormat(f.fmt);
    }
}
static Region readRegion(QDataStream &s)
{
    Region r;
    qint32 n = 0;
    s >> r.start >> r.epoch >> n;
    for (int i = 0; i < n; ++i) {
        BlockSnap b;
        QTextFormat f1, f2, f3, f4;
        qint32 lid = -1, nf = 0;
        s >> f1 >> f2 >> lid >> f3 >> nf;
        b.bf = f1.toBlockFormat();
        b.cf = f2.toCharFormat();
        b.listId = lid;
        b.lf = f3.toListFormat();
        for (int j = 0; j < nf; ++j) {
            Frag fr;
            s >> fr.text >> f4;
            fr.fmt = f4.toCharFormat();
            b.frags.append(std::move(fr));
        }
        r.blocks.append(std::move(b));
    }
    return r;
}

bool TxHistory::spill(Tx &t)
{
    auto f = std::make_shared<QTemporaryFile>(QDir(m_spillDir.isEmpty() ? QDir::tempPath() : m_spillDir)
                                                  .filePath(QStringLiteral("hn-history-XXXXXX.bin")));
    if (!f->open()) { m_err = f->errorString(); return false; }
    {
        QDataStream s(f.get());
        writeRegion(s, t.before);
        writeRegion(s, t.after);
        s << t.textBefore << t.textAfter;
    }
    if (!f->flush() || f->error() != QFileDevice::NoError) { m_err = f->errorString(); return false; }
    t.spill = f;
    t.before.blocks.clear();
    t.after.blocks.clear();
    t.textBefore.clear();
    t.textAfter.clear();
    t.payload = 512;
    return true;
}

void TxHistory::load(Tx &t)
{
    if (!t.spill) return;
    t.spill->seek(0);
    QDataStream s(t.spill.get());
    t.before = readRegion(s);
    t.after = readRegion(s);
    s >> t.textBefore >> t.textAfter;
}

bool TxHistory::canSpill(QString *err) const
{
    QTemporaryFile f(QDir(m_spillDir.isEmpty() ? QDir::tempPath() : m_spillDir).filePath(QStringLiteral("hn-probe-XXXXXX.bin")));
    if (f.open() && f.write("x") == 1 && f.flush()) return true;
    if (err) *err = f.errorString();
    return false;
}

qint64 TxHistory::now() const { return m_clock ? m_clock() : QDateTime::currentMSecsSinceEpoch(); }

void TxHistory::clear()
{
    m_txs.clear();
    m_pos = 0;
    m_bytes = 0;
    m_noMerge = false;
}

int TxHistory::spilledCount() const
{
    int n = 0;
    for (const auto &t : m_txs) n += t.spill ? 1 : 0;
    return n;
}

TxHistory::PushResult TxHistory::push(Tx tx)
{
    tx.time = now();
    while (int(m_txs.size()) > m_pos) { m_bytes -= m_txs.back().payload; m_txs.pop_back(); }   // discard redo branch
    tx.payload = txBytes(tx);
    const bool noMerge = m_noMerge;
    m_noMerge = false;
    if (!noMerge && !m_txs.empty()) {
        Tx &b = m_txs.back();
        if ((tx.kind == TxKind::Typing || tx.kind == TxKind::Delete) && b.kind == tx.kind && !b.spill
            && tx.time - b.time <= kPauseMs && tx.curBeforeAnchor == tx.curBeforePos && tx.curBeforePos == b.curAfterPos
            && b.curAfterPos == b.curAfterAnchor && tx.before.start == b.after.start
            && b.before.blocks.size() == b.after.blocks.size() && tx.before.blocks.size() == tx.after.blocks.size()
            && tx.before.blocks.size() == b.after.blocks.size()
            && b.payload - regionBytes(b.after) + regionBytes(tx.after) <= kMaxMerged) {
            m_bytes -= b.payload;
            b.after = std::move(tx.after);
            b.curAfterPos = tx.curAfterPos;
            b.curAfterAnchor = tx.curAfterAnchor;
            b.time = tx.time;
            b.payload = txBytes(b);
            m_bytes += b.payload;
            return PushResult::Merged;
        }
    }
    if (tx.payload > kMaxBytes && !spill(tx)) {
        clear();
        return PushResult::SpillFailed;
    }
    m_bytes += tx.payload;
    m_txs.push_back(std::move(tx));
    m_pos = int(m_txs.size());
    while ((int(m_txs.size()) > kMaxTx || m_bytes > kMaxBytes) && m_txs.size() > 1) {
        m_bytes -= m_txs.front().payload;
        m_txs.pop_front();
        --m_pos;
        ++m_evicted;
    }
    return PushResult::Stored;
}

Tx TxHistory::takeForUndo()
{
    Tx t = m_txs[--m_pos];
    load(t);
    return t;
}
Tx TxHistory::takeForRedo()
{
    Tx t = m_txs[m_pos++];
    load(t);
    return t;
}

// ---------------- EditRecorder ----------------

EditRecorder::Scope &EditRecorder::Scope::operator=(Scope &&o) noexcept
{
    if (this == &o) return *this;
    if (r) r->finish(*this);
    r = std::exchange(o.r, nullptr);
    kind = o.kind;
    before = std::move(o.before);
    blocks0 = o.blocks0;
    lastBlock = o.lastBlock;
    pos = o.pos;
    anchor = o.anchor;
    return *this;
}

void EditRecorder::Scope::cancel()
{
    if (!r) return;
    --r->m_depth;
    r = nullptr;
}

void EditRecorder::attach(QTextDocument *doc, CursorGet get, CursorSet set)
{
    if (m_conn) disconnect(m_conn);
    m_doc = doc;
    m_get = std::move(get);
    m_set = std::move(set);
    if (doc) m_conn = connect(doc, &QTextDocument::contentsChange, this, [this] { onContentsChange(); });
}

void EditRecorder::onContentsChange()
{
    if (m_quiet) return;
    ++m_revision;
    m_dirty = true;
    if (m_depth == 0) {   // a route that bypassed the adapter: history can no longer be trusted
        ++m_unrecorded;
        if (m_hist.count()) {
            m_hist.clear();
            emit historyInvalidated(QStringLiteral("unrecorded document change"));
        }
        m_dirty = false;
        emit changed(m_revision);
    }
}

EditRecorder::Scope EditRecorder::begin(TxKind kind, WindowMode wm)
{
    if (m_depth > 0 || !m_doc) return {};
    const QTextCursor c = m_get();
    int first = 0, last = m_doc->blockCount() - 1;
    if (wm != WindowMode::WholeDoc) {
        const int a = c.selectionStart(), b = c.selectionEnd();
        const QTextBlock ba = m_doc->findBlock(a), bb = m_doc->findBlock(b);
        first = ba.blockNumber();
        last = bb.blockNumber();
        if (wm == WindowMode::Wide) { --first; ++last; }
        else if (kind == TxKind::Delete || kind == TxKind::Cut) {
            if (a == ba.position()) --first;
            if (b == bb.position() + bb.length() - 1) ++last;
        }
    }
    return beginBlocks(kind, first, last);
}

EditRecorder::Scope EditRecorder::beginBlocks(TxKind kind, int first, int last)
{
    Scope s;
    if (m_depth > 0 || !m_doc) return s;
    first = qMax(0, first);
    last = qMin(m_doc->blockCount() - 1, qMax(last, first));
    s.r = this;
    s.kind = kind;
    s.blocks0 = m_doc->blockCount();
    s.lastBlock = last;
    s.before = captureRegion(m_doc, first, last - first + 1, m_epoch);
    if (regionBytes(s.before) > TxHistory::kMaxBytes / 2) {
        QString err;
        if (!m_hist.canSpill(&err)) emit recoveryWriteFailed(err);
    }
    const QTextCursor c = m_get();
    s.pos = c.position();
    s.anchor = c.anchor();
    m_dirty = false;
    ++m_depth;
    return s;
}

void EditRecorder::finish(Scope &s)
{
    s.r = nullptr;
    --m_depth;
    const bool dirty = m_dirty;
    m_dirty = false;
    if (!dirty) return;
    const int cnt = (s.lastBlock - s.before.start + 1) + (m_doc->blockCount() - s.blocks0);
    if (cnt < 1) {
        m_hist.clear();
        emit historyInvalidated(QStringLiteral("edit escaped its snapshot window"));
        emit changed(m_revision);
        return;
    }
    Tx t;
    t.kind = s.kind;
    t.after = captureRegion(m_doc, s.before.start, cnt, m_epoch);
    if (regionEqual(s.before, t.after)) { emit changed(m_revision); return; }
    t.before = std::move(s.before);
    t.curBeforePos = s.pos;
    t.curBeforeAnchor = s.anchor;
    const QTextCursor c = m_get();
    t.curAfterPos = c.position();
    t.curAfterAnchor = c.anchor();
    if (m_hist.push(std::move(t)) == TxHistory::PushResult::SpillFailed) {
        emit recoveryWriteFailed(m_hist.lastError());
        emit historyInvalidated(QStringLiteral("recovery write failed"));
    }
    emit changed(m_revision);
}

void EditRecorder::pushModeTx(Tx tx)
{
    tx.curBeforePos = tx.curBeforeAnchor = tx.curAfterPos = tx.curAfterAnchor = 0;
    if (m_hist.push(std::move(tx)) == TxHistory::PushResult::SpillFailed) {
        emit recoveryWriteFailed(m_hist.lastError());
        emit historyInvalidated(QStringLiteral("recovery write failed"));
    }
}

void EditRecorder::applyRegion(int startBlock, int oldCount, const Region &r)
{
    ++m_depth;
    QTextDocument *doc = m_doc;
    startBlock = qBound(0, startBlock, doc->blockCount() - 1);
    oldCount = qBound(1, oldCount, doc->blockCount() - startBlock);
    QTextBlock first = doc->findBlockByNumber(startBlock);
    QTextBlock last = doc->findBlockByNumber(startBlock + oldCount - 1);
    QTextCursor c(doc);
    c.beginEditBlock();
    c.setPosition(first.position());
    c.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);
    if (QTextList *l = first.textList()) l->remove(first);
    const int startPos = first.position();
    c.removeSelectedText();
    c.setPosition(startPos);
    auto fix = [&](QTextBlockFormat bf) { if (m_fixup) m_fixup(bf); return bf; };
    for (int i = 0; i < r.blocks.size(); ++i) {
        const BlockSnap &b = r.blocks[i];
        if (i == 0) { c.setBlockFormat(fix(b.bf)); c.setBlockCharFormat(b.cf); }
        else c.insertBlock(fix(b.bf), b.cf);
        for (const auto &f : b.frags) c.insertText(f.text, f.fmt);
    }
    // re-attach list membership
    QHash<int, QTextList *> pass;
    QTextBlock b = doc->findBlockByNumber(startBlock);
    for (int i = 0; i < r.blocks.size() && b.isValid(); ++i, b = b.next()) {
        const BlockSnap &s = r.blocks[i];
        if (s.listId < 0) continue;
        QTextList *L = pass.value(s.listId, nullptr);
        if (!L && r.epoch == m_epoch)
            if (auto *l = qobject_cast<QTextList *>(doc->object(m_remap.value(s.listId, s.listId))); l && l->count() > 0) L = l;
        if (!L) {   // neighbour heuristic: adopt the previous block's list when style/indent match
            QTextBlock p = b.previous();
            while (p.isValid() && !p.textList() && p.blockFormat().hasProperty(kContProp)) p = p.previous();   // skip item continuation blocks
            if (p.isValid() && p.textList() && p.textList()->format().indent() == s.lf.indent()
                && p.textList()->format().style() == s.lf.style())
                L = p.textList();
        }
        if (L) L->add(b);
        else {
            QTextCursor lc(b);
            L = lc.createList(s.lf);
            if (r.epoch == m_epoch) m_remap[s.listId] = L->objectIndex();
        }
        pass[s.listId] = L;
    }
    c.endEditBlock();
    --m_depth;
}

bool EditRecorder::undoRegion()
{
    if (!m_hist.canUndo()) return false;
    Tx t = m_hist.takeForUndo();
    applyRegion(t.after.start, int(t.after.blocks.size()), t.before);
    m_set(qMin(t.curBeforePos, m_doc->characterCount() - 1), qMin(t.curBeforeAnchor, m_doc->characterCount() - 1));
    emit changed(m_revision);
    return true;
}

bool EditRecorder::redoRegion()
{
    if (!m_hist.canRedo()) return false;
    Tx t = m_hist.takeForRedo();
    applyRegion(t.before.start, int(t.before.blocks.size()), t.after);
    m_set(qMin(t.curAfterPos, m_doc->characterCount() - 1), qMin(t.curAfterAnchor, m_doc->characterCount() - 1));
    emit changed(m_revision);
    return true;
}

} // namespace hn::editor
