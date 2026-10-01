# SharedBuffer

Version: 1.0.1

One memory pool, allocated once, shared by buffers that are not used at the same time.

## Description

Many firmwares hold large buffers that are never used together. A firmware update (DFU) may need two 4 KB buffers and runs only during an update. A calculation may need a 16 KB buffer and runs only in normal operation. Separate buffers take 24 KB of RAM. With SharedBuffer they take 16 KB.

Each user registers a named buffer and its size once and receives a handle. `begin()` allocates the pool once, from the heap or from a static array. The pool is as large as the largest registered buffer, or larger when asked. `acquire()` places the buffer in a free gap of the pool, clears it to 0, and holds those bytes until `release()`. When no free gap fits, `acquire()` fails at once with a status code and never waits. Nothing is allocated after `begin()`.

## Features

- Registration with a unique name and a size. The name is checked for duplicates and appears in the diagnostic dump.
- One pool per `SharedBuffer` object: heap memory allocated once by `begin()`, or caller memory passed to `begin(storage, size)`. A global object `SharedBuf` is provided.
- Pool size: the largest registration, or a larger minimum passed to `begin()`, so that several buffers fit at the same time.
- Registration after `begin()` is accepted when the buffer fits the pool.
- Placement at `acquire()`: the smallest free gap that fits (best fit), the lowest offset on a tie. Buffers are aligned to `SHARED_BUFFER_ALIGN`.
- Exclusive use: a held buffer cannot be acquired again, and its bytes are not given to another buffer until `release()`.
- Zeroed memory: `begin()` clears the pool, and every granted buffer is cleared to 0. No data passes from one user to the next.
- Built-in critical section: calls may come from several tasks and from interrupts (see Thread and interrupt safety).
- Status codes for every failure, `statusText()` for printing them, and `dump()` for the pool state.
- No dependency on the Arduino API. Plain C++11.

## Supported platforms

| Arduino architecture | PlatformIO platform | Built in CI |
|---|---|---|
| `avr` | `atmelavr` | Arduino Uno |
| `megaavr` | `atmelmegaavr` | Arduino Nano Every |
| `samd` | `atmelsam` | Arduino MKR Zero |
| `esp32` | `espressif32` | ESP32 and ESP32-C3 (latest core), ESP32 (core 2.0.17) |
| `arm-ble` (n-able core, NimBLE) | `n-able` | nRF52840 |
| host | `native` | Linux, g++ and clang++ |

- Other single-core Cortex-M targets (for example STM32 or Arduino UNO R4) compile with the Cortex-M critical section, but they are not built in CI and are not listed in the manifests.
- RP2040 and RP2350 (Arduino-Pico, Arduino Mbed and the pico-sdk), ESP8266 and every other target stop the build with `#error`, unless the build defines `SHARED_BUFFER_CRITICAL_ENTER()` and `SHARED_BUFFER_CRITICAL_EXIT()`.
- The Adafruit nRF52 core is not listed. It runs the Nordic SoftDevice, and it is not verified that a short interrupt-off section is safe with it.

## Installation

- Arduino IDE: download the repository as a ZIP file and use Sketch > Include Library > Add .ZIP Library, or clone it into your `libraries` folder.
- PlatformIO: add it to `platformio.ini`:

```ini
lib_deps = https://github.com/arkhipenko/SharedBuffer.git#v1.0.1
```

The library is not published in the Arduino Library Manager or the PlatformIO registry.

## Quick start

```cpp
#include <SharedBuffer.h>

SharedBuffer::Handle calcBuffer;
SharedBuffer::Handle dfuBufferA;
SharedBuffer::Handle dfuBufferB;

void setup() {
  Serial.begin(115200);

  // Register every buffer once. A negative handle is an error code.
  calcBuffer = SharedBuf.registerBuffer("calc", 16384);
  dfuBufferA = SharedBuf.registerBuffer("dfuA", 4096);
  dfuBufferB = SharedBuf.registerBuffer("dfuB", 4096);

  // Allocate the pool once: 16384 bytes, the largest registration
  SharedBuffer::Status status = SharedBuf.begin();
  if ( status != SharedBuffer::SB_OK ) {
    Serial.println(SharedBuffer::statusText(status));
    return;
  }

  // Normal operation
  float* samples = (float*) SharedBuf.acquire(calcBuffer, &status);
  // ... use samples ...
  SharedBuf.release(calcBuffer);

  // Firmware update: both DFU buffers fit in the pool at the same time
  uint8_t* a = (uint8_t*) SharedBuf.acquire(dfuBufferA);
  uint8_t* b = (uint8_t*) SharedBuf.acquire(dfuBufferB);
  // While a or b is held, acquire(calcBuffer) fails with SB_ERR_NO_SPACE
  SharedBuf.release(dfuBufferA);
  SharedBuf.release(dfuBufferB);
}

void loop() {
}
```

Examples in `examples/`:

- Basic: register, list the registrations, `begin()`, acquire, release, the "no space" failure, `dump()`, `end()`, and `begin(minPoolSize)` with room for both buffers.
- DfuAndCalc: the scenario above. Each feature takes all of its buffers or none.
- StaticPool: a private pool in a static array, registration after `begin()`, two buffers held at once, `end()`.

## How buffers share the pool

A buffer can be acquired when a free gap of the pool is at least its size (rounded up to `SHARED_BUFFER_ALIGN`). Two buffers can be held at the same time only when both fit in the pool together.

Worked example: pool 16384 bytes, alignment 8.

```
registered: calc 16384, dfuA 4096, dfuB 4096, icon 1024

acquire(dfuA)  - offset 0      (gap 0..16383, the only gap)
acquire(dfuB)  - offset 4096   (gap 4096..16383)
acquire(calc)  - SB_ERR_NO_SPACE (largest gap 8192)
release(dfuA)  - gaps 0..4095 (4096) and 8192..16383 (8192)
acquire(icon)  - offset 0      (best fit: the 4096 gap, not the 8192 gap)
release(icon)
release(dfuB)  - one gap 0..16383
acquire(calc)  - offset 0
```

Guarantees and limits:

- Every registered buffer can be acquired when no other buffer is held. The pool is never smaller than the largest registration.
- Several buffers held at once need a pool of at least the total of their sizes, each rounded up to `SHARED_BUFFER_ALIGN`. With alignment 8, two 300-byte buffers need 608 bytes, not 600. Pass that total to `begin(minPoolSize)`, or to `begin(storage, size)` as the array size.
- *** IMPORTANT *** The free space can be split into gaps. `acquire()` can then fail with `SB_ERR_NO_SPACE` although `freeBytes()` is large enough. `largestFree()` is the largest buffer that fits now. A feature that needs several buffers should acquire all of them when it starts and release all of them when it stops (see DfuAndCalc).
- `begin()` clears the whole pool to 0. A successful `acquire()` clears the buffer's `size(h)` bytes to 0. A failed `acquire()`, and a `begin()` refused with `SB_ERR_STARTED` at the start, touch no memory.
- The pointer from `acquire()` must not be used after `release()`.

## Thread and interrupt safety

Every call reads and changes the registration table inside a short critical section. It lasts O(`SHARED_BUFFER_MAX_BUFFERS`) steps, and the duplicate-name check in `registerBuffer()` also compares names.

| Platform | Critical section |
|---|---|
| ESP32 (all chips) | `portENTER_CRITICAL_SAFE` on a spinlock shared by all objects. Works in tasks and ISRs, and across both cores. See the IRAM note below. |
| AVR | Interrupts off, SREG restored. |
| Cortex-M, single core (SAMD, nRF52) | Interrupts off (PRIMASK), PRIMASK restored. Other single-core Cortex-M parts use the same code but are not built in CI. |
| Host (Linux, macOS, Windows) | Spinlock (`std::atomic_flag`). Tested on Linux only. |
| RP2040, RP2350 and other targets | The build stops with `#error`. Define `SHARED_BUFFER_CRITICAL_ENTER()` and `SHARED_BUFFER_CRITICAL_EXIT()`. |

- `begin()` and `end()` call `malloc()` and `free()`, so they must not be called from an interrupt. Call `begin()` once, before other tasks use the pool.
- `dump()` calls `snprintf()` and the writer. Do not call it from an interrupt.
- ESP32: the library code is in flash. An interrupt handler registered with `ESP_INTR_FLAG_IRAM` runs while the flash cache is off (for example during a flash write in an OTA update), so it must not call the library. Handlers attached with `attachInterrupt()` are not IRAM handlers on arduino-esp32 3.3.9 by default.
- `acquire()` never waits for another user. A task that must wait retries later.
- `acquire()` clears the buffer after its critical section, so interrupts stay on during the clear. The call takes time in proportion to the buffer size; in an interrupt, prefer small buffers.
- `begin(storage, size)` clears the storage outside the critical section and then checks the state again, as `begin(size_t)` does after `malloc()`.
- The `dump()` writer runs outside the critical section.

## API

### Object

- `SharedBuffer()` - an empty object with no pool. The constructor is `constexpr` and the destructor is trivial, so a global object is initialized at compile time and may be used from other global constructors. The destructor does not free the pool: call `end()`. The object cannot be copied (a copy would share the pool): pass a reference or a pointer.
- `extern SharedBuffer SharedBuf` - global object shared by all code in the firmware. When no code uses it, the linker removes it (checked on AVR and nRF52).
- `SharedBuffer::Handle` - `int8_t`. 0 or more is a handle, a negative value is a `Status`.

### Calls

| Call | Returns | Notes |
|---|---|---|
| `Handle registerBuffer(const char* name, size_t size)` | handle, or `SB_ERR_ARGUMENT`, `SB_ERR_DUPLICATE`, `SB_ERR_FULL`, `SB_ERR_TOO_LARGE` | The name pointer is stored, not copied: use a string literal. Names are compared by content. After `begin()` the size must fit the pool. Registration cannot be undone. |
| `Status begin(size_t minPoolSize = 0)` | `SB_OK`, `SB_ERR_STARTED`, `SB_ERR_ARGUMENT`, `SB_ERR_TOO_LARGE`, `SB_ERR_NO_MEMORY` | Pool = max(largest registration, `minPoolSize`), both rounded to the alignment. One `malloc()` of the pool size plus `SHARED_BUFFER_ALIGN - 1` bytes. `SB_ERR_ARGUMENT` when nothing is registered and `minPoolSize` is 0. The pool is cleared to 0. |
| `Status begin(void* storage, size_t storageSize)` | `SB_OK`, `SB_ERR_STARTED`, `SB_ERR_ARGUMENT`, `SB_ERR_TOO_LARGE` | The whole array becomes the pool (rounded down to the alignment). Declare it `alignas(SHARED_BUFFER_ALIGN)`, or up to `SHARED_BUFFER_ALIGN - 1` bytes at its start are skipped. `SB_ERR_ARGUMENT` for NULL storage or less than `SHARED_BUFFER_ALIGN` bytes left after alignment. `SB_ERR_TOO_LARGE` when a registered buffer does not fit. The pool area of the storage is cleared to 0. |
| `Status end()` | `SB_OK`, `SB_ERR_NOT_STARTED`, `SB_ERR_HELD` | Frees heap memory from `begin(size_t)`. Registrations stay, and `begin()` may be called again. |
| `void* acquire(Handle h, Status* status = NULL)` | pointer, or NULL | `*status`: `SB_OK`, `SB_ERR_ARGUMENT`, `SB_ERR_NOT_STARTED`, `SB_ERR_HELD`, `SB_ERR_NO_SPACE`. The pointer is aligned to `SHARED_BUFFER_ALIGN`, and `size(h)` bytes are usable and 0. |
| `Status release(Handle h)` | `SB_OK`, `SB_ERR_ARGUMENT`, `SB_ERR_NOT_HELD` | |
| `void* pointer(Handle h) const` | pointer, or NULL | The pointer of a held buffer. NULL when not held. |
| `bool isHeld(Handle h) const` | | |
| `size_t size(Handle h) const` | registered size, 0 for an unknown handle | |
| `const char* name(Handle h) const` | registered name, NULL for an unknown handle | |
| `uint8_t count() const` | number of registrations | |
| `bool started() const` | true between `begin()` and `end()` | |
| `size_t poolSize() const` | pool bytes, 0 before `begin()` | |
| `size_t requiredSize() const` | largest registration, rounded | The pool size `begin()` allocates with `minPoolSize` 0. |
| `size_t freeBytes() const` | pool bytes not held | 0 before `begin()`. |
| `size_t largestFree() const` | largest free gap | The largest buffer `acquire()` can place now. 0 before `begin()`. |
| `void dump(LineWriter writeLine) const` | | Calls `writeLine(const char* line)` once per line. NULL is ignored. |
| `static const char* statusText(int status)` | short text | For example "no space". A handle (0 or more) gives "ok". |

`dump()` output (from the DfuAndCalc example):

```
SharedBuffer: pool 16384 bytes, 8192 free, largest gap 8192, 3 of 8 buffers
  #0 calc: 16384 bytes, not held
  #1 dfuA: 4096 bytes, held at offset 0
  #2 dfuB: 4096 bytes, held at offset 4096
```

### Status codes

| Code | Value | Meaning |
|---|---|---|
| `SB_OK` | 0 | Success. |
| `SB_ERR_ARGUMENT` | -1 | NULL or empty name, size 0, unknown handle, NULL storage or less than `SHARED_BUFFER_ALIGN` bytes of it after alignment, or nothing to size the pool by. |
| `SB_ERR_DUPLICATE` | -2 | A buffer with this name is already registered. |
| `SB_ERR_FULL` | -3 | `SHARED_BUFFER_MAX_BUFFERS` buffers are already registered. |
| `SB_ERR_TOO_LARGE` | -4 | The size cannot be rounded, the buffer is larger than the pool, or a registered buffer does not fit the storage. |
| `SB_ERR_NO_MEMORY` | -5 | `malloc()` failed in `begin()`. |
| `SB_ERR_NOT_STARTED` | -6 | `begin()` was not called, or `end()` was called. |
| `SB_ERR_STARTED` | -7 | `begin()` was already called. |
| `SB_ERR_HELD` | -8 | `acquire()`: the buffer is already held. `end()`: a buffer is held. |
| `SB_ERR_NOT_HELD` | -9 | `release()`: the buffer is not held. |
| `SB_ERR_NO_SPACE` | -10 | No free gap fits the buffer at this moment. |

## Build options

Set them in the build, for example `build_flags = -D SHARED_BUFFER_MAX_BUFFERS=16` in PlatformIO. Every file of one firmware must see the same values. The Arduino IDE has no per-project build flags, so the defaults apply there.

| Macro | Default | Meaning |
|---|---|---|
| `SHARED_BUFFER_MAX_BUFFERS` | 8 | Registrations per object, 1 to 127. |
| `SHARED_BUFFER_ALIGN` | 8 (AVR: 1) | Alignment of every buffer and rounding of every size, a power of two. |
| `SHARED_BUFFER_CRITICAL_ENTER()`, `SHARED_BUFFER_CRITICAL_EXIT()` | built in | Define both to replace the built-in critical section. They must exclude every other caller (other cores included) and order memory accesses on the hardware. The library adds a compiler barrier after ENTER and before EXIT (GCC and clang) and never nests them. They are seen only by `SharedBuffer.cpp`. |
| `SHARED_BUFFER_VERSION`, `SHARED_BUFFER_VERSION_STRING` | 10001, "1.0.1" | Library version (major * 10000 + minor * 100 + patch). |

## Costs

- RAM per object: per registration a name pointer, two sizes, a flag and one byte of order, plus 6 fields. With the default 8 registrations `sizeof(SharedBuffer)` is 74 bytes on AVR, 156 bytes on 32-bit ARM and 304 bytes on a 64-bit host (measured with avr-gcc, arm-none-eabi-gcc 10.3 and g++ 13).
- Pool: the pool size, plus up to `SHARED_BUFFER_ALIGN - 1` bytes from `begin(size_t)`, plus the heap chunk header.
- Time: `begin()` clears the pool once, and `acquire()` clears `size(h)` bytes (`memset()`) on every successful call.
- Arduino Uno, measured with PlatformIO (atmelavr 5.1.0): register, begin, acquire and release take 86 bytes of RAM and about 1.35 KB of flash including `malloc()`. `statusText()` adds 144 bytes of RAM, and `dump()` adds 208 bytes of RAM and about 2.2 KB of flash (`snprintf()`), because AVR keeps string literals in RAM. Unused calls are removed by the linker.

## Upgrading to 1.0.1

- `acquire()` now returns a buffer cleared to 0, and `begin()` clears the pool. 1.0.0 left the previous user's data in place. Code that cleared a buffer after `acquire()` still works and may drop that step. Code that expected data to survive from one `acquire()` to the next was relying on unspecified behavior and must keep its data elsewhere.
- `acquire()` takes longer for a large buffer (the clear). No call, type or status code changed.

## Version history

- 1.0.1 (2026-10-01): `acquire()` clears the granted buffer to 0, and `begin()` clears the pool.
- 1.0.0 (2026-09-30): first release.

## Author

Anatoli Arkhipenko <arkhipenko@hotmail.com>

## License

BSD 3-Clause. See LICENSE.txt.

## Repository

https://github.com/arkhipenko/SharedBuffer
