package org.godotengine.godot.opencode;

import android.app.Activity;
import android.content.Context;
import android.util.Log;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Map;
import java.util.concurrent.TimeUnit;
import java.util.zip.GZIPInputStream;

/**
 * Runs the real opencode binary bundled in the APK, on-device.
 *
 * First run: extracts assets/opencode/opencode.gz to the app files dir
 * (gunzip + chmod 700) and seeds an opencode.json config (free model +
 * MCP loopback to the in-app Godot MCP server).
 *
 * Called from native code (modules/godot_mcp/opencode_runner.cpp).
 */
public class OpencodeRunner {
	private static final String TAG = "GodotOpencode";
	private static final String ASSET_PATH = "opencode/opencode.gz";
	private static final String BIN_NAME = "opencode";
	private static String cachedVersion = null;

	private static File homeDir(Context context) {
		return new File(context.getFilesDir(), "opencode-home");
	}

	private static File binFile(Context context) {
		return new File(homeDir(context), BIN_NAME);
	}

	private static String runCmd(String[] cmd, Map<String, String> envExtra, long timeoutSec) {
		try {
			ProcessBuilder pb = new ProcessBuilder(cmd);
			pb.redirectErrorStream(false);
			if (envExtra != null) {
				pb.environment().putAll(envExtra);
			}
			Process p = pb.start();
			// Close stdin immediately (non-interactive).
			try {
				p.getOutputStream().close();
			} catch (Exception ignored) {
			}
			final ByteArrayOutputStream outBuf = new ByteArrayOutputStream();
			final ByteArrayOutputStream errBuf = new ByteArrayOutputStream();
			Thread tOut = pipeThread(p.getInputStream(), outBuf);
			Thread tErr = pipeThread(p.getErrorStream(), errBuf);
			boolean finished = p.waitFor(timeoutSec, TimeUnit.SECONDS);
			if (!finished) {
				p.destroyForcibly();
				return new JSONObject().put("output", "").put("error", "timeout").toString();
			}
			tOut.join(5000);
			tErr.join(5000);
			JSONObject r = new JSONObject();
			r.put("output", new String(outBuf.toByteArray(), StandardCharsets.UTF_8));
			String err = new String(errBuf.toByteArray(), StandardCharsets.UTF_8);
			if (p.exitValue() != 0 && err.isEmpty()) {
				err = "exit code " + p.exitValue();
			}
			r.put("error", err);
			return r.toString();
		} catch (Exception e) {
			try {
				return new JSONObject().put("output", "").put("error", "runner: " + e.getMessage()).toString();
			} catch (Exception ignored) {
				return "{\"output\":\"\",\"error\":\"runner failed\"}";
			}
		}
	}

	private static Thread pipeThread(final InputStream in, final OutputStream out) {
		Thread t = new Thread(new Runnable() {
			@Override
			public void run() {
				try {
					byte[] buf = new byte[8192];
					int n;
					while ((n = in.read(buf)) != -1) {
						out.write(buf, 0, n);
					}
				} catch (Exception ignored) {
				}
			}
		});
		t.setDaemon(true);
		t.start();
		return t;
	}

	/** Extract the bundled binary on first run. Returns the binary path or "". */
	public static String extractBinary(Context context) {
		try {
			File home = homeDir(context);
			if (!home.isDirectory() && !home.mkdirs()) {
				return "";
			}
			File bin = binFile(context);
			if (bin.isFile() && bin.canExecute() && bin.length() > 1000000) {
				return bin.getAbsolutePath();
			}
			InputStream asset = context.getAssets().open(ASSET_PATH);
			GZIPInputStream gz = new GZIPInputStream(asset);
			FileOutputStream fos = new FileOutputStream(bin);
			byte[] buf = new byte[65536];
			int n;
			while ((n = gz.read(buf)) != -1) {
				fos.write(buf, 0, n);
			}
			fos.getFD().sync();
			fos.close();
			gz.close();
			// chmod 700 (setExecutable may be enough, belt and suspenders).
			bin.setExecutable(true, true);
			bin.setReadable(true, true);
			bin.setWritable(true, true);
			try {
				new ProcessBuilder("chmod", "700", bin.getAbsolutePath()).start().waitFor(10, TimeUnit.SECONDS);
			} catch (Exception ignored) {
			}
			if (bin.isFile() && bin.canExecute()) {
				return bin.getAbsolutePath();
			}
			return "";
		} catch (Exception e) {
			Log.w(TAG, "extractBinary failed: " + e.getMessage());
			return "";
		}
	}

	/** Seed opencode.json (free model + MCP loopback). Returns config path or "". */
	public static String ensureConfig(Context context, String model, String mcpUrl) {
		try {
			File home = homeDir(context);
			if (!home.isDirectory() && !home.mkdirs()) {
				return "";
			}
			File cfg = new File(home, "opencode.json");
			if (!cfg.isFile()) {
				JSONObject mcpGodot = new JSONObject();
				mcpGodot.put("type", "remote");
				mcpGodot.put("url", mcpUrl);
				mcpGodot.put("enabled", true);
				JSONObject mcp = new JSONObject();
				mcp.put("godot", mcpGodot);
				JSONObject root = new JSONObject();
				root.put("model", model);
				root.put("mcp", mcp);
				FileOutputStream fos = new FileOutputStream(cfg);
				fos.write(root.toString(2).getBytes(StandardCharsets.UTF_8));
				fos.close();
			}
			File work = new File(home, "work");
			if (!work.isDirectory()) {
				work.mkdirs();
			}
			return cfg.getAbsolutePath();
		} catch (Exception e) {
			Log.w(TAG, "ensureConfig failed: " + e.getMessage());
			return "";
		}
	}

	/** Blocking `opencode --version`. Returns version string or "". Result cached. */
	public static String getVersion(Context context) {
		if (cachedVersion != null) {
			return cachedVersion;
		}
		String bin = extractBinary(context);
		if (bin.isEmpty()) {
			return "";
		}
		try {
			java.util.HashMap<String, String> env = new java.util.HashMap<String, String>();
			env.put("HOME", homeDir(context).getAbsolutePath());
			String res = runCmd(new String[]{ bin, "--version" }, env, 30);
			JSONObject o = new JSONObject(res);
			String out = o.optString("output", "").trim();
			if (!out.isEmpty()) {
				// First line usually holds the version.
				int nl = out.indexOf('\n');
				String v = nl == -1 ? out : out.substring(0, nl).trim();
				if (!v.isEmpty()) {
					cachedVersion = v;
				}
				return v;
			}
			return "";
		} catch (Exception e) {
			return "";
		}
	}

	/**
	 * Staged diagnostics for "opencode binary missing".
	 * Returns JSON: asset_list, asset_bytes/asset_error, home, home_usable,
	 * free_mb, bin_exists, bin_size, bin_executable, extract_result,
	 * version, error.
	 */
	public static String diagnose(Context context) {
		JSONObject d = new JSONObject();
		try {
			try {
				String[] list = context.getAssets().list("opencode");
				StringBuilder sb = new StringBuilder();
				if (list != null) {
					for (int i = 0; i < list.length; i++) {
						if (i > 0) {
							sb.append(",");
						}
						sb.append(list[i]);
					}
				}
				d.put("asset_list", sb.toString());
			} catch (Exception e) {
				d.put("asset_error", "list: " + String.valueOf(e.getMessage()));
			}
			try {
				InputStream a = context.getAssets().open(ASSET_PATH);
				long total = 0;
				byte[] b = new byte[65536];
				int n;
				while ((n = a.read(b)) != -1) {
					total += n;
				}
				a.close();
				d.put("asset_bytes", total);
			} catch (Exception e) {
				d.put("asset_error", "open: " + String.valueOf(e.getMessage()));
			}
			File home = homeDir(context);
			d.put("home", home.getAbsolutePath());
			boolean usable = home.isDirectory() || home.mkdirs();
			d.put("home_usable", usable);
			try {
				d.put("free_mb", home.getUsableSpace() / 1048576L);
			} catch (Exception ignored) {
			}
			File bin = binFile(context);
			d.put("bin_exists", bin.isFile());
			d.put("bin_size", bin.isFile() ? bin.length() : 0);
			d.put("bin_executable", bin.canExecute());
			String p = extractBinary(context);
			d.put("extract_result", p.isEmpty() ? "FAILED" : "OK");
			if (!p.isEmpty()) {
				d.put("version", getVersion(context));
			}
			return d.toString();
		} catch (Exception e) {
			try {
				d.put("error", String.valueOf(e.getMessage()));
				return d.toString();
			} catch (Exception ignored) {
				return "{\"error\":\"diagnose failed\"}";
			}
		}
	}

	/** Blocking `opencode run`. Returns JSON {output, error}. */
	public static String runPrompt(Context context, String prompt, String model, String mcpUrl, int timeoutSec) {
		String bin = extractBinary(context);
		if (bin.isEmpty()) {
			return "{\"output\":\"\",\"error\":\"opencode binary missing\"}";
		}
		ensureConfig(context, model, mcpUrl);
		File home = homeDir(context);
		File work = new File(home, "work");
		try {
			java.util.HashMap<String, String> env = new java.util.HashMap<String, String>();
			env.put("HOME", home.getAbsolutePath());
			String effModel = model;
			if (effModel != null && !effModel.contains("/")) {
				effModel = "opencode/" + effModel;
			}
			return runCmd(new String[]{ bin, "run", prompt, "-m", effModel, "--format", "json", "--dir", work.getAbsolutePath() }, env, Math.max(timeoutSec, 30));
		} catch (Exception e) {
			return "{\"output\":\"\",\"error\":\"run failed: " + e.getMessage() + "\"}";
		}
	}

	/** True if the bundled binary is extracted and executable. */
	public static boolean isInstalled(Context context) {
		File bin = binFile(context);
		return bin.isFile() && bin.canExecute();
	}
}
