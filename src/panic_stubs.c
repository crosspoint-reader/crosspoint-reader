// Stub implementations for --wrap=panic_print_backtrace,--wrap=panic_abort
// These replace the default panic handlers with no-ops to save flash space.
// The linker's --wrap flag redirects all calls to these stubs.

void __wrap_panic_print_backtrace(const void* frame, int depth) {
  (void)frame;
  (void)depth;
}

void __wrap_panic_abort(const char* details) {
  (void)details;
  // Still need to halt — call the real abort
  while (1) { }
}
