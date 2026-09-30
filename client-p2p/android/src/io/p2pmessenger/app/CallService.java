package io.p2pmessenger.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

/** Keeps an explicitly answered call audible; never starts or answers a call itself. */
public final class CallService extends Service {
    private static volatile boolean running=false;
    public static boolean isActive(){return running;}
    public static boolean setActive(Context context, boolean active) {
        try {
            Intent intent=new Intent(context,CallService.class);
            if(!active){context.stopService(intent);return true;}
            if(Build.VERSION.SDK_INT>=26)context.startForegroundService(intent);else context.startService(intent);
            return true;
        } catch(Exception denied){return false;}
    }
    @Override public void onCreate(){
        super.onCreate();
        NotificationManager manager=(NotificationManager)getSystemService(NOTIFICATION_SERVICE);
        if(Build.VERSION.SDK_INT>=26)manager.createNotificationChannel(new NotificationChannel("active-call","语音通话",NotificationManager.IMPORTANCE_LOW));
        Intent launch=getPackageManager().getLaunchIntentForPackage(getPackageName());
        PendingIntent open=PendingIntent.getActivity(this,31007,launch,PendingIntent.FLAG_UPDATE_CURRENT|PendingIntent.FLAG_IMMUTABLE);
        int icon=getResources().getIdentifier("ic_notification","drawable",getPackageName());
        Notification.Builder builder=Build.VERSION.SDK_INT>=26?new Notification.Builder(this,"active-call"):new Notification.Builder(this);
        Notification notification=builder.setSmallIcon(icon).setContentTitle("双点聊 · 语音通话中").setContentText("麦克风用于当前通话，点击返回聊天或挂断").setCategory(Notification.CATEGORY_CALL).setOngoing(true).setContentIntent(open).build();
        try {
            if(Build.VERSION.SDK_INT>=29)startForeground(31007,notification,ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE|ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
            else startForeground(31007,notification);
            running=true;
        } catch(Exception denied){stopSelf();}
    }
    @Override public int onStartCommand(Intent intent,int flags,int startId){return START_NOT_STICKY;}
    @Override public IBinder onBind(Intent intent){return null;}
    @Override public void onDestroy(){running=false;super.onDestroy();}
}
