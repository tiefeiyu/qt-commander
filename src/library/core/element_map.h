#pragma once
#include <QObject>
#include <QHash>
#include <QPointer>
#include <QReadWriteLock>
#include <QReadLocker>
#include <cstdint>

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
// Qt 5 ships no qHash for QPointer, which QHash<QPointer<T>, ...> keys need.
// Defined in the global namespace so ADL from QtPrivate's qHash call sites
// finds it; Qt 6 provides its own overload, hence the version guard.
template <typename T>
inline uint qHash(const QPointer<T> &p, uint seed = 0) noexcept
{
    return qHash(p.data(), seed);
}
#endif

/// Thread-safe mapping from element_id (uint64_t) to QPointer<QObject>.
///
/// Dead-object safety comes from QPointer auto-nulling instead of
/// destroyed()-signal tracking.  The old tracking connected destroyed() to
/// this map, but the map lives on the RPC thread (no event loop there): a
/// queued connection was never delivered (leaving dangling pointers that
/// crashed QObject::disconnect in clear()), while forcing
/// Qt::DirectConnection deadlocked -- clear()/rebuild hold the map write
/// lock on the GUI thread and the slot re-enters it from ~QObject (the
/// QReadWriteLock is not recursive).  QPointer is nulled by Qt's internal
/// weak-ref machinery inside ~QObject with no callback, so no lock is
/// re-entered and stale entries simply read nullptr.
///
/// Threading: every map access runs on the GUI thread (the RPC dispatch
/// lambdas); QObject destruction that nulls QPointers also happens on the
/// GUI thread, so the QReadWriteLock guards the (now same-thread) accesses
/// and the QPointer read/write operations are race-free.
class ElementMap : public QObject
{
    Q_OBJECT
public:
    /// Clear all entries, reset next_id to 1, reset epoch to 0.
    /// Acquires a write lock internally.
    void clear();

    /// Insert or overwrite a mapping.  Acquires a write lock internally.
    /// @param id   Numeric element identifier.
    /// @param obj  Live QObject* (tracked via QPointer; nulls out on death).
    void insert(uint64_t id, QObject* obj);

    /// Grow-only insert: if *obj* is already mapped, return its existing
    /// id; otherwise allocate the next id and map it.  Never invalidates
    /// or renumbers existing ids -- the map only grows until clear().
    /// Returns 0 for a null object.  Acquires a write lock internally
    /// (recursive: safe to call under an outer write lock).
    uint64_t insertIfAbsent(QObject* obj);

    /// Look up an element by id.  Returns nullptr if the id is unknown or
    /// the object has been destroyed (QPointer auto-nulled).
    /// Acquires a read lock internally.
    /// @note The returned pointer is only valid while the caller holds
    ///       the read lock (via lockForRead/unlockRead).
    QObject* lookup(uint64_t id) const;

    /// Return the current epoch counter.  Acquires a read lock internally.
    uint64_t epoch() const;

    /// Return the next available element id (not consumed).
    uint64_t nextId() const;

    /// Atomically increment the epoch counter.  Acquires a write lock.
    void incrementEpoch();

    // -- External lock/unlock -----------------------------------------------
    // These allow a caller (e.g. the snapshot builder) to hold the write
    // lock across a clear/insert/incrementEpoch sequence without releasing
    // between steps.  The worker thread similarly uses lockForRead /
    // unlockRead when it needs to issue multiple lookups atomically.
    void lockForWrite();
    void unlock();
    void lockForRead() const;
    void unlockRead() const;

    /// Direct access to the underlying lock for use with QReadLocker/QWriteLocker.
    QReadWriteLock* rwLock() { return &lock_; }
    const QReadWriteLock* rwLock() const { return &lock_; }

    /// Alias for lookup() -- convenience for code that expects a `get` naming.
    QObject* get(uint64_t id) const { return lookup(id); }

    /// Reverse lookup: id of a live QObject*, 0 if not mapped.
    /// Acquires a read lock internally.  The QPointer key tracks object
    /// identity (not the raw address), so a destroyed object whose memory
    /// was reused by a new allocation never matches.
    uint64_t idFor(QObject* obj) const;

    /// Copy of the live mappings (dead objects omitted). Caller owns the copy.
    QHash<uint64_t, QObject*> snapshot() const {
        QReadLocker l(&lock_);
        QHash<uint64_t, QObject*> out;
        out.reserve(map_.size());
        for (auto it = map_.constBegin(); it != map_.constEnd(); ++it) {
            if (QObject* o = it.value().data())
                out.insert(it.key(), o);
        }
        return out;
    }

private:
    mutable QReadWriteLock lock_{QReadWriteLock::Recursive};
    QHash<uint64_t, QPointer<QObject>> map_;
    QHash<QPointer<QObject>, uint64_t> revMap_;   // QPointer -> id
    uint64_t next_id_ = 1;
    uint64_t epoch_ = 0;
};
