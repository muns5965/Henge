package dev.muns.hengeprobe;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;

import java.net.Inet4Address;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;

/**
 * [G7-EXEC] The Android side of the network probe's comparison: while the native probe runs, every 30 seconds this class writes (a) the system's view of the default network
 * (is there one, does the system call it validated, which transport) and (b) the Java name lookups of the same three built-in addresses (InetAddress, the system resolver as the
 * Java side sees it), so that they can be put next to the native lookups of the probe in one log. It also writes a line whenever the default network changes. Numbers and fixed words
 * only. It looks up names and reads state; it opens no connection and sends nothing.
 */
final class G7NetState implements G7NetProbe.Sampler {
    private static final String[] HOSTS = {"gs.apple.com", "gs.apple.com.akadns.net", "gs.apple.com.v.aaplimg.com"};   // the same order as host/net_probe.c: url=1..3
    private static final int INTERVAL_S = 30, ROUNDS = 10, LOOKUP_LIMIT_MS = 10000;
    private final Context app;
    private volatile boolean stop;
    private Thread thread;
    private ConnectivityManager cm;
    private ConnectivityManager.NetworkCallback callback;

    G7NetState(Context context) { this.app = context.getApplicationContext(); }

    @Override public void start(G7NetProbe.Sink sink) {
        cm = (ConnectivityManager) app.getSystemService(Context.CONNECTIVITY_SERVICE);
        if (cm != null) {
            callback = new ConnectivityManager.NetworkCallback() {
                @Override public void onAvailable(Network n) { sink.line("java-net-event: available"); }
                @Override public void onLost(Network n) { sink.line("java-net-event: lost"); }
                @Override public void onCapabilitiesChanged(Network n, NetworkCapabilities c) { sink.line("java-net-event: capabilities " + caps(c)); }
                @Override public void onLinkPropertiesChanged(Network n, LinkProperties lp) { sink.line("java-net-event: link dns_servers=" + lp.getDnsServers().size() + " addresses=" + lp.getLinkAddresses().size()); }
            };
            try { cm.registerDefaultNetworkCallback(callback); } catch (RuntimeException e) { callback = null; sink.line("java-net: callback not registered (" + e.getClass().getSimpleName() + ")"); }
        } else sink.line("java-net: no connectivity service");
        stop = false;
        thread = new Thread(() -> loop(sink), "g7-net-state");
        thread.setDaemon(true);
        thread.start();
    }

    @Override public void stop() {
        stop = true;
        if (thread != null) thread.interrupt();
        if (cm != null && callback != null) { try { cm.unregisterNetworkCallback(callback); } catch (RuntimeException ignored) { } callback = null; }
    }

    private void loop(G7NetProbe.Sink sink) {
        ExecutorService ex = Executors.newCachedThreadPool(r -> { Thread t = new Thread(r, "g7-net-dns"); t.setDaemon(true); return t; });
        try {
            long t0 = System.nanoTime();
            for (int round = 1; round <= ROUNDS && !stop; round++) {
                sink.line("java-net: t=" + (System.nanoTime() - t0) / 1_000_000_000L + "s round=" + round + " " + state());
                for (int k = 0; k < 3 && !stop; k++) {
                    int h = (round - 1 + k) % 3;   // the same rotation as the native probe
                    sink.line("java-dns: round=" + round + " url=" + (h + 1) + " " + lookup(ex, HOSTS[h]));
                }
                for (int s = 0; round < ROUNDS && s < INTERVAL_S && !stop; s++) Thread.sleep(1000);
            }
        } catch (InterruptedException ignored) {
        } finally { ex.shutdownNow(); }
    }

    private String state() {
        if (cm == null) return "active=unknown";
        try {
            Network n = cm.getActiveNetwork();
            if (n == null) return "active=none";
            NetworkCapabilities c = cm.getNetworkCapabilities(n);
            return "active=yes " + (c == null ? "caps=none" : caps(c));
        } catch (RuntimeException e) { return "active=error(" + e.getClass().getSimpleName() + ")"; }
    }

    private static String caps(NetworkCapabilities c) {
        String tr = c.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) ? "wifi" : c.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR) ? "cellular"
                : c.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET) ? "ethernet" : c.hasTransport(NetworkCapabilities.TRANSPORT_VPN) ? "vpn" : "other";
        return "transport=" + tr + " internet=" + (c.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET) ? 1 : 0)
                + " validated=" + (c.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED) ? 1 : 0)
                + " vpn=" + (c.hasTransport(NetworkCapabilities.TRANSPORT_VPN) ? 1 : 0)
                + " metered=" + (c.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED) ? 0 : 1);
    }

    /** One Java lookup, bounded: "ok ms=.. v4=.. v6=.." or the exception's class name. Never prints the addresses. */
    private static String lookup(ExecutorService ex, String host) {
        long a = System.nanoTime();
        Future<InetAddress[]> f = ex.submit(() -> InetAddress.getAllByName(host));
        try {
            InetAddress[] r = f.get(LOOKUP_LIMIT_MS, TimeUnit.MILLISECONDS);
            int v4 = 0, v6 = 0;
            for (InetAddress i : r) { if (i instanceof Inet4Address) v4++; else if (i instanceof Inet6Address) v6++; }
            return "ok ms=" + (System.nanoTime() - a) / 1_000_000L + " v4=" + v4 + " v6=" + v6;
        } catch (java.util.concurrent.TimeoutException e) { f.cancel(true); return "timeout ms=" + (System.nanoTime() - a) / 1_000_000L;
        } catch (java.util.concurrent.ExecutionException e) { Throwable c = e.getCause() != null ? e.getCause() : e; return "failed ms=" + (System.nanoTime() - a) / 1_000_000L + " " + c.getClass().getSimpleName();
        } catch (InterruptedException e) { Thread.currentThread().interrupt(); return "interrupted"; }
    }
}
