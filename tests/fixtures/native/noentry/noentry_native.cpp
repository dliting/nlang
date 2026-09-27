// No-entry fixture: a shared library that deliberately does NOT export
// nlang_native_init, so the loader must report the missing entry point.

void some_unrelated_symbol() {}
