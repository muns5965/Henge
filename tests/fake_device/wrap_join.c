/* Test-only link-time wrapper (-Wl,--wrap=thread_join): counts joins so a test can compare them with thread_new calls (I6, T13: every connection thread must be joined). */
#include <libimobiledevice-glue/thread.h>
int __real_thread_join(THREAD_T t);
volatile int hv_test_thread_join_calls = 0;
int __wrap_thread_join(THREAD_T t) { __atomic_add_fetch(&hv_test_thread_join_calls, 1, __ATOMIC_SEQ_CST); return __real_thread_join(t); }
