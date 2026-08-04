#pragma once
#include <QObject>
#include <QHash>
#include <QReadWriteLock>
#include <QReadLocker>
#include <cstdint>

/// Thread-safe mapping from element_id (uint64_t) to QObject* raw pointer.
///
/// Protected by a QReadWriteLock:
///   - Main thread  takes a WRITE lock during snapshot clear/rebuild.
///   - Worker thread takes a READ  lock during element lookups.
///
/// An epoch counter is incremented on every snapshot.  The worker thread
/// captures the epoch before dispatching to the main thread and re-validates
/// it after acquiring the read lock on the main thread.  A mismatch means
/// the element_map was rebuilt out from under the operation and the ID is
/// stale (error code 1002).
///
/// QPointer is deliberately NOT used:  QPointer is not thread-safe (it can
/// auto-null from any thread, corrupting concurrent reads).  Raw QObject*
/// with explicit lock protection is the correct pattern here.
class ElementMap
{
public:
    /// Clear all entries, reset next_id to 1, reset epoch to 0.
    /// Acquires a write lock internally.
    void clear();

    /// Insert or overwrite a mapping.  Acquires a write lock internally.
    /// @param id   Numeric element identifier.
    /// @param obj  Raw QObject pointer (never dereferenced without the lock).
    void insert(uint64_t id, QObject* obj);

    /// Look up an element by id.  Returns nullptr if not found.
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

    /// Reverse lookup: id of a QObject*, 0 if not mapped.
    /// Acquires a read lock internally.
    uint64_t idFor(QObject* obj) const;

    /// Return a copy of the full map (thread-safe snapshot). Caller owns the copy.
    QHash<uint64_t, QObject*> snapshot() const {
        QReadLocker l(&lock_);
        return map_;
    }

private:
    mutable QReadWriteLock lock_{QReadWriteLock::Recursive};
    QHash<uint64_t, QObject*> map_;
    QHash<QObject*, uint64_t> revMap_;   // obj -> id (maintained by insert/clear)
    uint64_t next_id_ = 1;
    uint64_t epoch_ = 0;
};
