// Stub implementations for --wrap=panic_print_backtrace,--wrap=panic_abort
// These replace the default panic handlers with no-ops to save flash space.
// The linker's --wrap flag redirects all calls to these stubs.
// Only compiled in non-debug builds to allow full panic info in debug mode.

#ifndef NDEBUG
// In debug builds, don't use stubs - allow full panic handling
#else

void __wrap_panic_print_backtrace(const void* frame, int depth) {
  (void)frame;
  (void)depth;
}

void __wrap_panic_abort(const char* details) {
  (void)details;
  // Still need to halt — call the real abort
  while (1) { }
}

#endif  // NDEBUG
