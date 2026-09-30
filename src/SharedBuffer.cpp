#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "SharedBuffer.h"

// ---------------------------------------------------------------------------------------
// Critical section around every read or change of the registration table.
// It is held for O(SHARED_BUFFER_MAX_BUFFERS) steps and never around malloc(), free()
// or a user callback. The build may replace it by defining both
// SHARED_BUFFER_CRITICAL_ENTER() and SHARED_BUFFER_CRITICAL_EXIT().
// ---------------------------------------------------------------------------------------

#if defined(SHARED_BUFFER_CRITICAL_ENTER) && defined(SHARED_BUFFER_CRITICAL_EXIT)

namespace {
struct Critical {
    Critical()  { SHARED_BUFFER_CRITICAL_ENTER(); }
    ~Critical() { SHARED_BUFFER_CRITICAL_EXIT(); }
};
}

#elif defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)

// Spinlock plus interrupts off on this core. The _SAFE forms work in tasks and in ISRs.
#include "freertos/FreeRTOS.h"

namespace {
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
struct Critical {
    Critical()  { portENTER_CRITICAL_SAFE(&s_mux); }
    ~Critical() { portEXIT_CRITICAL_SAFE(&s_mux); }
};
}

#elif defined(__AVR__)

#include <avr/io.h>
#include <avr/interrupt.h>

namespace {
struct Critical {
    uint8_t sreg;
    Critical() : sreg(SREG) { cli(); }
    ~Critical() { SREG = sreg; __asm__ __volatile__ ("" ::: "memory"); }
};
}

#elif defined(ARDUINO_ARCH_RP2040)

#error "SharedBuffer: RP2040 has two cores. Define SHARED_BUFFER_CRITICAL_ENTER() and SHARED_BUFFER_CRITICAL_EXIT() in the build."

#elif defined(__ARM_ARCH_6M__) || defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__) || \
      defined(__ARM_ARCH_8M_BASE__) || defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_8_1M_MAIN__)

// Single-core Cortex-M: save PRIMASK, disable interrupts, restore PRIMASK
namespace {
struct Critical {
    uint32_t primask;
    Critical() {
        __asm__ __volatile__ ("mrs %0, primask" : "=r" (primask));
        __asm__ __volatile__ ("cpsid i" ::: "memory");
    }
    ~Critical() {
        __asm__ __volatile__ ("msr primask, %0" :: "r" (primask) : "memory");
    }
};
}

#elif defined(__linux__) || defined(__APPLE__) || defined(_WIN32)

// Host builds (unit tests, simulators): spinlock shared by all objects
#include <atomic>

namespace {
std::atomic_flag s_lock = ATOMIC_FLAG_INIT;
struct Critical {
    Critical()  { while ( s_lock.test_and_set(std::memory_order_acquire) ) {} }
    ~Critical() { s_lock.clear(std::memory_order_release); }
};
}

#else

#error "SharedBuffer: no built-in critical section for this platform. Define SHARED_BUFFER_CRITICAL_ENTER() and SHARED_BUFFER_CRITICAL_EXIT() in the build."

#endif

// ---------------------------------------------------------------------------------------

namespace {
// Bytes to skip from p to the next SHARED_BUFFER_ALIGN boundary
inline size_t alignSkip(const void* p) {
    return (size_t) (0 - (uintptr_t) p) & (size_t) (SHARED_BUFFER_ALIGN - 1);
}
}

SharedBuffer::Handle SharedBuffer::registerBuffer(const char* name, size_t size) {
    if ( name == NULL || name[0] == '\0' || size == 0 ) return SB_ERR_ARGUMENT;
    if ( size > (size_t) -1 - (size_t) (SHARED_BUFFER_ALIGN - 1) ) return SB_ERR_TOO_LARGE;
    size_t need = alignUp(size);

    Critical cs;
    for ( uint8_t i = 0; i < m_count; i++ ) {
        if ( strcmp(m_entries[i].name, name) == 0 ) return SB_ERR_DUPLICATE;
    }
    if ( m_count >= SHARED_BUFFER_MAX_BUFFERS ) return SB_ERR_FULL;
    if ( m_pool != NULL && need > m_poolSize ) return SB_ERR_TOO_LARGE;

    Entry& e = m_entries[m_count];
    e.name   = name;
    e.size   = size;
    e.offset = 0;
    e.held   = false;
    if ( need > m_largest ) m_largest = need;
    return (Handle) m_count++;
}

SharedBuffer::Status SharedBuffer::begin(size_t minPoolSize) {
    if ( minPoolSize > (size_t) -1 - (size_t) (SHARED_BUFFER_ALIGN - 1) ) return SB_ERR_TOO_LARGE;
    size_t size;
    {
        Critical cs;
        if ( m_pool != NULL ) return SB_ERR_STARTED;
        size = alignUp(minPoolSize);
        if ( m_largest > size ) size = m_largest;
    }
    if ( size == 0 ) return SB_ERR_ARGUMENT;

    // Room to align the start: malloc() alignment may be smaller than SHARED_BUFFER_ALIGN.
    // size is at most SIZE_MAX - (ALIGN - 1), so the sum does not overflow.
    void* raw = malloc(size + (SHARED_BUFFER_ALIGN - 1));
    if ( raw == NULL ) return SB_ERR_NO_MEMORY;

    Status result = SB_OK;
    {
        Critical cs;
        if ( m_pool != NULL ) {
            result = SB_ERR_STARTED;            // another task called begin() meanwhile
        }
        else if ( m_largest > size ) {
            result = SB_ERR_TOO_LARGE;          // a larger buffer was registered meanwhile
        }
        else {
            m_raw      = raw;
            m_pool     = (uint8_t*) raw + alignSkip(raw);
            m_poolSize = size;
        }
    }
    if ( result != SB_OK ) free(raw);
    return result;
}

SharedBuffer::Status SharedBuffer::begin(void* storage, size_t storageSize) {
    if ( storage == NULL ) return SB_ERR_ARGUMENT;
    size_t skip = alignSkip(storage);
    if ( storageSize <= skip ) return SB_ERR_ARGUMENT;
    size_t size = (storageSize - skip) & ~((size_t) (SHARED_BUFFER_ALIGN - 1));
    if ( size == 0 ) return SB_ERR_ARGUMENT;

    Critical cs;
    if ( m_pool != NULL ) return SB_ERR_STARTED;
    if ( m_largest > size ) return SB_ERR_TOO_LARGE;
    m_raw      = NULL;
    m_pool     = (uint8_t*) storage + skip;
    m_poolSize = size;
    return SB_OK;
}

SharedBuffer::Status SharedBuffer::end() {
    void* raw;
    {
        Critical cs;
        if ( m_pool == NULL ) return SB_ERR_NOT_STARTED;
        if ( m_heldCount > 0 ) return SB_ERR_HELD;
        raw        = m_raw;
        m_raw      = NULL;
        m_pool     = NULL;
        m_poolSize = 0;
    }
    free(raw);
    return SB_OK;
}

void* SharedBuffer::acquire(Handle handle, Status* status) {
    Status result;
    void*  p = NULL;
    {
        Critical cs;
        if ( !valid(handle) )                 result = SB_ERR_ARGUMENT;
        else if ( m_pool == NULL )            result = SB_ERR_NOT_STARTED;
        else if ( m_entries[handle].held )    result = SB_ERR_HELD;
        else {
            // Best fit: the smallest gap that fits, the lowest offset on a tie.
            // Gap i lies before held buffer m_order[i]; gap m_heldCount is the tail.
            size_t need    = alignUp(m_entries[handle].size);
            size_t prevEnd = 0;
            size_t bestGap = 0;
            size_t bestOff = 0;
            int    bestPos = -1;
            for ( uint8_t i = 0; i <= m_heldCount; i++ ) {
                size_t gapEnd = ( i < m_heldCount ) ? m_entries[m_order[i]].offset : m_poolSize;
                size_t gap    = gapEnd - prevEnd;
                if ( gap >= need && ( bestPos < 0 || gap < bestGap ) ) {
                    bestPos = i;
                    bestGap = gap;
                    bestOff = prevEnd;
                }
                if ( i < m_heldCount ) {
                    const Entry& h = m_entries[m_order[i]];
                    prevEnd = h.offset + alignUp(h.size);
                }
            }
            if ( bestPos < 0 ) {
                result = SB_ERR_NO_SPACE;
            }
            else {
                for ( int j = m_heldCount; j > bestPos; j-- ) m_order[j] = m_order[j - 1];
                m_order[bestPos] = (uint8_t) handle;
                m_heldCount++;
                m_entries[handle].offset = bestOff;
                m_entries[handle].held   = true;
                p = m_pool + bestOff;
                result = SB_OK;
            }
        }
    }
    if ( status != NULL ) *status = result;
    return p;
}

SharedBuffer::Status SharedBuffer::release(Handle handle) {
    Critical cs;
    if ( !valid(handle) ) return SB_ERR_ARGUMENT;
    if ( !m_entries[handle].held ) return SB_ERR_NOT_HELD;
    uint8_t i = 0;
    while ( i < m_heldCount && m_order[i] != (uint8_t) handle ) i++;
    if ( i < m_heldCount ) {                            // always true: a held buffer is in m_order
        for ( ; i + 1 < m_heldCount; i++ ) m_order[i] = m_order[i + 1];
        m_heldCount--;
    }
    m_entries[handle].held = false;
    return SB_OK;
}

void* SharedBuffer::pointer(Handle handle) const {
    Critical cs;
    if ( !valid(handle) || !m_entries[handle].held ) return NULL;
    return m_pool + m_entries[handle].offset;
}

bool SharedBuffer::isHeld(Handle handle) const {
    Critical cs;
    return valid(handle) && m_entries[handle].held;
}

size_t SharedBuffer::size(Handle handle) const {
    Critical cs;
    return valid(handle) ? m_entries[handle].size : 0;
}

const char* SharedBuffer::name(Handle handle) const {
    Critical cs;
    return valid(handle) ? m_entries[handle].name : NULL;
}

uint8_t SharedBuffer::count() const {
    Critical cs;
    return m_count;
}

bool SharedBuffer::started() const {
    Critical cs;
    return m_pool != NULL;
}

size_t SharedBuffer::poolSize() const {
    Critical cs;
    return m_poolSize;
}

size_t SharedBuffer::requiredSize() const {
    Critical cs;
    return m_largest;
}

size_t SharedBuffer::freeBytes() const {
    Critical cs;
    return freeBytesLocked();
}

size_t SharedBuffer::largestFree() const {
    Critical cs;
    return largestFreeLocked();
}

size_t SharedBuffer::freeBytesLocked() const {
    size_t used = 0;
    for ( uint8_t i = 0; i < m_heldCount; i++ ) used += alignUp(m_entries[m_order[i]].size);
    return m_poolSize - used;
}

size_t SharedBuffer::largestFreeLocked() const {
    size_t prevEnd = 0;
    size_t largest = 0;
    for ( uint8_t i = 0; i <= m_heldCount; i++ ) {
        size_t gapEnd = ( i < m_heldCount ) ? m_entries[m_order[i]].offset : m_poolSize;
        if ( gapEnd - prevEnd > largest ) largest = gapEnd - prevEnd;
        if ( i < m_heldCount ) {
            const Entry& h = m_entries[m_order[i]];
            prevEnd = h.offset + alignUp(h.size);
        }
    }
    return largest;
}

void SharedBuffer::dump(LineWriter writeLine) const {
    if ( writeLine == NULL ) return;
    char line[96];

    bool    isStarted;
    size_t  pool, freeNow, largest, required;
    uint8_t n;
    {
        Critical cs;
        isStarted = ( m_pool != NULL );
        pool      = m_poolSize;
        freeNow   = freeBytesLocked();
        largest   = largestFreeLocked();
        required  = m_largest;
        n         = m_count;
    }
    if ( isStarted ) {
        snprintf(line, sizeof(line), "SharedBuffer: pool %lu bytes, %lu free, largest gap %lu, %u of %u buffers",
                 (unsigned long) pool, (unsigned long) freeNow, (unsigned long) largest,
                 (unsigned) n, (unsigned) SHARED_BUFFER_MAX_BUFFERS);
    }
    else {
        snprintf(line, sizeof(line), "SharedBuffer: not started, %lu bytes required, %u of %u buffers",
                 (unsigned long) required, (unsigned) n, (unsigned) SHARED_BUFFER_MAX_BUFFERS);
    }
    writeLine(line);

    for ( uint8_t i = 0; i < n; i++ ) {
        Entry e;
        {
            Critical cs;
            e = m_entries[i];
        }
        if ( e.held ) {
            snprintf(line, sizeof(line), "  #%u %s: %lu bytes, held at offset %lu",
                     (unsigned) i, e.name, (unsigned long) e.size, (unsigned long) e.offset);
        }
        else {
            snprintf(line, sizeof(line), "  #%u %s: %lu bytes, not held",
                     (unsigned) i, e.name, (unsigned long) e.size);
        }
        writeLine(line);
    }
}

const char* SharedBuffer::statusText(int status) {
    switch ( status ) {
        case SB_OK:              return "ok";
        case SB_ERR_ARGUMENT:    return "invalid argument";
        case SB_ERR_DUPLICATE:   return "duplicate name";
        case SB_ERR_FULL:        return "registration table full";
        case SB_ERR_TOO_LARGE:   return "too large";
        case SB_ERR_NO_MEMORY:   return "out of memory";
        case SB_ERR_NOT_STARTED: return "not started";
        case SB_ERR_STARTED:     return "already started";
        case SB_ERR_HELD:        return "held";
        case SB_ERR_NOT_HELD:    return "not held";
        case SB_ERR_NO_SPACE:    return "no space";
        default:                 return ( status > 0 ) ? "ok" : "unknown status";   // a handle
    }
}

// Constant-initialized (constexpr constructor, trivial destructor): no global constructor runs for it
SharedBuffer SharedBuf;
