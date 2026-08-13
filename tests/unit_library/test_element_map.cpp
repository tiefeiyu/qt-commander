// ElementMap unit tests
//
// Tests thread-safe ID-to-QObject mapping:
//   - Insert / lookup / clear
//   - Epoch management
//   - Next-ID sequencing
//   - External lock/unlock for read and write
//   - Multiple inserts and overwrite semantics
//   - Clear resets epoch and nextId

#include "element_map.h"
#include <QObject>
#include <iostream>
#include <cstdint>

#if defined(_WIN32) && defined(_MSC_VER)
// MSVC-only: crtdbg/_set_abort_behavior are UCRT-only (no MinGW msvcrt)
#include <windows.h>
#include <crtdbg.h>
#endif

static int passed = 0, failed = 0;
#define TEST(n) do { std::cout << "  " << n << "... "; } while(0)
#define PASS()  do { std::cout << "PASS\n"; passed++; } while(0)
#define FAIL(m) do { std::cout << "FAIL: " << m << "\n"; failed++; } while(0)
#define CHECK(c,m) do { if(!(c)) { FAIL(m); return; } } while(0)

// ---------------------------------------------------------------------------
// 1. Insert element, lookup returns correct QObject*
// ---------------------------------------------------------------------------
static void test_insert_and_lookup()
{
    TEST("insert and lookup");
    ElementMap map;
    QObject obj;
    map.insert(42, &obj);
    QObject* result = map.lookup(42);
    CHECK(result == &obj, "lookup returned wrong pointer");
    PASS();
}

// ---------------------------------------------------------------------------
// 2. Lookup non-existent ID returns nullptr
// ---------------------------------------------------------------------------
static void test_lookup_nonexistent()
{
    TEST("lookup non-existent ID returns nullptr");
    ElementMap map;
    QObject* result = map.lookup(999);
    CHECK(result == nullptr, "should return nullptr for non-existent ID");
    PASS();
}

// ---------------------------------------------------------------------------
// 3. Clear() removes all entries
// ---------------------------------------------------------------------------
static void test_clear()
{
    TEST("clear removes all entries");
    ElementMap map;
    QObject obj1, obj2;
    map.insert(1, &obj1);
    map.insert(2, &obj2);
    map.clear();
    CHECK(map.lookup(1) == nullptr, "entry 1 should be removed after clear");
    CHECK(map.lookup(2) == nullptr, "entry 2 should be removed after clear");
    PASS();
}

// ---------------------------------------------------------------------------
// 4. Epoch increments correctly
// ---------------------------------------------------------------------------
static void test_epoch()
{
    TEST("epoch increments and reads correctly");
    ElementMap map;
    CHECK(map.epoch() == 0, "initial epoch should be 0");
    map.incrementEpoch();
    CHECK(map.epoch() == 1, "epoch should be 1 after one increment");
    map.incrementEpoch();
    CHECK(map.epoch() == 2, "epoch should be 2 after two increments");
    PASS();
}

// ---------------------------------------------------------------------------
// 5. Next ID returns sequential values (starts at 1, not consumed by insert)
// ---------------------------------------------------------------------------
static void test_next_id()
{
    TEST("nextId returns sequential values starting at 1");
    ElementMap map;
    CHECK(map.nextId() == 1, "initial nextId should be 1");

    QObject obj1, obj2;

    // insert at explicit id; nextId is unchanged because we didn't use nextId
    map.insert(10, &obj1);
    CHECK(map.nextId() == 1, "nextId unchanged after insert with explicit id");

    // Insert using nextId directly; nextId is NOT auto-advanced by insert
    // (the caller is responsible for advancing)
    map.insert(map.nextId(), &obj2);
    // nextId still 1 because ElementMap doesn't consume it automatically
    CHECK(map.nextId() == 1, "nextId remains 1 after insert(map.nextId(), ...)");
    PASS();
}

// ---------------------------------------------------------------------------
// 6. lockForRead / unlockRead
// ---------------------------------------------------------------------------
static void test_read_lock()
{
    TEST("lockForRead / unlockRead provides read access");
    ElementMap map;
    QObject obj;
    map.insert(1, &obj);

    map.lockForRead();
    QObject* result = map.lookup(1);
    CHECK(result == &obj, "lookup under read lock should succeed");
    map.unlockRead();

    PASS();
}

// ---------------------------------------------------------------------------
// 7. lockForWrite / unlock  (unlock releases a write lock)
// ---------------------------------------------------------------------------
static void test_write_lock()
{
    TEST("lockForWrite / unlock provides write access");
    ElementMap map;

    map.lockForWrite();
    QObject obj;
    map.insert(10, &obj);
    map.unlock();

    QObject* result = map.lookup(10);
    CHECK(result == &obj, "insert under write lock should persist after unlock");
    PASS();
}

// ---------------------------------------------------------------------------
// 8. Multiple inserts + lookups
// ---------------------------------------------------------------------------
static void test_multiple_inserts()
{
    TEST("multiple inserts and lookups");
    ElementMap map;
    QObject objs[5];

    for (int i = 0; i < 5; i++) {
        map.insert(static_cast<uint64_t>(i + 1), &objs[i]);
    }
    for (int i = 0; i < 5; i++) {
        uint64_t id = static_cast<uint64_t>(i + 1);
        QObject* result = map.lookup(id);
        CHECK(result == &objs[i], "lookup failed for sequential insert");
    }
    PASS();
}

// ---------------------------------------------------------------------------
// 9. Insert overwrites existing ID
// ---------------------------------------------------------------------------
static void test_insert_overwrite()
{
    TEST("insert overwrites existing ID");
    ElementMap map;
    QObject obj1, obj2;

    map.insert(1, &obj1);
    map.insert(1, &obj2);

    QObject* result = map.lookup(1);
    CHECK(result == &obj2, "should return the latest inserted object for overwritten ID");
    PASS();
}

// ---------------------------------------------------------------------------
// 10. Clear resets epoch and nextId
// ---------------------------------------------------------------------------
static void test_clear_resets_counters()
{
    TEST("clear resets epoch and nextId");
    ElementMap map;
    QObject obj;

    map.insert(100, &obj);
    map.incrementEpoch();
    CHECK(map.epoch() == 1, "expected epoch 1 before clear");

    map.clear();
    CHECK(map.epoch() == 0, "expected epoch 0 after clear");
    CHECK(map.nextId() == 1, "expected nextId 1 after clear");
    PASS();
}

// ---------------------------------------------------------------------------
// 11. snapshot() returns a copy of the map
// ---------------------------------------------------------------------------
static void test_snapshot()
{
    TEST("snapshot returns a copy unaffected by later inserts");
    ElementMap map;
    QObject obj1, obj2;
    map.insert(1, &obj1);
    map.insert(2, &obj2);

    QHash<uint64_t, QObject*> snap = map.snapshot();
    CHECK(snap.size() == 2, "snapshot should contain 2 entries");
    CHECK(snap.value(1) == &obj1, "snapshot id=1 should match");
    CHECK(snap.value(2) == &obj2, "snapshot id=2 should match");

    // Insert after snapshot -- original copy is unchanged
    QObject obj3;
    map.insert(3, &obj3);
    CHECK(snap.size() == 2, "snapshot copy should still have 2 entries");
    PASS();
}

// ---------------------------------------------------------------------------
// 12. Direct rwLock() accessor works with QReadLocker/QWriteLocker
// ---------------------------------------------------------------------------
static void test_rwlock_accessor()
{
    TEST("rwLock() accessor works with QReadLocker/QWriteLocker");
    ElementMap map;
    QObject obj;

    {
        QWriteLocker locker(map.rwLock());
        map.insert(7, &obj);
    }

    {
        QReadLocker locker(map.rwLock());
        CHECK(map.lookup(7) == &obj, "lookup after write-lock block should succeed");
    }
    PASS();
}

// ---------------------------------------------------------------------------
// 13. insertIfAbsent: existing object returns its current id; a new object
// gets the next id; ids are never invalidated by inserts
// ---------------------------------------------------------------------------
static void test_insert_if_absent()
{
    TEST("insertIfAbsent returns existing id or allocates next");
    ElementMap map;
    QObject obj1, obj2;

    const uint64_t id1 = map.insertIfAbsent(&obj1);
    CHECK(id1 == 1, "first insertIfAbsent should allocate id 1");
    CHECK(map.lookup(id1) == &obj1, "lookup id1 should return obj1");

    const uint64_t id1Again = map.insertIfAbsent(&obj1);
    CHECK(id1Again == id1, "re-insert of obj1 must return the SAME id");

    const uint64_t id2 = map.insertIfAbsent(&obj2);
    CHECK(id2 == 2, "second object should get the next id");
    CHECK(id2 != id1, "ids must be distinct");

    // Existing insert(id, obj) entries must be honored too.
    ElementMap map2;
    QObject obj3;
    map2.insert(42, &obj3);
    CHECK(map2.insertIfAbsent(&obj3) == 42,
          "insertIfAbsent must return the id from a prior insert()");

    // Null object is rejected.
    ElementMap map3;
    CHECK(map3.insertIfAbsent(nullptr) == 0, "null object must return 0");

    // After clear, ids are allocated fresh.
    map.clear();
    const uint64_t idAfterClear = map.insertIfAbsent(&obj1);
    CHECK(idAfterClear == 1, "after clear, nextId restarts at 1");
    PASS();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main()
{
#if defined(_WIN32) && defined(_MSC_VER)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    std::cout << "test_element_map\n" << std::flush;
    test_insert_and_lookup();
    test_lookup_nonexistent();
    test_clear();
    test_epoch();
    test_next_id();
    test_read_lock();
    test_write_lock();
    test_multiple_inserts();
    test_insert_overwrite();
    test_insert_if_absent();
    test_clear_resets_counters();
    test_snapshot();
    test_rwlock_accessor();

    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed > 0 ? 1 : 0;
}
