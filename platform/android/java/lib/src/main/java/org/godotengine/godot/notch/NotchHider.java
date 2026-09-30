package org.godotengine.godot.notch;

import android.app.Activity;
import android.content.Context;
import android.os.Build;
import android.view.WindowManager;

/**
 * Uses the full display including the display cutout (notch) area, so the
 * Godot editor is truly fullscreen with no letterbox bars. The editor UI
 * itself stays clear of the notch thanks to Godot's cutout padding.
 * Toggleable at runtime.
 *
 * Called from native code (modules/godot_mcp/notch_hider.cpp).
 */
public class NotchHider {
	public static void setHidden(Context context, final boolean hidden) {
		if (!(context instanceof Activity) || Build.VERSION.SDK_INT < 28) {
			return;
		}
		final Activity activity = (Activity) context;
		activity.runOnUiThread(new Runnable() {
			@Override
			public void run() {
				try {
					WindowManager.LayoutParams attrs = activity.getWindow().getAttributes();
					attrs.layoutInDisplayCutoutMode = hidden
							? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
							: WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_DEFAULT;
					activity.getWindow().setAttributes(attrs);
				} catch (Exception ignored) {
				}
			}
		});
	}
}
