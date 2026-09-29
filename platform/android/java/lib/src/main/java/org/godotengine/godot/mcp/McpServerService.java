package org.godotengine.godot.mcp;

import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.os.IBinder;
import android.os.PowerManager;
import android.util.Log;

import androidx.core.app.ActivityCompat;
import androidx.core.app.NotificationCompat;
import androidx.core.app.NotificationManagerCompat;
import androidx.core.content.ContextCompat;

/**
 * Foreground service that keeps the Godot MCP server alive while the editor is
 * in the background, and shows a notification with a "Matikan server" action
 * so the MCP server can be stopped without opening the editor.
 */
public class McpServerService extends android.app.Service {

    private static final String TAG = "GodotMCP";
    private static final String CHANNEL_ID = "mcp_server";
    private static final int NOTIF_ID = 8766;

    public static final String ACTION_STOP_MCP = "org.godotengine.godot.mcp.STOP";
    public static final String ACTION_COPY_URL_MCP = "org.godotengine.godot.mcp.COPY_URL";

    private static McpServerService instance = null;

    // Native side: implemented in modules/godot_mcp/mcp_android.cpp
    static native void notifyMcpStop();
    static native String getServerUrl();

    public static void start(Context context, int port) {
        requestNotificationPermission(context);
        try {
            Intent intent = new Intent(context, McpServerService.class);
            intent.putExtra("port", port);
            ContextCompat.startForegroundService(context, intent);
        } catch (Exception e) {
            Log.w(TAG, "start foreground service failed: " + e.getMessage());
        }
    }

    public static void stop(Context context) {
        if (instance == null) {
            return;
        }
        try {
            instance.stopSelf();
        } catch (Exception ignored) {
        }
        instance = null;
    }

    private static void requestNotificationPermission(Context context) {
        if (Build.VERSION.SDK_INT >= 33 && context instanceof Activity) {
            Activity activity = (Activity) context;
            if (ContextCompat.checkSelfPermission(context, android.Manifest.permission.POST_NOTIFICATIONS)
                    != PackageManager.PERMISSION_GRANTED) {
                ActivityCompat.requestPermissions(activity,
                        new String[]{android.Manifest.permission.POST_NOTIFICATIONS}, 8766);
            }
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        int port = 8766;
        if (intent != null && intent.hasExtra("port")) {
            port = intent.getIntExtra("port", 8766);
        }
        createChannel();
        Notification notification = buildNotification(port);
        try {
            if (Build.VERSION.SDK_INT >= 34) {
                startForeground(NOTIF_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
            } else if (Build.VERSION.SDK_INT >= 29) {
                startForeground(NOTIF_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_NONE);
            } else {
                startForeground(NOTIF_ID, notification);
            }
        } catch (Exception e) {
            try {
                startForeground(NOTIF_ID, notification);
            } catch (Exception e2) {
                Log.w(TAG, "startForeground failed: " + e.getMessage());
            }
        }
        keepAwake();
        return START_STICKY;
    }

    private void keepAwake() {
        try {
            PowerManager pm = (PowerManager) getSystemService(Context.POWER_SERVICE);
            if (pm.isWakeLockLevelSupported(PowerManager.PARTIAL_WAKE_LOCK)) {
                PowerManager.WakeLock wl = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "godot:mcp");
                wl.acquire(6 * 60 * 60 * 1000L);
            }
        } catch (Exception ignored) {
        }
    }

    @Override
    public void onDestroy() {
        instance = null;
        try {
            NotificationManagerCompat.from(this).cancel(NOTIF_ID);
        } catch (Exception ignored) {
        }
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private void createChannel() {
        if (Build.VERSION.SDK_INT >= 26) {
            NotificationChannel channel = new NotificationChannel(
                    CHANNEL_ID,
                    "MCP Server",
                    NotificationManager.IMPORTANCE_LOW);
            channel.setDescription("Godot MCP server status");
            channel.setShowBadge(false);
            NotificationManager nm = getSystemService(NotificationManager.class);
            nm.createNotificationChannel(channel);
        }
    }

    private Notification buildNotification(int port) {
        Intent stopIntent = new Intent(this, StopReceiver.class);
        stopIntent.setAction(ACTION_STOP_MCP);
        PendingIntent stopPi = PendingIntent.getBroadcast(
                this, 1, stopIntent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        int iconRes = getIconRes();
        String url = getServerUrl();
        if (url == null || url.isEmpty()) {
            url = "http://127.0.0.1:" + port + "/mcp";
        }

        Intent copyIntent = new Intent(this, CopyUrlReceiver.class);
        copyIntent.setAction(ACTION_COPY_URL_MCP);
        PendingIntent copyPi = PendingIntent.getBroadcast(
                this, 2, copyIntent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        NotificationCompat.Builder builder = new NotificationCompat.Builder(this, CHANNEL_ID)
                .setSmallIcon(iconRes)
                .setContentTitle("Server MCP hidup")
                .setContentText(url)
                .setOnlyAlertOnce(true)
                .setPriority(NotificationCompat.PRIORITY_LOW)
                .addAction(new NotificationCompat.Action(iconRes, "Salin URL MCP", copyPi))
                .addAction(new NotificationCompat.Action(iconRes, "Matikan server", stopPi));

        return builder.build();
    }

    private int getIconRes() {
        try {
            int id = getResources().getIdentifier("icon_monochrome", "mipmap", getPackageName());
            return id != 0 ? id : android.R.drawable.ic_menu_compass;
        } catch (Exception e) {
            return android.R.drawable.ic_menu_compass;
        }
    }

    /** Receives the "Salin URL MCP" notification action and copies the URL to the clipboard. */
    public static class CopyUrlReceiver extends BroadcastReceiver {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (ACTION_COPY_URL_MCP.equals(intent.getAction())) {
                String url = null;
                try {
                    url = McpServerService.getServerUrl();
                } catch (UnsatisfiedLinkError e) {
                    Log.w(TAG, "native getServerUrl not available: " + e.getMessage());
                }
                if (url == null || url.isEmpty()) {
                    url = "http://127.0.0.1:8766/mcp";
                }
                ClipboardManager cm = (ClipboardManager) context.getSystemService(Context.CLIPBOARD_SERVICE);
                cm.setPrimaryClip(ClipData.newPlainText("MCP URL", url));
                Log.i(TAG, "MCP URL copied to clipboard: " + url);
            }
        }
    }

    /** Receives the "Matikan server" notification action and stops the MCP server. */
    public static class StopReceiver extends BroadcastReceiver {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (ACTION_STOP_MCP.equals(intent.getAction())) {
                Log.i(TAG, "Stopping MCP server from notification");
                try {
                    McpServerService.notifyMcpStop();
                } catch (UnsatisfiedLinkError e) {
                    Log.w(TAG, "native notifyMcpStop not available: " + e.getMessage());
                }
            }
        }
    }
}