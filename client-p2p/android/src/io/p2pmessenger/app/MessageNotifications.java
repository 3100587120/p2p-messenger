package io.p2pmessenger.app;
import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.content.pm.PackageManager;

public final class MessageNotifications {
    private static final String CHANNEL = "messages";
    public static boolean allowed(Context context) {
        if(Build.VERSION.SDK_INT>=33 && context.checkSelfPermission("android.permission.POST_NOTIFICATIONS")!=PackageManager.PERMISSION_GRANTED)return false;
        NotificationManager manager=(NotificationManager)context.getSystemService(Context.NOTIFICATION_SERVICE);
        if(manager==null || (Build.VERSION.SDK_INT>=24 && !manager.areNotificationsEnabled()))return false;
        if(Build.VERSION.SDK_INT>=26){NotificationChannel channel=manager.getNotificationChannel(CHANNEL);if(channel!=null && channel.getImportance()==NotificationManager.IMPORTANCE_NONE)return false;}
        return true;
    }
    public static void requestPermission(Context context) {
        if (Build.VERSION.SDK_INT >= 33 && context instanceof Activity && context.checkSelfPermission("android.permission.POST_NOTIFICATIONS") != PackageManager.PERMISSION_GRANTED)
            ((Activity)context).runOnUiThread(() -> ((Activity)context).requestPermissions(new String[]{"android.permission.POST_NOTIFICATIONS"},8323));
    }
    public static void show(Context context, String title, String body) {
        try {
            if (Build.VERSION.SDK_INT >= 33 && context.checkSelfPermission("android.permission.POST_NOTIFICATIONS") != PackageManager.PERMISSION_GRANTED) return;
            NotificationManager manager = (NotificationManager)context.getSystemService(Context.NOTIFICATION_SERVICE);
            if (manager == null) return;
            if (Build.VERSION.SDK_INT >= 26) manager.createNotificationChannel(new NotificationChannel(CHANNEL,"消息和好友申请",NotificationManager.IMPORTANCE_HIGH));
            Intent launch = context.getPackageManager().getLaunchIntentForPackage(context.getPackageName());
            if (launch == null) return; launch.addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP);
            PendingIntent click = PendingIntent.getActivity(context,0,launch,PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
            int icon = context.getResources().getIdentifier("ic_notification","drawable",context.getPackageName());
            Notification.Builder builder = Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(context,CHANNEL) : new Notification.Builder(context);
            builder.setSmallIcon(icon).setContentTitle(title).setContentText(body).setStyle(new Notification.BigTextStyle().bigText(body)).setVisibility(Notification.VISIBILITY_PRIVATE).setContentIntent(click).setAutoCancel(true).setCategory(Notification.CATEGORY_MESSAGE).setPriority(Notification.PRIORITY_HIGH).setDefaults(Notification.DEFAULT_ALL);
            manager.notify(title.hashCode(),builder.build());
        } catch (Exception ignored) { /* Notification denial must never crash chat. */ }
    }
}
