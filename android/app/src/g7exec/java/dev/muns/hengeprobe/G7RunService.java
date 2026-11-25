package dev.muns.hengeprobe;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;

/**
 * Keeps the app's process (and the clone-boot runner it spawned) out of the cached/background state while a run is live.
 *
 * Why: the 2026-11-25 device logs show the runner's resident memory falling from 1886 MiB to ~84 MiB with ~2 GB of swap and the guest's
 * CPU samples almost standing still, the pattern of Android compacting/freezing a process that is no longer foreground. That cause is
 * NOT proven (no PSI/freezer measurement yet); this service only removes the app from the cached state so the next run shows whether
 * it was the cause. It starts nothing in the VM, changes no VM setting, and touches no file.
 *
 * It stops itself when no clone-boot run has been live for IDLE_STOP_MS (long enough for the automatic-restart wait) or when the
 * activity asks it to; the wake lock is a partial one (CPU only, the screen may turn off) and always has a timeout.
 */
public final class G7RunService extends Service {
    private static final String CHANNEL = "g7run";
    private static final int NOTE_ID = 7001;
    static final long POLL_MS = 15_000, IDLE_STOP_MS = 10 * 60_000L, LOCK_MAX_MS = 8 * 3600_000L;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private PowerManager.WakeLock lock;
    private long idleMs;

    /** Starts the service; never throws (a refusal by the system only means the run is not protected, which the log shows by the memory pattern). */
    public static boolean start(Context c) {
        try {
            Intent i = new Intent(c, G7RunService.class);
            if (Build.VERSION.SDK_INT >= 26) c.startForegroundService(i); else c.startService(i);
            return true;
        } catch (RuntimeException e) { return false; }
    }

    public static void stop(Context c) {
        try { c.stopService(new Intent(c, G7RunService.class)); } catch (RuntimeException ignored) { }
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        if (Build.VERSION.SDK_INT >= 26 && nm != null) nm.createNotificationChannel(new NotificationChannel(CHANNEL, "Henge 실행", NotificationManager.IMPORTANCE_LOW));
        Intent open = new Intent(this, G7ExecActivity.class).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_REORDER_TO_FRONT);
        PendingIntent pi = PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        Notification.Builder nb = Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(this, CHANNEL) : new Notification.Builder(this);
        Notification n = nb.setSmallIcon(android.R.drawable.ic_media_play).setContentTitle("Henge 복제본 부팅 실행 중")
                .setContentText("앱이 백그라운드로 가도 실행이 느려지지 않게 유지합니다.").setContentIntent(pi).setOngoing(true).build();
        try {
            if (Build.VERSION.SDK_INT >= 34) startForeground(NOTE_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
            else startForeground(NOTE_ID, n);
        } catch (RuntimeException e) { stopSelf(); return START_NOT_STICKY; }
        if (lock == null) {
            PowerManager pm = (PowerManager) getSystemService(Context.POWER_SERVICE);
            if (pm != null) {
                lock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "henge:g7run");
                lock.setReferenceCounted(false);
                lock.acquire(LOCK_MAX_MS);
            }
        }
        handler.removeCallbacks(poll);
        idleMs = 0;
        handler.postDelayed(poll, POLL_MS);
        return START_NOT_STICKY;   // a killed process has no run left to protect
    }

    private final Runnable poll = new Runnable() {
        @Override public void run() {
            if (G7CloneBoot.hasLive()) idleMs = 0; else idleMs += POLL_MS;
            if (idleMs >= IDLE_STOP_MS) { stopSelf(); return; }
            handler.postDelayed(this, POLL_MS);
        }
    };

    @Override public void onDestroy() {
        handler.removeCallbacks(poll);
        if (lock != null && lock.isHeld()) lock.release();
        lock = null;
        super.onDestroy();
    }

    @Override public IBinder onBind(Intent intent) { return null; }
}
