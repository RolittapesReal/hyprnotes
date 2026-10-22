#pragma once
// Detects an AddressSanitizer/ThreadSanitizer build for tests whose wall-clock budgets are scaled there (the Release run is
// the performance gate). GCC defines __SANITIZE_ADDRESS__/__SANITIZE_THREAD__; Clang defines neither and only answers
// __has_feature(). This header is detection only: it changes which builds count as sanitized, nothing else.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define HN_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define HN_TEST_SANITIZED 1
#endif
#endif
