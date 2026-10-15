package org.godotengine.godot.notch;

import android.app.Activity;
import android.content.Context;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.view.WindowManager;

import org.godotengine.godot.Godot;
import org.godotengine.godot.GodotActivity;

/**
 * Uses the full display including the display cutout (notch) area, so the
 * Godot editor is truly fullscreen with no letterbox bars.
 *
 * Window flag saja tidak cukup: Godot.kt memberi padding sebesar area poni
 * ke tampilan editor. Jadi selain SHORT_EDGES, modul ini juga menyalakan
 * edge-to-edge + setIgnoreDisplayCutout agar padding poni jadi nol.
 * Diterapkan sekali saat startup editor (perubahan setting butuh restart).
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
				applyGodotPart(activity, hidden, 0);
			}
		});
	}

	// Instance Godot bisa belum ada saat apply startup; coba ulang sampai ada.
	private static void applyGodotPart(final Activity activity, final boolean hidden, final int attempt) {
		Godot godot = null;
		try {
			if (activity instanceof GodotActivity) {
				godot = ((GodotActivity) activity).getGodot();
			}
		} catch (Exception ignored) {
		}
		if (godot != null) {
			try {
				final Godot g = godot;
				activity.runOnUiThread(new Runnable() {
					@Override
					public void run() {
						try {
							g.setIgnoreDisplayCutout(hidden);
							g.enableEdgeToEdge(hidden, true);
						} catch (Exception ignored) {
						}
					}
				});
			} catch (Exception ignored) {
			}
			return;
		}
		if (attempt < 20) {
			new Handler(Looper.getMainLooper()).postDelayed(new Runnable() {
				@Override
				public void run() {
					applyGodotPart(activity, hidden, attempt + 1);
				}
			}, 500);
		}
	}
}
