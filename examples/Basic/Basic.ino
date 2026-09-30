// Basic: register two buffers, create the pool, take a buffer and give it back.
//
// "rx" needs 256 bytes and "work" needs 512. The pool is the largest of them,
// 512 bytes. While "rx" is held, "work" does not fit and acquire() fails with
// "no space". After "rx" is released, "work" gets the whole pool.

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

void setup() {
  Serial.begin(115200);

  // 1. Register every buffer once. A negative handle is an error code.
  rxBuffer   = SharedBuf.registerBuffer("rx", 256);
  workBuffer = SharedBuf.registerBuffer("work", 512);
  report("register rx", rxBuffer);
  report("register work", workBuffer);
  if ( rxBuffer < 0 || workBuffer < 0 ) return;

  // 2. Allocate the pool once: the largest registration, 512 bytes
  SharedBuffer::Status status = SharedBuf.begin();
  report("begin", status);
  if ( status != SharedBuffer::SB_OK ) return;
  Serial.print("pool bytes: ");
  Serial.println((unsigned long) SharedBuf.poolSize());      // 512

  // 3. Take "rx" and use it
  uint8_t* rx = (uint8_t*) SharedBuf.acquire(rxBuffer, &status);
  report("acquire rx", status);                              // ok
  if ( rx == NULL ) return;
  memset(rx, 0, SharedBuf.size(rxBuffer));

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
}

void loop() {
}
