/* Test-only link-time wrapper (-Wl,--wrap=thread_new, see tools/fake-restore-test.sh): passes every call through to the real thread_new, except that a test can arm ONE
   failure. The vendored sources are not changed; calls from the static library into the glue's thread_new are what gets redirected. */
#include <libimobiledevice-glue/thread.h>
int __real_thread_new(THREAD_T *t, thread_func_t f, void *d);
volatile int hv_test_fail_next_thread_new = 0;
volatile int hv_test_thread_new_calls = 0;
volatile int hv_test_thread_new_failed = 0;
int __wrap_thread_new(THREAD_T *t, thread_func_t f, void *d) {
    __atomic_add_fetch(&hv_test_thread_new_calls, 1, __ATOMIC_SEQ_CST);
    if (__atomic_exchange_n(&hv_test_fail_next_thread_new, 0, __ATOMIC_SEQ_CST)) { __atomic_add_fetch(&hv_test_thread_new_failed, 1, __ATOMIC_SEQ_CST); return -1; }
    return __real_thread_new(t, f, d);
}
