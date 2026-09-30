// StaticPool: a private pool in a static array.
//
// The linker counts the array in the RAM report at build time, and malloc() is never
// called. The example also registers buffers after begin(), and holds two buffers at
// the same time because together they fit in the pool.

#include <SharedBuffer.h>

alignas(SHARED_BUFFER_ALIGN) static uint8_t poolMemory[1024];

SharedBuffer pool;                       // separate from the global SharedBuf

void report(const char* what, int status) {
  Serial.print(what);
  Serial.print(": ");
  Serial.println(SharedBuffer::statusText(status));
}

void printValue(const char* what, unsigned long value) {
  Serial.print(what);
  Serial.print(": ");
  Serial.println(value);
}

void setup() {
  Serial.begin(115200);

  SharedBuffer::Handle frame = pool.registerBuffer("frame", 600);
  report("register frame", frame);                            // ok
  printValue("required bytes", pool.requiredSize());          // 600

  SharedBuffer::Status status = pool.begin(poolMemory, sizeof(poolMemory));
  report("begin", status);                                    // ok
  if ( status != SharedBuffer::SB_OK ) return;
  printValue("pool bytes", pool.poolSize());                  // 1024

  // After begin() a registration must fit the pool
  SharedBuffer::Handle log = pool.registerBuffer("log", 300);
  report("register log (300 bytes)", log);                    // ok
  report("register big (2000 bytes)", pool.registerBuffer("big", 2000));   // too large
  if ( frame < 0 || log < 0 ) return;

  // 600 + 300 fit in 1024, so both can be held at once
  uint8_t* f = (uint8_t*) pool.acquire(frame, &status);
  report("acquire frame", status);                            // ok
  uint8_t* l = (uint8_t*) pool.acquire(log, &status);
  report("acquire log", status);                              // ok
  if ( f == NULL || l == NULL ) return;
  printValue("log offset", (unsigned long) (l - poolMemory)); // 600
  printValue("free bytes", pool.freeBytes());                 // 124 on AVR, 120 with 8-byte alignment (300 becomes 304)
  printValue("largest gap", pool.largestFree());              // the same: the free bytes form one gap
  printValue("log is held", pool.isHeld(log));                // 1
  printValue("pointer(log) matches", pool.pointer(log) == l); // 1

  report("end while held", pool.end());                       // held
  pool.release(frame);
  pool.release(log);
  printValue("frame is held", pool.isHeld(frame));            // 0
  report("end", pool.end());                                  // ok: registrations stay
  printValue("registered buffers", pool.count());             // 2
}

void loop() {
}
