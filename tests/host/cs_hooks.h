// Force-included (-include cs_hooks.h) by the "counted" test variant.
// Replaces the built-in critical section with counting hooks defined in
// test_sharedbuffer.cpp, so the tests can check that every enter has an exit,
// that sections never nest, and that user callbacks run outside a section.
#pragma once

void sb_test_enter();
void sb_test_exit();

#define SHARED_BUFFER_CRITICAL_ENTER()  sb_test_enter()
#define SHARED_BUFFER_CRITICAL_EXIT()   sb_test_exit()
