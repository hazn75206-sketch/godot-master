package org.godotengine.godot.notch;

import android.app.Activity;
import android.content.Context;
import android.os.Build;
import android.view.WindowManager;

/**
 * Hides the display cutout (notch) area by letterboxing the window, so the
 * Godot editor never draws under the notch. Toggleable at runtime.
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
							? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_NEVER
							: WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_DEFAULT;
					activity.getWindow().setAttributes(attrs);
				} catch (Exception ignored) {
				}
			}
		});
	}
}
