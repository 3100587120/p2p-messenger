package io.p2pmessenger.app;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
public final class MessageBootReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context,Intent intent) {
        if(Intent.ACTION_BOOT_COMPLETED.equals(intent.getAction()) && MessageService.enabled(context) && context.getSharedPreferences("background-messages",0).getBoolean("registered",false)) MessageService.start(context,true);
    }
}
