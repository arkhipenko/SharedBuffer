// Host unit tests for the SharedBuffer library
// Build and run: make -C tests/host [CXX=clang++] [STD=c++17]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <type_traits>
#if !defined(TEST_COUNTED_CRITICAL)
#include <atomic>
#include <thread>
#endif
#include "SharedBuffer.h"

typedef SharedBuffer SB;

static_assert(SHARED_BUFFER_MAX_BUFFERS >= 8, "the tests register up to 8 buffers in one object");
static_assert(!std::is_copy_constructible<SharedBuffer>::value, "a copy would share the pool (D1)");
static_assert(!std::is_copy_assignable<SharedBuffer>::value, "a copy would share the pool (D1)");

static const size_t A = SHARED_BUFFER_ALIGN;
static const size_t SIZE_MAX_ = (size_t) -1;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
        g_checks++; \
        if ( !(cond) ) { g_failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

#define CHECK_EQ(a, b) do { \
        g_checks++; \
        long long va_ = (long long) (a), vb_ = (long long) (b); \
        if ( va_ != vb_ ) { g_failures++; \
            printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); } \
    } while (0)

#define CHECK_STR(a, b) do { \
        g_checks++; \
        const char* sa_ = (a); const char* sb_ = (b); \
        if ( sa_ == NULL || sb_ == NULL || strcmp(sa_, sb_) != 0 ) { g_failures++; \
            printf("FAIL %s:%d: %s == \"%s\" (got \"%s\")\n", __FILE__, __LINE__, #a, sb_ ? sb_ : "(null)", sa_ ? sa_ : "(null)"); } \
    } while (0)

static size_t al(size_t n) { return (n + A - 1) / A * A; }

// ---------------------------------------------------------------------------------------
// Counting critical section (variant "counted", see cs_hooks.h)

static int g_depth  = 0;    // current nesting depth, must be 0 or 1

#if defined(TEST_COUNTED_CRITICAL)
static int  g_nested = 0;   // number of enters while already inside
static long g_enters = 0;
static long g_exits  = 0;

// g_inject runs once, just before critical section number g_injectAt starts. It plays
// another task that runs between two critical sections of the call under test.
static void (*g_inject)() = NULL;
static long g_injectAt = -1;

void sb_test_enter() {
    if ( g_injectAt >= 0 && g_enters == g_injectAt ) {
        void (*f)() = g_inject;
        g_injectAt = -1;
        f();
    }
    if ( g_depth != 0 ) g_nested++;
    g_depth++;
    g_enters++;
}
void sb_test_exit()  { g_depth--; g_exits++; }
#endif

// ---------------------------------------------------------------------------------------

static void test_register() {
    SB sb;
    CHECK_EQ(sb.registerBuffer(NULL, 10), SB::SB_ERR_ARGUMENT);
    CHECK_EQ(sb.registerBuffer("", 10), SB::SB_ERR_ARGUMENT);
    CHECK_EQ(sb.registerBuffer("a", 0), SB::SB_ERR_ARGUMENT);
    CHECK_EQ(sb.count(), 0);

    SB::Handle a = sb.registerBuffer("a", 100);
    SB::Handle b = sb.registerBuffer("b", 200);
    CHECK_EQ(a, 0);
    CHECK_EQ(b, 1);
    CHECK_EQ(sb.registerBuffer("a", 5), SB::SB_ERR_DUPLICATE);
    char copyOfB[] = "b";                                   // compared by content
    CHECK_EQ(sb.registerBuffer(copyOfB, 5), SB::SB_ERR_DUPLICATE);
    CHECK_EQ(sb.count(), 2);
    CHECK_EQ(sb.size(a), 100);
    CHECK_EQ(sb.size(b), 200);
    CHECK_STR(sb.name(a), "a");
    CHECK_STR(sb.name(b), "b");
    CHECK_EQ(sb.requiredSize(), al(200));
    CHECK(!sb.isHeld(a));
    CHECK(sb.pointer(a) == NULL);

    // Unknown handles
    CHECK_EQ(sb.size(-1), 0);
    CHECK_EQ(sb.size(2), 0);
    CHECK(sb.name(-1) == NULL);
    CHECK(sb.name(2) == NULL);
    CHECK(!sb.isHeld(-1));
    CHECK(sb.pointer(5) == NULL);

    // Fill the table
    static char names[SHARED_BUFFER_MAX_BUFFERS][16];
    for ( int i = 2; i < SHARED_BUFFER_MAX_BUFFERS; i++ ) {
        snprintf(names[i], sizeof(names[i]), "n%d", i);
        CHECK_EQ(sb.registerBuffer(names[i], 10), i);
    }
    CHECK_EQ(sb.count(), SHARED_BUFFER_MAX_BUFFERS);
    CHECK_EQ(sb.registerBuffer("x", 1), SB::SB_ERR_FULL);
    CHECK_EQ(sb.registerBuffer("a", 1), SB::SB_ERR_DUPLICATE);    // duplicate is reported first
    CHECK_EQ(sb.count(), SHARED_BUFFER_MAX_BUFFERS);

    // Rounding overflow boundary
    SB big;
    CHECK_EQ(big.registerBuffer("max", SIZE_MAX_ - (A - 1)), 0);
    CHECK_EQ(big.requiredSize(), SIZE_MAX_ - (A - 1));
    if ( A > 1 ) {
        CHECK_EQ(big.registerBuffer("over", SIZE_MAX_ - (A - 1) + 1), SB::SB_ERR_TOO_LARGE);
        CHECK_EQ(big.registerBuffer("max2", SIZE_MAX_), SB::SB_ERR_TOO_LARGE);
    }
    CHECK_EQ(big.count(), 1);
}

static void test_begin_heap() {
    SB sb;
    CHECK_EQ(sb.begin(), SB::SB_ERR_ARGUMENT);              // nothing to size the pool by
    CHECK(!sb.started());
    CHECK_EQ(sb.poolSize(), 0);
    CHECK_EQ(sb.freeBytes(), 0);
    CHECK_EQ(sb.largestFree(), 0);
    CHECK_EQ(sb.end(), SB::SB_ERR_NOT_STARTED);

    SB::Handle calc = sb.registerBuffer("calc", 16383);
    sb.registerBuffer("dfuA", 4096);
    CHECK_EQ(sb.requiredSize(), al(16383));

    CHECK_EQ(sb.begin(), SB::SB_OK);
    CHECK(sb.started());
    CHECK_EQ(sb.poolSize(), al(16383));
    CHECK_EQ(sb.freeBytes(), al(16383));
    CHECK_EQ(sb.largestFree(), al(16383));
    CHECK_EQ(sb.begin(), SB::SB_ERR_STARTED);
    static uint8_t other[64];
    CHECK_EQ(sb.begin(other, sizeof(other)), SB::SB_ERR_STARTED);

    void* p = sb.acquire(calc);
    CHECK(p != NULL);
    CHECK_EQ((uintptr_t) p % A, 0);
    memset(p, 0xA5, 16383);                                  // the whole buffer is writable
    CHECK_EQ(sb.release(calc), SB::SB_OK);
    CHECK_EQ(sb.end(), SB::SB_OK);

    // begin(min): the larger of min and the largest registration
    CHECK_EQ(sb.begin(20001), SB::SB_OK);
    CHECK_EQ(sb.poolSize(), al(20001));
    CHECK_EQ(sb.end(), SB::SB_OK);
    CHECK_EQ(sb.begin(10), SB::SB_OK);
    CHECK_EQ(sb.poolSize(), al(16383));
    CHECK_EQ(sb.end(), SB::SB_OK);

    // malloc() failure
    CHECK_EQ(sb.begin(SIZE_MAX_ / 2), SB::SB_ERR_NO_MEMORY);
    CHECK(!sb.started());
    if ( A > 1 ) CHECK_EQ(sb.begin(SIZE_MAX_), SB::SB_ERR_TOO_LARGE);

    // begin() with only a minimum and no registrations
    SB empty;
    CHECK_EQ(empty.begin(100), SB::SB_OK);
    CHECK_EQ(empty.poolSize(), al(100));
    CHECK_EQ(empty.end(), SB::SB_OK);
}

static void test_begin_storage() {
    alignas(64) static uint8_t storage[1024];
    SB sb;
    SB::Handle a = sb.registerBuffer("a", 300);

    CHECK_EQ(sb.begin(NULL, 100), SB::SB_ERR_ARGUMENT);
    CHECK_EQ(sb.begin(storage, 0), SB::SB_ERR_ARGUMENT);
    CHECK_EQ(sb.begin(storage, 299), SB::SB_ERR_TOO_LARGE);
    CHECK(!sb.started());

    // Aligned storage: all of it becomes the pool
    CHECK_EQ(sb.begin(storage, sizeof(storage)), SB::SB_OK);
    CHECK_EQ(sb.poolSize(), 1024 / A * A);
    uint8_t* p = (uint8_t*) sb.acquire(a);
    CHECK(p == storage);
    CHECK_EQ(sb.release(a), SB::SB_OK);
    CHECK_EQ(sb.end(), SB::SB_OK);                          // must not free() static memory

    // Misaligned storage: the start moves to the next boundary
    CHECK_EQ(sb.begin(storage + 1, sizeof(storage) - 1), SB::SB_OK);
    size_t skip = ( A > 1 ) ? A - 1 : 0;
    CHECK_EQ(sb.poolSize(), (sizeof(storage) - 1 - skip) / A * A);
    p = (uint8_t*) sb.acquire(a);
    CHECK(p == storage + 1 + skip);
    CHECK_EQ((uintptr_t) p % A, 0);
    CHECK_EQ(sb.release(a), SB::SB_OK);
    CHECK_EQ(sb.end(), SB::SB_OK);

    if ( A > 1 ) {
        CHECK_EQ(sb.begin(storage + 1, A - 1), SB::SB_ERR_ARGUMENT);   // nothing left after alignment
        CHECK_EQ(sb.begin(storage + 1, A), SB::SB_ERR_ARGUMENT);       // 1 byte left, less than A
    }

    // Storage with nothing registered yet
    SB empty;
    CHECK_EQ(empty.begin(storage, 256), SB::SB_OK);
    CHECK_EQ(empty.poolSize(), 256 / A * A);
    CHECK_EQ(empty.end(), SB::SB_OK);
}

static void test_acquire_release() {
    SB sb;
    SB::Handle a = sb.registerBuffer("a", 100);
    SB::Handle b = sb.registerBuffer("b", 50);
    SB::Status st = SB::SB_OK;

    CHECK(sb.acquire(a, &st) == NULL);
    CHECK_EQ(st, SB::SB_ERR_NOT_STARTED);
    CHECK_EQ(sb.release(a), SB::SB_ERR_NOT_HELD);
    CHECK_EQ(sb.begin(), SB::SB_OK);

    CHECK(sb.acquire(-1, &st) == NULL);
    CHECK_EQ(st, SB::SB_ERR_ARGUMENT);
    CHECK(sb.acquire(2, &st) == NULL);
    CHECK_EQ(st, SB::SB_ERR_ARGUMENT);
    CHECK_EQ(sb.release(-1), SB::SB_ERR_ARGUMENT);
    CHECK_EQ(sb.release(2), SB::SB_ERR_ARGUMENT);
    CHECK_EQ(sb.release(a), SB::SB_ERR_NOT_HELD);

    void* pa = sb.acquire(a, &st);
    CHECK(pa != NULL);
    CHECK_EQ(st, SB::SB_OK);
    CHECK(sb.isHeld(a));
    CHECK(sb.pointer(a) == pa);
    CHECK_EQ(sb.freeBytes(), 0);

    CHECK(sb.acquire(a, &st) == NULL);                      // already held
    CHECK_EQ(st, SB::SB_ERR_HELD);
    CHECK(sb.acquire(b, &st) == NULL);                      // pool is exactly a
    CHECK_EQ(st, SB::SB_ERR_NO_SPACE);
    CHECK(!sb.isHeld(b));

    CHECK_EQ(sb.release(a), SB::SB_OK);
    CHECK_EQ(sb.release(a), SB::SB_ERR_NOT_HELD);
    CHECK(sb.pointer(a) == NULL);
    CHECK(!sb.isHeld(a));

    void* pb = sb.acquire(b);                               // status pointer is optional
    CHECK(pb == pa);                                        // the same bytes, offset 0
    CHECK_EQ(sb.freeBytes(), al(100) - al(50));
    CHECK_EQ(sb.release(b), SB::SB_OK);
    CHECK_EQ(sb.end(), SB::SB_OK);
}

// The owner's example: DFU needs 2 x 4096, a calculation needs 16384, never at the same time
static void test_dfu_and_calc() {
    SB sb;
    SB::Handle calc = sb.registerBuffer("calc", 16384);
    SB::Handle dfuA = sb.registerBuffer("dfuA", 4096);
    SB::Handle dfuB = sb.registerBuffer("dfuB", 4096);
    SB::Status st;
    CHECK_EQ(sb.begin(), SB::SB_OK);
    CHECK_EQ(sb.poolSize(), 16384);

    // Normal operation
    uint8_t* base = (uint8_t*) sb.acquire(calc);
    CHECK(base != NULL);
    CHECK(sb.acquire(dfuA, &st) == NULL);
    CHECK_EQ(st, SB::SB_ERR_NO_SPACE);
    CHECK_EQ(sb.release(calc), SB::SB_OK);

    // DFU
    uint8_t* a = (uint8_t*) sb.acquire(dfuA);
    uint8_t* b = (uint8_t*) sb.acquire(dfuB);
    CHECK(a == base);
    CHECK(b == base + 4096);
    CHECK(sb.acquire(calc, &st) == NULL);
    CHECK_EQ(st, SB::SB_ERR_NO_SPACE);
    CHECK_EQ(sb.release(dfuA), SB::SB_OK);
    CHECK(sb.acquire(calc, &st) == NULL);                   // dfuB still holds 4096..8191
    CHECK_EQ(st, SB::SB_ERR_NO_SPACE);
    CHECK_EQ(sb.release(dfuB), SB::SB_OK);

    // Back to normal operation
    CHECK(sb.acquire(calc) == base);
    CHECK_EQ(sb.release(calc), SB::SB_OK);
    CHECK_EQ(sb.end(), SB::SB_OK);
}

// A small buffer can use bytes that a held buffer does not cover
static void test_byte_range() {
    SB sb;
    SB::Handle dfuA = sb.registerBuffer("dfuA", 4096);
    SB::Handle dfuB = sb.registerBuffer("dfuB", 4096);
    SB::Handle icon = sb.registerBuffer("icon", 1024);
    CHECK_EQ(sb.begin(8192), SB::SB_OK);

    uint8_t* a = (uint8_t*) sb.acquire(dfuA);
    uint8_t* b = (uint8_t*) sb.acquire(dfuB);
    CHECK(a != NULL && b == a + 4096);
    CHECK(sb.acquire(icon) == NULL);
    CHECK_EQ(sb.release(dfuA), SB::SB_OK);
    CHECK(sb.acquire(icon) == a);                           // 0..1023 is free while dfuB is held
    CHECK_EQ(sb.largestFree(), 4096 - 1024);
    CHECK(sb.acquire(dfuA) == NULL);                        // 3072 free before dfuB: too small
    CHECK_EQ(sb.release(icon), SB::SB_OK);
    CHECK_EQ(sb.release(dfuB), SB::SB_OK);
    CHECK_EQ(sb.end(), SB::SB_OK);
}

// The worked example in README.md, "How buffers share the pool"
static void test_readme_example() {
    SB sb;
    SB::Handle calc = sb.registerBuffer("calc", 16384);
    SB::Handle dfuA = sb.registerBuffer("dfuA", 4096);
    SB::Handle dfuB = sb.registerBuffer("dfuB", 4096);
    SB::Handle icon = sb.registerBuffer("icon", 1024);
    SB::Status st;
    CHECK_EQ(sb.begin(), SB::SB_OK);
    CHECK_EQ(sb.poolSize(), 16384);

    uint8_t* base = (uint8_t*) sb.acquire(dfuA);            // offset 0
    CHECK(base != NULL);
    CHECK(sb.acquire(dfuB) == base + 4096);                 // offset 4096
    CHECK(sb.acquire(calc, &st) == NULL);
    CHECK_EQ(st, SB::SB_ERR_NO_SPACE);
    CHECK_EQ(sb.largestFree(), 8192);
    CHECK_EQ(sb.release(dfuA), SB::SB_OK);                  // gaps 0..4095 and 8192..16383
    CHECK(sb.acquire(icon) == base);                        // best fit: the 4096 gap
    CHECK_EQ(sb.release(icon), SB::SB_OK);
    CHECK_EQ(sb.release(dfuB), SB::SB_OK);
    CHECK(sb.acquire(calc) == base);
    CHECK_EQ(sb.release(calc), SB::SB_OK);
    CHECK_EQ(sb.end(), SB::SB_OK);
}

// Sizes are multiples of 64, so the offsets do not depend on SHARED_BUFFER_ALIGN (up to 64)
static void test_best_fit() {
    SB sb;
    SB::Handle p1 = sb.registerBuffer("p1", 128);
    SB::Handle p2 = sb.registerBuffer("p2", 256);
    SB::Handle p3 = sb.registerBuffer("p3", 128);
    SB::Handle p4 = sb.registerBuffer("p4", 256);
    SB::Handle s  = sb.registerBuffer("s", 128);
    SB::Handle t  = sb.registerBuffer("t", 256);
    CHECK_EQ(sb.begin(1024), SB::SB_OK);

    uint8_t* base = (uint8_t*) sb.acquire(p1);              // 0..127
    CHECK(sb.acquire(p2) == base + 128);                    // 128..383
    CHECK(sb.acquire(p3) == base + 384);                    // 384..511
    CHECK(sb.acquire(p4) == base + 512);                    // 512..767, tail gap 768..1023
    CHECK_EQ(sb.release(p1), SB::SB_OK);                    // gap 0..127
    CHECK_EQ(sb.release(p3), SB::SB_OK);                    // gap 384..511

    // Gaps 128 @ 0, 128 @ 384, 256 @ 768: smallest that fits, lowest offset on a tie
    CHECK(sb.acquire(s) == base);
    CHECK(sb.acquire(t) == base + 768);
    CHECK_EQ(sb.release(s), SB::SB_OK);
    CHECK_EQ(sb.release(t), SB::SB_OK);

    // Gaps 128 @ 0, 128 @ 384, 256 @ 768. Release p2: gap 0..511 (512), 256 @ 768.
    CHECK_EQ(sb.release(p2), SB::SB_OK);
    CHECK(sb.acquire(t) == base + 768);                     // best fit, not first fit (which is 0)
    CHECK(sb.acquire(s) == base);                           // only gap left
    CHECK_EQ(sb.release(s), SB::SB_OK);
    CHECK_EQ(sb.release(t), SB::SB_OK);
    CHECK_EQ(sb.release(p4), SB::SB_OK);
    CHECK_EQ(sb.freeBytes(), 1024);
    CHECK_EQ(sb.end(), SB::SB_OK);
}

// Enough free bytes in total, but no single gap is large enough
static void test_fragmentation() {
    SB sb;
    SB::Handle a = sb.registerBuffer("a", 384);
    SB::Handle b = sb.registerBuffer("b", 256);
    SB::Handle c = sb.registerBuffer("c", 384);
    SB::Handle d = sb.registerBuffer("d", 512);
    SB::Status st;
    CHECK_EQ(sb.begin(1024), SB::SB_OK);
    uint8_t* base = (uint8_t*) sb.acquire(a);
    CHECK(sb.acquire(b) == base + 384);
    CHECK(sb.acquire(c) == base + 640);
    CHECK_EQ(sb.release(a), SB::SB_OK);
    CHECK_EQ(sb.release(c), SB::SB_OK);
    CHECK_EQ(sb.freeBytes(), 768);
    CHECK_EQ(sb.largestFree(), 384);
    CHECK(sb.acquire(d, &st) == NULL);
    CHECK_EQ(st, SB::SB_ERR_NO_SPACE);
    CHECK_EQ(sb.release(b), SB::SB_OK);
    CHECK(sb.acquire(d) == base);
    CHECK_EQ(sb.release(d), SB::SB_OK);
    CHECK_EQ(sb.end(), SB::SB_OK);
}

static void test_late_registration() {
    SB sb;
    SB::Handle a = sb.registerBuffer("a", 100);
    CHECK_EQ(sb.begin(), SB::SB_OK);
    CHECK_EQ(sb.poolSize(), al(100));

    SB::Handle late = sb.registerBuffer("late", al(100));
    CHECK_EQ(late, 1);
    CHECK_EQ(sb.registerBuffer("late2", al(100) + 1), SB::SB_ERR_TOO_LARGE);
    CHECK_EQ(sb.count(), 2);
    CHECK_EQ(sb.poolSize(), al(100));                       // the pool never grows

    CHECK(sb.acquire(a) != NULL);
    CHECK(sb.acquire(late) == NULL);
    CHECK_EQ(sb.release(a), SB::SB_OK);
    CHECK(sb.acquire(late) != NULL);
    CHECK_EQ(sb.release(late), SB::SB_OK);

    // After end() the limit is gone again, and begin() sizes the pool to the new largest
    CHECK_EQ(sb.end(), SB::SB_OK);
    CHECK_EQ(sb.registerBuffer("late2", 1000), 2);
    CHECK_EQ(sb.begin(), SB::SB_OK);
    CHECK_EQ(sb.poolSize(), al(1000));
    CHECK_EQ(sb.end(), SB::SB_OK);
}

// 1.0.1: a granted buffer is all 0; a failed acquire touches nothing
static void test_zero_on_acquire() {
    SB sb;
    SB::Handle a = sb.registerBuffer("a", 1000);
    SB::Handle b = sb.registerBuffer("b", 300);
    CHECK_EQ(sb.begin(), SB::SB_OK);
    const size_t pool = sb.poolSize();

    uint8_t* pa = (uint8_t*) sb.acquire(a);
    CHECK(pa != NULL);
    size_t nonZero = 0;
    for ( size_t i = 0; i < 1000; i++ ) if ( pa[i] != 0 ) nonZero++;
    CHECK_EQ(nonZero, 0);
    memset(pa, 0xA5, pool);                                 // dirty the whole pool

    SB::Status st;
    CHECK(sb.acquire(b, &st) == NULL);                      // a fills the pool
    CHECK_EQ(st, SB::SB_ERR_NO_SPACE);
    size_t changed = 0;
    for ( size_t i = 0; i < pool; i++ ) if ( pa[i] != 0xA5 ) changed++;
    CHECK_EQ(changed, 0);                                   // the failed acquire wrote nothing
    CHECK_EQ(sb.release(a), SB::SB_OK);

    uint8_t* pb = (uint8_t*) sb.acquire(b);                 // the same bytes as a, offset 0
    CHECK(pb == pa);
    nonZero = 0;
    for ( size_t i = 0; i < 300; i++ ) if ( pb[i] != 0 ) nonZero++;
    CHECK_EQ(nonZero, 0);
    changed = 0;
    for ( size_t i = al(300); i < pool; i++ ) if ( pb[i] != 0xA5 ) changed++;
    CHECK_EQ(changed, 0);                                   // nothing past b's slot is written
    CHECK_EQ(sb.release(b), SB::SB_OK);
    CHECK_EQ(sb.end(), SB::SB_OK);
}

// 1.0.1: begin() clears the whole pool; a begin() refused at once touches nothing
static void test_begin_clears() {
    alignas(64) static uint8_t storage[512];
    memset(storage, 0xEE, sizeof(storage));
    SB sb;
    SB::Handle a = sb.registerBuffer("a", 64);
    CHECK_EQ(sb.begin(storage, sizeof(storage)), SB::SB_OK);
    size_t nonZero = 0;
    for ( size_t i = 0; i < sb.poolSize(); i++ ) if ( storage[i] != 0 ) nonZero++;
    CHECK_EQ(nonZero, 0);

    uint8_t* p = (uint8_t*) sb.acquire(a);
    CHECK(p == storage);
    memset(p, 0x77, 64);
    CHECK_EQ(sb.begin(storage, sizeof(storage)), SB::SB_ERR_STARTED);
    CHECK_EQ(sb.begin(), SB::SB_ERR_STARTED);
    size_t changed = 0;
    for ( size_t i = 0; i < 64; i++ ) if ( storage[i] != 0x77 ) changed++;
    CHECK_EQ(changed, 0);                                   // the held buffer kept its data
    CHECK_EQ(sb.release(a), SB::SB_OK);
    CHECK_EQ(sb.end(), SB::SB_OK);

    // Heap pool: the bytes past the first buffer were cleared by begin(), not by acquire()
    SB heap;
    SB::Handle small = heap.registerBuffer("small", 16);
    CHECK_EQ(heap.begin(4096), SB::SB_OK);
    uint8_t* q = (uint8_t*) heap.acquire(small);            // offset 0
    CHECK(q != NULL);
    nonZero = 0;
    for ( size_t i = 0; i < heap.poolSize(); i++ ) if ( q[i] != 0 ) nonZero++;
    CHECK_EQ(nonZero, 0);
    CHECK_EQ(heap.release(small), SB::SB_OK);
    CHECK_EQ(heap.end(), SB::SB_OK);
}

static void test_end() {
    SB sb;
    SB::Handle a = sb.registerBuffer("a", 64);
    SB::Status st;
    CHECK_EQ(sb.begin(), SB::SB_OK);
    CHECK(sb.acquire(a) != NULL);
    CHECK_EQ(sb.end(), SB::SB_ERR_HELD);
    CHECK(sb.started());
    CHECK_EQ(sb.release(a), SB::SB_OK);
    CHECK_EQ(sb.end(), SB::SB_OK);
    CHECK(!sb.started());
    CHECK_EQ(sb.poolSize(), 0);
    CHECK_EQ(sb.count(), 1);                                // registrations stay
    CHECK(sb.acquire(a, &st) == NULL);
    CHECK_EQ(st, SB::SB_ERR_NOT_STARTED);
    CHECK_EQ(sb.end(), SB::SB_ERR_NOT_STARTED);
    CHECK_EQ(sb.begin(), SB::SB_OK);
    CHECK(sb.acquire(a) != NULL);
    CHECK_EQ(sb.release(a), SB::SB_OK);
    CHECK_EQ(sb.end(), SB::SB_OK);
}

// ---------------------------------------------------------------------------------------

static std::vector<std::string> g_lines;
static int g_writerDepthErrors = 0;

static void collectLine(const char* line) {
    if ( g_depth != 0 ) g_writerDepthErrors++;              // must run outside the critical section
    g_lines.push_back(line);
}

static std::string fmt(const char* format, unsigned long x, unsigned long y = 0, unsigned long z = 0) {
    char buf[128];
    snprintf(buf, sizeof(buf), format, x, y, z);
    return buf;
}

static void test_dump() {
    SB sb;
    sb.dump(NULL);                                          // ignored
    SB::Handle a = sb.registerBuffer("alpha", 100);
    sb.registerBuffer("beta", 300);

    g_lines.clear();
    sb.dump(collectLine);
    CHECK_EQ(g_lines.size(), 3);
    if ( g_lines.size() == 3 ) {
        CHECK(g_lines[0] == fmt("SharedBuffer: not started, %lu bytes required, %lu of %lu buffers",
                                al(300), 2, SHARED_BUFFER_MAX_BUFFERS));
        CHECK(g_lines[1] == "  #0 alpha: 100 bytes, not held");
        CHECK(g_lines[2] == "  #1 beta: 300 bytes, not held");
    }

    CHECK_EQ(sb.begin(al(300) + al(100)), SB::SB_OK);
    CHECK(sb.acquire(1) != NULL);                           // beta at 0
    CHECK(sb.acquire(a) != NULL);                           // alpha after beta
    g_lines.clear();
    sb.dump(collectLine);
    CHECK_EQ(g_lines.size(), 3);
    if ( g_lines.size() == 3 ) {
        CHECK(g_lines[0] == fmt("SharedBuffer: pool %lu bytes, %lu free, largest gap %lu, 2 of ",
                                al(300) + al(100), 0, 0) + fmt("%lu buffers", SHARED_BUFFER_MAX_BUFFERS));
        CHECK(g_lines[1] == fmt("  #0 alpha: 100 bytes, held at offset %lu", al(300)));
        CHECK(g_lines[2] == "  #1 beta: 300 bytes, held at offset 0");
    }
    CHECK_EQ(sb.release(a), SB::SB_OK);
    CHECK_EQ(sb.release(1), SB::SB_OK);
    CHECK_EQ(sb.end(), SB::SB_OK);

    // A long name is cut to the line buffer (95 characters)
    static char longName[200];
    memset(longName, 'x', sizeof(longName) - 1);
    SB lng;
    CHECK_EQ(lng.registerBuffer(longName, 1), 0);
    g_lines.clear();
    lng.dump(collectLine);
    CHECK_EQ(g_lines.size(), 2);
    if ( g_lines.size() == 2 ) CHECK_EQ(g_lines[1].size(), 95);
    CHECK_EQ(g_writerDepthErrors, 0);
}

static void test_status_text() {
    CHECK_STR(SB::statusText(SB::SB_OK), "ok");
    CHECK_STR(SB::statusText(SB::SB_ERR_ARGUMENT), "invalid argument");
    CHECK_STR(SB::statusText(SB::SB_ERR_DUPLICATE), "duplicate name");
    CHECK_STR(SB::statusText(SB::SB_ERR_FULL), "registration table full");
    CHECK_STR(SB::statusText(SB::SB_ERR_TOO_LARGE), "too large");
    CHECK_STR(SB::statusText(SB::SB_ERR_NO_MEMORY), "out of memory");
    CHECK_STR(SB::statusText(SB::SB_ERR_NOT_STARTED), "not started");
    CHECK_STR(SB::statusText(SB::SB_ERR_STARTED), "already started");
    CHECK_STR(SB::statusText(SB::SB_ERR_HELD), "held");
    CHECK_STR(SB::statusText(SB::SB_ERR_NOT_HELD), "not held");
    CHECK_STR(SB::statusText(SB::SB_ERR_NO_SPACE), "no space");
    CHECK_STR(SB::statusText(5), "ok");                     // a handle
    CHECK_STR(SB::statusText(-11), "unknown status");
    CHECK_STR(SB::statusText(-128), "unknown status");
}

// ---------------------------------------------------------------------------------------
// Random operations checked against a byte map of the pool

static uint32_t g_rng = 12345;
static uint32_t rnd(uint32_t n) { g_rng = g_rng * 1103515245u + 12345u; return (g_rng >> 8) % n; }

static void test_random() {
    const size_t POOL = 2048;
    const int    N    = 8;
    SB sb;
    static char names[N][16];
    size_t sizes[N];
    for ( int i = 0; i < N; i++ ) {
        snprintf(names[i], sizeof(names[i]), "r%d", i);
        sizes[i] = 1 + rnd(600);
        CHECK_EQ(sb.registerBuffer(names[i], sizes[i]), i);
    }
    CHECK_EQ(sb.begin(POOL), SB::SB_OK);
    const size_t pool = sb.poolSize();
    CHECK_EQ(pool, al(POOL));

    std::vector<int> owner(pool, -1);                       // byte -> handle, -1 free
    uint8_t* base = NULL;
    bool held[N] = { false };
    size_t offset[N] = { 0 };
    long acquired = 0, noSpace = 0, badData = 0, badPlace = 0, badStats = 0, badZero = 0;

    for ( int op = 0; op < 20000; op++ ) {
        int h = (int) rnd(N);
        if ( held[h] ) {
            uint8_t* p = (uint8_t*) sb.pointer((SB::Handle) h);
            for ( size_t i = 0; i < sizes[h]; i++ ) if ( p[i] != (uint8_t) (h * 37 + 1) ) { badData++; break; }
            CHECK_EQ(sb.release((SB::Handle) h), SB::SB_OK);
            for ( size_t i = 0; i < al(sizes[h]); i++ ) owner[offset[h] + i] = -1;
            held[h] = false;
        }
        else {
            // Expected result from the byte map: best fit, lowest offset on a tie
            size_t need = al(sizes[h]), bestOff = 0, bestLen = 0;
            bool found = false;
            for ( size_t i = 0; i < pool; ) {
                if ( owner[i] != -1 ) { i++; continue; }
                size_t j = i;
                while ( j < pool && owner[j] == -1 ) j++;
                if ( j - i >= need && ( !found || j - i < bestLen ) ) { found = true; bestOff = i; bestLen = j - i; }
                i = j;
            }
            SB::Status st;
            uint8_t* p = (uint8_t*) sb.acquire((SB::Handle) h, &st);
            if ( !found ) {
                if ( p != NULL || st != SB::SB_ERR_NO_SPACE ) badPlace++;
                noSpace++;
                continue;
            }
            if ( p == NULL ) { badPlace++; continue; }
            if ( base == NULL ) base = p - bestOff;
            if ( p != base + bestOff || (uintptr_t) p % A != 0 ) badPlace++;
            for ( size_t i = 0; i < sizes[h]; i++ ) if ( p[i] != 0 ) { badZero++; break; }
            size_t off = (size_t) (p - base);
            for ( size_t i = 0; i < need && off + i < pool; i++ ) {
                if ( owner[off + i] != -1 ) badPlace++;
                owner[off + i] = h;
            }
            memset(p, h * 37 + 1, sizes[h]);
            held[h] = true;
            offset[h] = off;
            acquired++;
        }

        // Statistics agree with the byte map
        size_t freeCount = 0, run = 0, longest = 0;
        for ( size_t i = 0; i < pool; i++ ) {
            if ( owner[i] == -1 ) { freeCount++; run++; if ( run > longest ) longest = run; }
            else run = 0;
        }
        if ( sb.freeBytes() != freeCount || sb.largestFree() != longest ) badStats++;
    }
    CHECK_EQ(badData, 0);
    CHECK_EQ(badPlace, 0);
    CHECK_EQ(badZero, 0);
    CHECK_EQ(badStats, 0);
    CHECK(acquired > 1000);
    CHECK(noSpace > 100);                                   // the pool was often full or fragmented

    for ( int h = 0; h < N; h++ ) if ( held[h] ) sb.release((SB::Handle) h);
    CHECK_EQ(sb.end(), SB::SB_OK);
}

// ---------------------------------------------------------------------------------------
// Tasks: three threads acquire, fill, verify and release their own buffers and one
// shared buffer they compete for. A fourth thread reads every query and registers a
// buffer while the others run. Run under TSan by the "tsan" variant. Not run in the
// "counted" variant (no real lock).

#if !defined(TEST_COUNTED_CRITICAL)
static void test_threads() {
    const int T = 3;
    const int PER = 2;
    SB sb;
    static char names[T * PER][16];
    for ( int i = 0; i < T * PER; i++ ) {
        snprintf(names[i], sizeof(names[i]), "t%d", i);
        CHECK_EQ(sb.registerBuffer(names[i], (size_t) (100 + 90 * i)), i);
    }
    const SB::Handle shared = sb.registerBuffer("shared", 300);
    CHECK_EQ(shared, T * PER);
    CHECK_EQ(sb.begin(1500), SB::SB_OK);                   // less than the sum: requests collide

    std::atomic<long> ok(0), busy(0), corrupt(0), sharedOk(0);
    std::atomic<int> lateHandle(-100);
    std::atomic<bool> stop(false);
    std::vector<std::thread> threads;
    for ( int t = 0; t < T; t++ ) {
        threads.push_back(std::thread([&, t]() {
            for ( int n = 0; n < 20000; n++ ) {
                SB::Handle h = ( n % 3 == 2 ) ? shared : (SB::Handle) (t * PER + n % PER);
                size_t len = sb.size(h);
                uint8_t* p = (uint8_t*) sb.acquire(h);
                if ( p == NULL ) { busy++; continue; }
                if ( sb.pointer(h) != p || !sb.isHeld(h) ) corrupt++;
                for ( size_t i = 0; i < len; i++ ) if ( p[i] != 0 ) { corrupt++; break; }
                memset(p, 0x10 + t, len);
                for ( size_t i = 0; i < len; i++ ) if ( p[i] != 0x10 + t ) { corrupt++; break; }
                if ( sb.release(h) != SB::SB_OK ) corrupt++;
                ok++;
                if ( h == shared ) sharedOk++;
            }
        }));
    }
    std::thread reader([&]() {
        long n = 0;
        while ( !stop ) {
            if ( sb.freeBytes() > sb.poolSize() ) corrupt++;
            if ( sb.largestFree() > sb.poolSize() ) corrupt++;
            (void) sb.pointer(shared);
            (void) sb.isHeld(shared);
            if ( sb.size(shared) != 300 || sb.name(shared) == NULL ) corrupt++;
            if ( sb.count() < T * PER + 1 ) corrupt++;
            if ( ++n == 1000 ) lateHandle = sb.registerBuffer("late", 50);   // while the others run
        }
    });
    for ( size_t i = 0; i < threads.size(); i++ ) threads[i].join();
    stop = true;
    reader.join();

    CHECK_EQ(corrupt.load(), 0);
    CHECK(ok.load() > 0);
    CHECK(sharedOk.load() > 0);
    CHECK_EQ(lateHandle.load(), T * PER + 1);
    printf("   threads: %ld acquired (%ld of the shared buffer), %ld refused\n",
           ok.load(), sharedOk.load(), busy.load());
    CHECK_EQ(sb.freeBytes(), sb.poolSize());
    CHECK_EQ(sb.end(), SB::SB_OK);
}
#endif

// ---------------------------------------------------------------------------------------
// Counted variant only: critical sections per call, and begin() racing another task

#if defined(TEST_COUNTED_CRITICAL)
#define ENTERS(expr, n) do { long e0_ = g_enters; (void) (expr); CHECK_EQ(g_enters - e0_, n); } while (0)

// Every call that touches the table enters exactly the expected number of sections
static void test_lock_per_call() {
    alignas(64) static uint8_t storage[128];
    SB sb;
    ENTERS(sb.registerBuffer("a", 64), 1);
    ENTERS(sb.begin(), 2);                                  // before and after malloc()
    ENTERS(sb.acquire(0), 1);
    ENTERS(sb.pointer(0), 1);
    ENTERS(sb.isHeld(0), 1);
    ENTERS(sb.size(0), 1);
    ENTERS(sb.name(0), 1);
    ENTERS(sb.count(), 1);
    ENTERS(sb.started(), 1);
    ENTERS(sb.poolSize(), 1);
    ENTERS(sb.requiredSize(), 1);
    ENTERS(sb.freeBytes(), 1);
    ENTERS(sb.largestFree(), 1);
    g_lines.clear();
    ENTERS(sb.dump(collectLine), 2);                        // the header, then one per buffer
    ENTERS(sb.release(0), 1);
    ENTERS(sb.end(), 1);
    ENTERS(sb.begin(storage, sizeof(storage)), 2);         // before and after the clear
    ENTERS(sb.end(), 1);
    ENTERS(SB::statusText(SB::SB_ERR_HELD), 0);
}

static SB*        g_raceTarget = NULL;
static int        g_raceResult = -100;
alignas(64) static uint8_t g_raceStorage[4096];

static void raceRegisterLarger() { g_raceResult = g_raceTarget->registerBuffer("big", 4000); }
static void raceRegisterHuge()   { g_raceResult = g_raceTarget->registerBuffer("huge", 8000); }
static void raceBeginStorage()   { g_raceResult = g_raceTarget->begin(g_raceStorage, sizeof(g_raceStorage)); }
static void raceBeginHeap()      { g_raceResult = g_raceTarget->begin(); }

// Another task acts between the two critical sections of begin(size_t)
static void test_begin_race() {
    SB sb;
    g_raceTarget = &sb;
    CHECK_EQ(sb.registerBuffer("a", 100), 0);

    // A larger registration arrives while begin() is in malloc(): the pool would be too small
    g_inject = raceRegisterLarger;
    g_injectAt = g_enters + 1;
    CHECK_EQ(sb.begin(), SB::SB_ERR_TOO_LARGE);
    CHECK_EQ(g_raceResult, 1);
    CHECK(!sb.started());                                   // and the block was freed (LeakSanitizer)
    CHECK_EQ(sb.begin(), SB::SB_OK);                        // a retry sizes the pool for "big"
    CHECK_EQ(sb.poolSize(), al(4000));
    CHECK_EQ(sb.end(), SB::SB_OK);

    // Another begin() wins while this one is in malloc()
    g_inject = raceBeginStorage;
    g_injectAt = g_enters + 1;
    CHECK_EQ(sb.begin(), SB::SB_ERR_STARTED);
    CHECK_EQ(g_raceResult, SB::SB_OK);
    CHECK_EQ(sb.poolSize(), sizeof(g_raceStorage));        // the winner's pool stays
    CHECK(sb.acquire(1) == g_raceStorage);
    CHECK_EQ(sb.release(1), SB::SB_OK);
    CHECK_EQ(sb.end(), SB::SB_OK);

    // The same two races between the two critical sections of begin(storage)
    g_inject = raceRegisterHuge;
    g_injectAt = g_enters + 1;
    CHECK_EQ(sb.begin(g_raceStorage, sizeof(g_raceStorage)), SB::SB_ERR_TOO_LARGE);
    CHECK_EQ(g_raceResult, 2);
    CHECK(!sb.started());

    g_inject = raceBeginHeap;
    g_injectAt = g_enters + 1;
    alignas(64) static uint8_t other[8192];
    CHECK_EQ(sb.begin(other, sizeof(other)), SB::SB_ERR_STARTED);
    CHECK_EQ(g_raceResult, SB::SB_OK);
    CHECK_EQ(sb.poolSize(), al(8000));                     // the heap pool won
    CHECK_EQ(sb.end(), SB::SB_OK);
    g_inject = NULL;
}
#endif

// ---------------------------------------------------------------------------------------

extern SB::Handle g_earlyHandle;                           // early_user.cpp

static void test_static_init() {
    CHECK_EQ(g_earlyHandle, 0);
    CHECK_EQ(SharedBuf.count(), 1);
    CHECK_STR(SharedBuf.name(0), "early");
    CHECK_EQ(SharedBuf.begin(), SB::SB_OK);
    CHECK(SharedBuf.acquire(0) != NULL);
    CHECK_EQ(SharedBuf.release(0), SB::SB_OK);
    CHECK_EQ(SharedBuf.end(), SB::SB_OK);
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);                       // keep FAIL lines when a sanitizer exits early
    test_static_init();
    test_register();
    test_begin_heap();
    test_begin_storage();
    test_acquire_release();
    test_dfu_and_calc();
    test_byte_range();
    test_readme_example();
    test_best_fit();
    test_fragmentation();
    test_late_registration();
    test_zero_on_acquire();
    test_begin_clears();
    test_end();
    test_dump();
    test_status_text();
    test_random();
#if defined(TEST_COUNTED_CRITICAL)
    test_lock_per_call();
    test_begin_race();
    CHECK(g_enters > 0);
    CHECK_EQ(g_enters, g_exits);
    CHECK_EQ(g_depth, 0);
    CHECK_EQ(g_nested, 0);
    printf("   critical sections: %ld entered, %ld left, %d nested\n", g_enters, g_exits, g_nested);
#else
    test_threads();
#endif
    printf("   %d checks, %d failures (SHARED_BUFFER_ALIGN %u, SHARED_BUFFER_MAX_BUFFERS %u)\n",
           g_checks, g_failures, (unsigned) SHARED_BUFFER_ALIGN, (unsigned) SHARED_BUFFER_MAX_BUFFERS);
    return g_failures == 0 ? 0 : 1;
}
