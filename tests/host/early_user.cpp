// A global constructor that registers a buffer in SharedBuf during static
// initialization. The Makefile links this file before SharedBuffer.cpp. If SharedBuf
// were initialized at run time, its constructor would run after this one and erase
// the registration, and test_static_init() would fail.
#include "SharedBuffer.h"

SharedBuffer::Handle g_earlyHandle = -100;

namespace {
struct EarlyUser {
    EarlyUser() { g_earlyHandle = SharedBuf.registerBuffer("early", 64); }
} s_earlyUser;
}
