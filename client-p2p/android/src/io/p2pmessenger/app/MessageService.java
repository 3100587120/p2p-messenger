package io.p2pmessenger.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import org.qtproject.qt.android.bindings.QtService;

/** Dedicated process: owns the encrypted session only while the UI is away. */
public final class MessageService extends QtService {
    public static boolean enabled(Context context) { return context.getSharedPreferences("background-messages",0).getBoolean("enabled",true); }
    public static boolean configure(Context context, boolean enabled) {
        context.getSharedPreferences("background-messages",0).edit().putBoolean("enabled",enabled).apply();
        return start(context,enabled);
    }
    public static boolean start(Context context,boolean start) {
        try {
            Intent intent=new Intent(context,MessageService.class);
            if(!start) { context.stopService(intent); return true; }
            if(!enabled(context) || !context.getSharedPreferences("background-messages",0).getBoolean("registered",false))return true;
            if(Build.VERSION.SDK_INT>=26)context.startForegroundService(intent); else context.startService(intent);
            return true;
        } catch(Exception error) { return false; }
    }
    public static boolean accountReady(Context context) {
        context.getSharedPreferences("background-messages",0).edit().putBoolean("registered",true).apply();
        return start(context,true);
    }
    public static boolean openBackgroundSettings(Context context) {
        try {
            Intent settings=new Intent(android.provider.Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS);
            settings.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);context.startActivity(settings);return true;
        } catch(Exception error) {
            try {
                Intent settings=new Intent(android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS,android.net.Uri.parse("package:"+context.getPackageName()));
                settings.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);context.startActivity(settings);return true;
            } catch(Exception ignored) {return false;}
        }
    }
    @Override public void onCreate() {
        // Enter foreground before loading Qt's native runtime (5-second limit).
        NotificationManager manager=(NotificationManager)getSystemService(NOTIFICATION_SERVICE);
        if(Build.VERSION.SDK_INT>=26)manager.createNotificationChannel(new NotificationChannel("background","后台消息连接",NotificationManager.IMPORTANCE_LOW));
        Intent open=getPackageManager().getLaunchIntentForPackage(getPackageName());
        PendingIntent click=PendingIntent.getActivity(this,0,open,PendingIntent.FLAG_UPDATE_CURRENT|PendingIntent.FLAG_IMMUTABLE);
        int icon=getResources().getIdentifier("ic_notification","drawable",getPackageName());
        Notification.Builder builder=Build.VERSION.SDK_INT>=26?new Notification.Builder(this,"background"):new Notification.Builder(this);
        startForeground(31005,builder.setSmallIcon(icon).setContentTitle("双点聊 · 后台收消息").setContentText("关闭聊天界面后保持连接，点击返回应用").setContentIntent(click).setOngoing(true).build());
        super.onCreate();
    }
    public static boolean openAppSettings(Context context) {
        try {Intent intent=new Intent(android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS,android.net.Uri.parse("package:"+context.getPackageName()));intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);context.startActivity(intent);return true;}catch(Exception error){return false;}
    }
    public static void moveToBackground(Context context) {
        if(context instanceof android.app.Activity)((android.app.Activity)context).runOnUiThread(()->((android.app.Activity)context).moveTaskToBack(true));
    }
    @Override public int onStartCommand(Intent intent,int flags,int startId) {
        super.onStartCommand(intent,flags,startId);
        return START_STICKY;
    }
}
