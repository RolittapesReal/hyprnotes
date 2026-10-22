/* LD_PRELOAD shim for the valgrind runs (tests/sanitize/valgrind-in-container.sh).
 * Qt's QRegularExpression compiles hot patterns with the PCRE2 JIT. The JIT code lives in anonymous memory, valgrind cannot unwind through
 * it, and its SIMD scans read up to 15 bytes past a subject and branch on the padding, which memcheck reports as "Invalid read" and
 * "Conditional jump depends on uninitialised value" with two unnamed frames. No suppression can match those narrowly, so the JIT is
 * turned off instead: reporting failure makes Qt keep using PCRE2's interpreter, which memcheck understands. */
int pcre2_jit_compile_16(void *code, unsigned int options) { (void)code; (void)options; return -45; /* PCRE2_ERROR_JIT_BADOPTION */ }
int pcre2_jit_compile_8(void *code, unsigned int options) { (void)code; (void)options; return -45; }
