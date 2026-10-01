// DfuAndCalc: two features that never run at the same time share one pool.
//
// Normal operation runs a calculation that needs a 16 KB buffer. A firmware update
// (DFU) needs two 4 KB buffers and never runs during the calculation. Separate
// buffers would take 24 KB of RAM. SharedBuffer takes 16 KB: the largest of them.
// Each feature takes all of its buffers when it starts and gives them back when it
// stops. A feature cannot start while the other one holds the memory.
//
// Arduino Uno has 2 KB of RAM, so on AVR the same layout runs at 1/16 scale.

#include <SharedBuffer.h>

#if defined(__AVR__)
const size_t CALC_SIZE = 1024;
const size_t DFU_SIZE  = 256;
#else
const size_t CALC_SIZE = 16384;
const size_t DFU_SIZE  = 4096;
#endif

SharedBuffer::Handle calcBuffer;
SharedBuffer::Handle dfuBufferA;
SharedBuffer::Handle dfuBufferB;

void printLine(const char* line) {
  Serial.println(line);
}

void report(const char* what, int status) {
  Serial.print(what);
  Serial.print(": ");
  Serial.println(SharedBuffer::statusText(status));
}

// ---- calculation ----------------------------------------------------------------

float* samples = NULL;

bool startCalculation() {
  SharedBuffer::Status status;
  samples = (float*) SharedBuf.acquire(calcBuffer, &status);
  report("start calculation", status);
  if ( samples == NULL ) return false;
  size_t n = SharedBuf.size(calcBuffer) / sizeof(float);
  for ( size_t i = 0; i < n; i++ ) samples[i] = 0.5f * (float) i;
  return true;
}

void stopCalculation() {
  report("stop calculation", SharedBuf.release(calcBuffer));
  samples = NULL;                        // the memory belongs to the pool again
}

// ---- firmware update: both buffers or none ---------------------------------------

bool startDfu() {
  SharedBuffer::Status status;
  if ( SharedBuf.acquire(dfuBufferA, &status) == NULL ) {
    report("start DFU, buffer A", status);
    return false;
  }
  if ( SharedBuf.acquire(dfuBufferB, &status) == NULL ) {
    report("start DFU, buffer B", status);
    report("give back DFU buffer A", SharedBuf.release(dfuBufferA));   // do not keep half of the memory
    return false;
  }
  report("start DFU", status);
  return true;
}

void stopDfu() {
  report("stop DFU, buffer A", SharedBuf.release(dfuBufferA));
  report("stop DFU, buffer B", SharedBuf.release(dfuBufferB));
}

// ---------------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  calcBuffer = SharedBuf.registerBuffer("calc", CALC_SIZE);
  dfuBufferA = SharedBuf.registerBuffer("dfuA", DFU_SIZE);
  dfuBufferB = SharedBuf.registerBuffer("dfuB", DFU_SIZE);
  if ( calcBuffer < 0 || dfuBufferA < 0 || dfuBufferB < 0 ) {
    Serial.println("registration failed");
    return;
  }

  SharedBuffer::Status status = SharedBuf.begin();
  report("begin", status);
  if ( status != SharedBuffer::SB_OK ) return;
  Serial.print("pool bytes: ");
  Serial.print((unsigned long) SharedBuf.poolSize());          // 16384 (AVR: 1024)
  Serial.print(" instead of ");
  Serial.println((unsigned long) (CALC_SIZE + 2 * DFU_SIZE));  // 24576 (AVR: 1536)

  // Normal operation
  startCalculation();                    // ok
  startDfu();                            // buffer A: no space

  // An update arrives: stop the calculation, then run DFU
  stopCalculation();                     // ok
  if ( startDfu() ) {                    // ok
    uint8_t* a = (uint8_t*) SharedBuf.pointer(dfuBufferA);
    uint8_t* b = (uint8_t*) SharedBuf.pointer(dfuBufferB);
    Serial.print("DFU buffer B starts ");
    Serial.print((unsigned long) (b - a));
    Serial.println(" bytes after buffer A");                   // 4096 (AVR: 256)
    SharedBuf.dump(printLine);
    startCalculation();                  // no space
    stopDfu();                           // ok, ok
  }

  // Back to normal operation
  startCalculation();                    // ok
  stopCalculation();                     // ok
}

void loop() {
}
