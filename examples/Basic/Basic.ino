// Basic: register two buffers, create the pool, take a buffer and give it back.
//
// "rx" needs 256 bytes and "work" needs 512. The pool is the largest of them,
// 512 bytes. While "rx" is held, "work" does not fit and acquire() fails with
// "no space". After "rx" is released, "work" gets the whole pool. At the end the
// pool is made again with room for both buffers at once. Every granted buffer is
// cleared to 0, even where the previous user left data.

#include <SharedBuffer.h>

SharedBuffer::Handle rxBuffer;
SharedBuffer::Handle workBuffer;

void printLine(const char* line) {
  Serial.println(line);
}

void report(const char* what, int status) {
  Serial.print(what);
  Serial.print(": ");
  Serial.println(SharedBuffer::statusText(status));
}

bool allZero(const uint8_t* p, size_t n) {
  for ( size_t i = 0; i < n; i++ ) {
    if ( p[i] != 0 ) return false;
  }
  return true;
}

void setup() {
  Serial.begin(115200);

  // 1. Register every buffer once. A negative handle is an error code.
  rxBuffer   = SharedBuf.registerBuffer("rx", 256);
  workBuffer = SharedBuf.registerBuffer("work", 512);
  report("register rx", rxBuffer);
  report("register work", workBuffer);
  if ( rxBuffer < 0 || workBuffer < 0 ) return;
  for ( SharedBuffer::Handle h = 0; h < (SharedBuffer::Handle) SharedBuf.count(); h++ ) {
    Serial.print("  ");
    Serial.print(SharedBuf.name(h));                         // rx, work
    Serial.print(" ");
    Serial.println((unsigned long) SharedBuf.size(h));       // 256, 512
  }

  // 2. Allocate the pool once: the largest registration, 512 bytes
  SharedBuffer::Status status = SharedBuf.begin();
  report("begin", status);
  if ( status != SharedBuffer::SB_OK ) return;
  Serial.print("pool bytes: ");
  Serial.println((unsigned long) SharedBuf.poolSize());      // 512

  // 3. Take "rx". It comes cleared to 0.
  uint8_t* rx = (uint8_t*) SharedBuf.acquire(rxBuffer, &status);
  report("acquire rx", status);                              // ok
  if ( rx == NULL ) return;
  Serial.print("rx is all 0: ");
  Serial.println((unsigned long) allZero(rx, SharedBuf.size(rxBuffer)));   // 1

  // "work" needs 512 bytes, and "rx" holds 256 of them
  if ( SharedBuf.acquire(workBuffer, &status) == NULL ) {
    report("acquire work while rx is held", status);         // no space
  }
  SharedBuf.dump(printLine);

  // 4. Give "rx" back. The whole pool is free again.
  report("release rx", SharedBuf.release(rxBuffer));         // ok

  uint8_t* work = (uint8_t*) SharedBuf.acquire(workBuffer, &status);
  report("acquire work", status);                            // ok
  if ( work == NULL ) return;
  for ( size_t i = 0; i < SharedBuf.size(workBuffer); i++ ) work[i] = (uint8_t) i;
  Serial.print("work[511]: ");
  Serial.println((unsigned long) work[511]);                 // 255
  report("release work", SharedBuf.release(workBuffer));     // ok
  report("release work again", SharedBuf.release(workBuffer));  // not held

  // "work" left 0, 1, 2 ... in the pool. "rx" gets the same bytes, cleared to 0.
  rx = (uint8_t*) SharedBuf.acquire(rxBuffer, &status);
  report("acquire rx again", status);                        // ok
  if ( rx == NULL ) return;
  Serial.print("rx is all 0 after work used the bytes: ");
  Serial.println((unsigned long) allZero(rx, SharedBuf.size(rxBuffer)));   // 1
  report("release rx", SharedBuf.release(rxBuffer));         // ok

  // 5. Both buffers at the same time need a pool of 256 + 512 bytes.
  //    end() frees the pool; the registrations stay.
  report("end", SharedBuf.end());                            // ok
  Serial.print("started: ");
  Serial.println((unsigned long) SharedBuf.started());       // 0
  report("begin(768)", SharedBuf.begin(768));                // ok
  rx   = (uint8_t*) SharedBuf.acquire(rxBuffer, &status);
  report("acquire rx", status);                              // ok
  work = (uint8_t*) SharedBuf.acquire(workBuffer, &status);
  report("acquire work while rx is held", status);           // ok
  Serial.print("free bytes: ");
  Serial.println((unsigned long) SharedBuf.freeBytes());     // 0
  report("release rx", SharedBuf.release(rxBuffer));         // ok
  report("release work", SharedBuf.release(workBuffer));     // ok
}

void loop() {
}
