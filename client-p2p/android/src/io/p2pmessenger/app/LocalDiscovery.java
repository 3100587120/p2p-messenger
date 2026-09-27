package io.p2pmessenger.app;

import android.content.Context;
import android.net.wifi.WifiManager;

/** Allow LAN peer discovery while the app is foregrounded; release to save battery. */
public final class LocalDiscovery {
    private static WifiManager.MulticastLock multicastLock;

    private LocalDiscovery() {}

    public static synchronized void setActive(Context context, boolean active) {
        if (active && multicastLock == null) {
            WifiManager manager = (WifiManager) context.getApplicationContext()
                    .getSystemService(Context.WIFI_SERVICE);
            if (manager == null) return;
            try {
                multicastLock = manager.createMulticastLock("shuangdianliao-lan-discovery");
                multicastLock.setReferenceCounted(false);
                multicastLock.acquire();
            } catch (SecurityException e) {
                multicastLock = null;
            }
        } else if (!active && multicastLock != null) {
            if (multicastLock.isHeld()) multicastLock.release();
            multicastLock = null;
        }
    }
}
