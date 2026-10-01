#pragma once

#include <stdint.h>
#include <stddef.h>

/** @brief Library version as a number: major * 10000 + minor * 100 + patch */
#define SHARED_BUFFER_VERSION           10000
/** @brief Library version as a string */
#define SHARED_BUFFER_VERSION_STRING    "1.0.0"

// The two settings below may be set by the build, e.g. -D SHARED_BUFFER_MAX_BUFFERS=16.
// Every file of one firmware must see the same values.

#ifndef SHARED_BUFFER_MAX_BUFFERS
/** @brief Number of buffers one SharedBuffer object can register (1..127) */
#define SHARED_BUFFER_MAX_BUFFERS   8
#endif

#ifndef SHARED_BUFFER_ALIGN
#if defined(__AVR__)
/** @brief Alignment of every buffer, bytes (a power of two) */
#define SHARED_BUFFER_ALIGN         1
#else
#define SHARED_BUFFER_ALIGN         8
#endif
#endif

#if SHARED_BUFFER_MAX_BUFFERS < 1 || SHARED_BUFFER_MAX_BUFFERS > 127
#error "SHARED_BUFFER_MAX_BUFFERS must be 1..127"
#endif
#if SHARED_BUFFER_ALIGN < 1 || (SHARED_BUFFER_ALIGN & (SHARED_BUFFER_ALIGN - 1)) != 0
#error "SHARED_BUFFER_ALIGN must be a power of two"
#endif

/**
 * @brief One memory pool shared by buffers that are not used at the same time
 *
 * Each user registers a named buffer and its size once and receives a handle.
 * begin() allocates the pool once: the largest registered size, or more when asked.
 * acquire() places the buffer in the smallest free gap of the pool that fits it and
 * marks those bytes as held until release(). A request that no free gap can hold
 * fails with SB_ERR_NO_SPACE. The pool is never resized and nothing is allocated
 * after begin().
 *
 * All calls may be made from several tasks and from interrupts, except begin() and
 * end(), which call malloc() and free().
 */
class SharedBuffer {
    public:
        /** @brief Buffer handle returned by registerBuffer(): 0 or more, or a negative Status */
        typedef int8_t Handle;

        /** @brief Result codes. Errors are negative, so they fit in a Handle. */
        enum Status : int8_t {
            SB_OK               =   0,  ///< Success
            SB_ERR_ARGUMENT     =  -1,  ///< NULL or empty name, size 0, unknown handle, NULL storage or less than one alignment unit of it, nothing to size the pool by
            SB_ERR_DUPLICATE    =  -2,  ///< A buffer with this name is already registered
            SB_ERR_FULL         =  -3,  ///< SHARED_BUFFER_MAX_BUFFERS buffers are already registered
            SB_ERR_TOO_LARGE    =  -4,  ///< Size cannot be rounded, larger than the pool, or a registered buffer does not fit the storage
            SB_ERR_NO_MEMORY    =  -5,  ///< malloc() failed in begin()
            SB_ERR_NOT_STARTED  =  -6,  ///< The pool does not exist yet (begin() not called)
            SB_ERR_STARTED      =  -7,  ///< begin() was already called
            SB_ERR_HELD         =  -8,  ///< The buffer is already held (acquire), or a buffer is held (end)
            SB_ERR_NOT_HELD     =  -9,  ///< The buffer is not held (release)
            SB_ERR_NO_SPACE     = -10   ///< No free gap of the pool fits the buffer at this moment
        };

        /** @brief Line writer for dump(), for example a lambda that calls Serial.println() */
        typedef void (*LineWriter)(const char* line);

        /**
         * @brief Constructor. No memory is allocated until begin().
         * @note The constructor is constexpr and the destructor is trivial, so a global
         *       object is initialized at compile time and is usable from other global
         *       constructors. The destructor does not free the pool: call end().
         */
        constexpr SharedBuffer()
            : m_entries{}, m_order{}, m_pool(NULL), m_raw(NULL), m_poolSize(0),
              m_largest(0), m_count(0), m_heldCount(0) {}

        // Not copyable: a copy would share the pool, hand out the same bytes twice,
        // and free it twice in end(). Pass a reference or a pointer instead.
        SharedBuffer(const SharedBuffer&) = delete;
        SharedBuffer& operator=(const SharedBuffer&) = delete;

        /**
         * @brief Register a buffer for later use
         * @param name Unique name. The pointer is stored, not copied: pass a string that
         *        lives as long as the object (a string literal).
         * @param size Buffer size in bytes, 1 or more
         * @return Handle (0 or more), or SB_ERR_ARGUMENT, SB_ERR_DUPLICATE, SB_ERR_FULL,
         *         SB_ERR_TOO_LARGE (size cannot be rounded, or after begin(): larger than the pool)
         * @note Allowed before and after begin(). Registration cannot be undone.
         */
        Handle registerBuffer(const char* name, size_t size);

        /**
         * @brief Allocate the pool with malloc(), once
         * @param minPoolSize Smallest pool size wanted, bytes. The pool is the larger of
         *        this and the largest registered buffer (both rounded to SHARED_BUFFER_ALIGN).
         * @return SB_OK, SB_ERR_STARTED, SB_ERR_ARGUMENT (nothing registered and
         *         minPoolSize 0), SB_ERR_TOO_LARGE, SB_ERR_NO_MEMORY
         * @note Calls malloc(): not from an interrupt.
         */
        Status begin(size_t minPoolSize = 0);

        /**
         * @brief Use caller memory as the pool
         * @param storage Memory that outlives the pool, for example a static array.
         *        Declare it alignas(SHARED_BUFFER_ALIGN), or up to SHARED_BUFFER_ALIGN - 1
         *        bytes at its start are skipped.
         * @param storageSize Size of storage, bytes. All of it (after alignment) becomes the pool.
         * @return SB_OK, SB_ERR_STARTED, SB_ERR_ARGUMENT (NULL storage, or less than
         *         SHARED_BUFFER_ALIGN bytes left after alignment), SB_ERR_TOO_LARGE (a
         *         registered buffer does not fit)
         */
        Status begin(void* storage, size_t storageSize);

        /**
         * @brief Release the pool. Registrations stay, and begin() may be called again.
         * @return SB_OK, SB_ERR_NOT_STARTED, SB_ERR_HELD (a buffer is still held)
         * @note Frees heap memory from begin(size_t). Calls free(): not from an interrupt.
         */
        Status end();

        /**
         * @brief Take a registered buffer for exclusive use
         * @param handle Handle from registerBuffer()
         * @param status Optional: receives SB_OK, SB_ERR_ARGUMENT, SB_ERR_NOT_STARTED,
         *        SB_ERR_HELD or SB_ERR_NO_SPACE
         * @return Pointer to size(handle) bytes aligned to SHARED_BUFFER_ALIGN, or NULL
         * @note Never waits. The memory is not cleared: it holds whatever the previous
         *       user of those bytes left.
         */
        void* acquire(Handle handle, Status* status = NULL);

        /**
         * @brief Give a held buffer back to the pool
         * @return SB_OK, SB_ERR_ARGUMENT, SB_ERR_NOT_HELD
         * @note The pointer from acquire() must not be used afterwards.
         */
        Status release(Handle handle);

        /** @brief Pointer of a held buffer, or NULL when the buffer is not held or the handle is unknown */
        void* pointer(Handle handle) const;

        /** @brief True when the buffer is held */
        bool isHeld(Handle handle) const;

        /** @brief Registered size in bytes, or 0 for an unknown handle */
        size_t size(Handle handle) const;

        /** @brief Registered name, or NULL for an unknown handle */
        const char* name(Handle handle) const;

        /** @brief Number of registered buffers */
        uint8_t count() const;

        /** @brief True between a successful begin() and end() */
        bool started() const;

        /** @brief Pool size in bytes, 0 before begin() */
        size_t poolSize() const;

        /** @brief Pool size begin() would allocate with minPoolSize 0: the largest registered buffer, rounded */
        size_t requiredSize() const;

        /** @brief Bytes of the pool not held by any buffer, 0 before begin() */
        size_t freeBytes() const;

        /** @brief Largest free gap in bytes: the largest buffer acquire() can place now, 0 before begin() */
        size_t largestFree() const;

        /**
         * @brief Write the pool state and every registration, one line per call
         * @param writeLine Called once per line, outside the critical section. NULL is ignored.
         * @note Each line is a snapshot. Lines may disagree when other tasks acquire or
         *       release meanwhile.
         */
        void dump(LineWriter writeLine) const;

        /** @brief Short text for a status code, for example "no space" */
        static const char* statusText(int status);

    private:
        struct Entry {
            const char* name;   ///< Registered name, not copied
            size_t      size;   ///< Registered size, bytes
            size_t      offset; ///< Offset in the pool while held
            bool        held;   ///< True between acquire() and release()
        };

        static constexpr size_t alignUp(size_t n) {
            return (n + (size_t) (SHARED_BUFFER_ALIGN - 1)) & ~((size_t) (SHARED_BUFFER_ALIGN - 1));
        }
        bool   valid(Handle handle) const { return handle >= 0 && handle < (Handle) m_count; }
        size_t freeBytesLocked() const;
        size_t largestFreeLocked() const;

        Entry    m_entries[SHARED_BUFFER_MAX_BUFFERS]; ///< Registrations, index = handle
        uint8_t  m_order[SHARED_BUFFER_MAX_BUFFERS];   ///< Handles of held buffers, sorted by offset
        uint8_t* m_pool;        ///< Aligned pool start, NULL before begin()
        void*    m_raw;         ///< Pointer from malloc() to free in end(), NULL for caller storage
        size_t   m_poolSize;    ///< Pool size, bytes
        size_t   m_largest;     ///< Largest registered size, rounded
        uint8_t  m_count;       ///< Number of registrations
        uint8_t  m_heldCount;   ///< Number of held buffers (used length of m_order)
};

/** @brief Global pool shared by all code in the firmware */
extern SharedBuffer SharedBuf;
