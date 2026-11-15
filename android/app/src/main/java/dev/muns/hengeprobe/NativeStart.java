package dev.muns.hengeprobe;

/** Pure, Android-free interpretation of ui_jni.c's impl_nativeStart() return value. Both screens
 *  (DisplayTestActivity and VmDisplayActivity) share the ONE native singleton (see ui_jni.c's own comment
 *  on why a single display pipeline serves either class), so both need the same decision here -- and both
 *  had the same bug: checking only `r < 0` as failure, which lets `r == 1` ("already running", i.e. owned
 *  by whichever screen built it first) fall through as if this caller had built its own instance. That let
 *  a second screen set `started = true` and go on to touch Surface/runner/native-destroy calls against
 *  resources it never actually built.
 *
 *  Kept as its own, Android-free class (not a method on either Activity, which extend android.app.Activity
 *  and so cannot be loaded in tests/java's plain JVM) so this specific decision has a direct test:
 *  tests/java/NativeStartTest.java.
 *
 *  impl_nativeStart()'s contract (ui_jni.c): 0 built and open (the only success for THIS caller); 1 already
 *  running elsewhere; -1 building failed; -2 the previous instance is still closing or being released. */
final class NativeStart {
    private NativeStart() {}

    /** True only for r == 0: this call itself built a fresh instance. r == 1 is NOT success for the caller,
     *  even though a native singleton does exist and is open -- it belongs to whichever screen built it. */
    static boolean succeeded(int r) { return r == 0; }

    /** A short reason to append to the status text for a failing r. Returns "" for -1 and any other
     *  unrecognised value, and must not be called with r == 0 (succeeded() is true there). */
    static String refusalReason(int r) {
        if (r == -2) return ": the previous screen is still being closed, try again in a moment";
        if (r == 1) return ": another screen is already using the native display";
        return "";
    }
}
