#include "element_map.h"

// ======================== Convenience methods =========================

void ElementMap::clear()
{
    QWriteLocker locker(&lock_);
    map_.clear();
    next_id_ = 1;
    epoch_ = 0;
}

void ElementMap::insert(uint64_t id, QObject* obj)
{
    QWriteLocker locker(&lock_);
    map_.insert(id, obj);
}

QObject* ElementMap::lookup(uint64_t id) const
{
    QReadLocker locker(&lock_);
    return map_.value(id, nullptr);
}

uint64_t ElementMap::epoch() const
{
    QReadLocker locker(&lock_);
    return epoch_;
}

uint64_t ElementMap::nextId() const
{
    QReadLocker locker(&lock_);
    return next_id_;
}

void ElementMap::incrementEpoch()
{
    QWriteLocker locker(&lock_);
    epoch_++;
}

// ======================== External lock/unlock =========================
//
// These methods are used when a caller needs to hold the lock across
// multiple operations without releasing in between.
//
// Typical snapshot sequence on the main thread:
//   map.lockForWrite();
//   map.clear();                       // clears + resets next/epoch
//   map_ = rebuild(...);               // external rebuild
//   map.incrementEpoch();              // not needed if clear() already set it
//   map.unlock();
//
// Typical worker-thread sequence:
//   map.lockForRead();
//   QObject* obj = map.lookup(id);
//   if (obj) { /* read properties */ }
//   map.unlockRead();

void ElementMap::lockForWrite()
{
    lock_.lockForWrite();
}

void ElementMap::unlock()
{
    lock_.unlock();
}

void ElementMap::lockForRead() const
{
    lock_.lockForRead();
}

void ElementMap::unlockRead() const
{
    lock_.unlock();
}
