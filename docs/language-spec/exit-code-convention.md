# Exit Code Convention


The process exit code is 8-bit (0-255) on Windows. Test expected values must
not exceed 255. For tests requiring larger computations, use modular arithmetic
or return a derived value that fits in the exit code range.
