// Application-owned bounded transaction history (spec 5.4). Native QTextDocument undo is disabled;
// every edit route records a block-window snapshot pair through EditRecorder.
#pragma once
#include <QList>
#include <QObject>
#include <QString>
#include <QTemporaryFile>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextListFormat>
#include <deque>
#include <functional>
#include <memory>

namespace hn::editor {

struct Frag { QString text; QTextCharFormat fmt; };
struct BlockSnap {
    QTextBlockFormat bf;      // ObjectIndex stripped
    QTextCharFormat cf;
    int listId = -1;          // QTextList::objectIndex() at capture time
    QTextListFormat lf;
    QList<Frag> frags;
};
struct Region {
    int start = 0;            // first block number
    int epoch = 0;            // list-id validity epoch
    QList<BlockSnap> blocks;
};

enum class TxKind { Typing, Delete, Paste, Cut, Drop, Format, Auto, Structure, ModeSwitch, Replace };

struct Tx {
    TxKind kind = TxKind::Structure;
    Region before, after;                 // region txs
    QString textBefore, textAfter;        // ModeSwitch / Replace payload (markdown or source text)
    int modeFrom = 0, modeTo = 0;         // ModeSwitch: 0 visual, 1 source
    int curBeforePos = 0, curBeforeAnchor = 0, curAfterPos = 0, curAfterAnchor = 0;
    qint64 time = 0;
    qint64 payload = 0;                   // accounted bytes held in memory
    std::shared_ptr<QTemporaryFile> spill; // non-null => before/after/text live on disk
};

qint64 regionBytes(const Region &r);
qint64 txBytes(const Tx &t);
Region captureRegion(QTextDocument *doc, int startBlock, int count, int epoch);
bool regionEqual(const Region &a, const Region &b);

class TxHistory {
public:
    static constexpr int kMaxTx = 500;
    static constexpr qint64 kMaxBytes = 2 * 1024 * 1024;
    static constexpr qint64 kMaxMerged = 64 * 1024;
    static constexpr qint64 kPauseMs = 1000;
    enum class PushResult { Stored, Merged, SpillFailed };

    void setClock(std::function<qint64()> c) { m_clock = std::move(c); }
    void setSpillDir(const QString &d) { m_spillDir = d; }
    qint64 now() const;
    void clear();
    PushResult push(Tx tx);
    bool canUndo() const { return m_pos > 0; }
    bool canRedo() const { return m_pos < int(m_txs.size()); }
    const Tx *peekUndo() const { return canUndo() ? &m_txs[m_pos - 1] : nullptr; }
    const Tx *peekRedo() const { return canRedo() ? &m_txs[m_pos] : nullptr; }
    Tx takeForUndo();   // payload loaded from disk if spilled
    Tx takeForRedo();
    int count() const { return int(m_txs.size()); }
    int position() const { return m_pos; }
    qint64 bytes() const { return m_bytes; }
    int evicted() const { return m_evicted; }
    int spilledCount() const;
    // Verifies a temp file can be written (used before applying an edit that might need recovery on disk).
    bool canSpill(QString *err = nullptr) const;
    void breakMerge() { m_noMerge = true; }
    QString lastError() const { return m_err; }

private:
    bool spill(Tx &t);
    static void load(Tx &t);
    std::deque<Tx> m_txs;
    int m_pos = 0;
    qint64 m_bytes = 0;
    int m_evicted = 0;
    bool m_noMerge = false;
    QString m_spillDir, m_err;
    std::function<qint64()> m_clock;
};

enum class WindowMode { Selection, Wide, WholeDoc };

class EditRecorder : public QObject {
    Q_OBJECT
public:
    explicit EditRecorder(QObject *parent = nullptr) : QObject(parent) {}
    using CursorGet = std::function<QTextCursor()>;
    using CursorSet = std::function<void(int pos, int anchor)>;
    void attach(QTextDocument *doc, CursorGet get, CursorSet set);
    QTextDocument *document() const { return m_doc; }
    TxHistory &history() { return m_hist; }
    const TxHistory &history() const { return m_hist; }
    // Fixup applied to restored block formats (e.g. theme line height).
    void setBlockFixup(std::function<void(QTextBlockFormat &)> f) { m_fixup = std::move(f); }

    class Scope {
    public:
        Scope() = default;
        Scope(Scope &&o) noexcept { *this = std::move(o); }
        Scope &operator=(Scope &&o) noexcept;
        ~Scope() { if (r) r->finish(*this); }
        void cancel();
        bool active() const { return r != nullptr; }
    private:
        friend class EditRecorder;
        EditRecorder *r = nullptr;
        TxKind kind = TxKind::Structure;
        Region before;
        int blocks0 = 0, lastBlock = 0;
        int pos = 0, anchor = 0;
    };
    Scope begin(TxKind kind, WindowMode wm);
    Scope beginBlocks(TxKind kind, int first, int last);
    bool undoRegion();
    bool redoRegion();
    void pushModeTx(Tx tx);             // ModeSwitch / Replace: caller did the work
    void newEpoch() { ++m_epoch; m_remap.clear(); }
    int epoch() const { return m_epoch; }
    int revision() const { return m_revision; }
    void resetRevision() { m_revision = 0; m_dirty = false; }
    void touch() { ++m_revision; emit changed(m_revision); }
    int unrecorded() const { return m_unrecorded; }
    void applyRegion(int startBlock, int oldCount, const Region &r);   // quiet structural replace
    // Suppress bookkeeping for programmatic changes that are not user edits (theme, normalize, load).
    struct Quiet { EditRecorder *r; explicit Quiet(EditRecorder *x) : r(x) { ++r->m_quiet; } ~Quiet() { --r->m_quiet; } };

signals:
    void changed(int revision);
    void historyInvalidated(const QString &reason);
    void recoveryWriteFailed(const QString &reason);

private:
    void finish(Scope &s);
    void onContentsChange();
    QTextDocument *m_doc = nullptr;
    CursorGet m_get;
    CursorSet m_set;
    TxHistory m_hist;
    std::function<void(QTextBlockFormat &)> m_fixup;
    QHash<int, int> m_remap;
    QMetaObject::Connection m_conn;
    int m_depth = 0, m_quiet = 0, m_revision = 0, m_epoch = 0, m_unrecorded = 0;
    bool m_dirty = false;
};

} // namespace hn::editor
