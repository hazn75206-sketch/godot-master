/**************************************************************************/
/*  GodotImportActivity.kt                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

package org.godotengine.editor

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.util.Log
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.TextView
import org.godotengine.godot.Godot
import org.godotengine.godot.GodotHost
import org.godotengine.godot.GodotLib
import org.godotengine.godot.utils.ProcessPhoenix
import java.io.File
import java.io.RandomAccessFile

/**
 * Runs an import-only Godot instance in a dedicated process and displays its progress.
 *
 * The engine is started headless, so this activity must not use GodotActivity/GodotFragment
 * (they require a render view). The UI here is plain Android, while all import work happens
 * in the engine.
 */
class GodotImportActivity : Activity(), GodotHost {
	companion object {
		private const val TAG = "GodotSafeImport"
		private const val EXTRA_PROJECT_PATH = "project_path"
		private const val LOG_FILE_NAME = "safe_import.log"
		private const val LOG_POLL_INTERVAL_MS = 200L
		private const val MAX_LOG_CHARS = 200_000
		private const val SUCCESS_RETURN_DELAY_MS = 1000L
	}

	private var godot: Godot? = null
	private val commandLine = mutableListOf<String>()

	private lateinit var titleView: TextView
	private lateinit var logScroll: ScrollView
	private lateinit var logView: TextView
	private lateinit var statusView: TextView
	private lateinit var progressBar: ProgressBar
	private lateinit var spinner: SafeImportSpinner
	private lateinit var exitButton: Button
	private lateinit var reloadButton: Button

	private var projectPath = ""
	private var logFile: File? = null
	private var logOffset = 0L
	private val pendingLog = StringBuilder()
	private val logHandler = Handler(Looper.getMainLooper())
	private val logPoller = object : Runnable {
		override fun run() {
			if (destroyed) {
				return
			}
			pollLog()
			logHandler.postDelayed(this, LOG_POLL_INTERVAL_MS)
		}
	}

	@Volatile private var importRunning = false
	@Volatile private var sawDone = false
	@Volatile private var destroyed = false

	private var totalFiles = 0
	private var currentFile = 0

	override fun onCreate(savedInstanceState: Bundle?) {
		super.onCreate(savedInstanceState)
		window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
		setContentView(R.layout.godot_import_layout)
		bindViews()

		projectPath = intent.getStringExtra(EXTRA_PROJECT_PATH).orEmpty()
		if (projectPath.isBlank()) {
			Log.e(TAG, "Missing project path")
			finish()
			return
		}

		titleView.text = getString(R.string.safe_import_title, File(projectPath).name)
		logFile = File(cacheDir, LOG_FILE_NAME)
		try {
			logFile?.writeText("")
		} catch (t: Throwable) {
			Log.w(TAG, "Unable to reset safe import log", t)
			logFile = File(cacheDir, "safe_import_${System.currentTimeMillis()}.log")
		}

		exitButton.setOnClickListener { onExitPressed() }
		reloadButton.setOnClickListener { onReloadPressed() }
		setStatus(getString(R.string.safe_import_starting))

		// Let the terminal screen draw before the native engine setup blocks the UI thread.
		window.decorView.post { startEngine() }
	}

	private fun bindViews() {
		titleView = findViewById(R.id.safe_import_title)
		logScroll = findViewById(R.id.safe_import_log_scroll)
		logView = findViewById(R.id.safe_import_log)
		statusView = findViewById(R.id.safe_import_status)
		progressBar = findViewById(R.id.safe_import_progress)
		spinner = findViewById(R.id.safe_import_spinner)
		exitButton = findViewById(R.id.safe_import_exit)
		reloadButton = findViewById(R.id.safe_import_reload)
	}

	private fun startEngine() {
		if (destroyed) {
			return
		}

		val logPath = logFile?.absolutePath ?: run {
			onImportFailed(getString(R.string.safe_import_failed))
			return
		}

		commandLine.clear()
		commandLine.addAll(
			listOf(
				"--headless",
				"--import",
				"--path",
				projectPath,
				"--log-file",
				logPath,
				"--safe-import",
			),
		)

		val engine = try {
			Godot.getInstance(applicationContext)
		} catch (t: Throwable) {
			Log.e(TAG, "Unable to access the Godot engine", t)
			null
		}
		godot = engine
		val initialized = try {
			engine?.initEngine(this, commandLine) == true
		} catch (t: Throwable) {
			Log.e(TAG, "Unable to initialize the Godot engine", t)
			false
		}

		if (!initialized) {
			onImportFailed(getString(R.string.safe_import_engine_failed))
			return
		}

		startLogPolling()
		startImportLoop()
	}

	private fun startImportLoop() {
		val engine = godot ?: return
		importRunning = true
		Thread({
			try {
				while (!destroyed) {
					val swapped = GodotLib.step()
					if (engine.runStatus == Godot.RunStatus.TERMINATING) {
						break
					}
					if (!swapped) {
						Thread.sleep(1L)
					}
				}
			} catch (t: Throwable) {
				Log.e(TAG, "Safe import loop failed", t)
			} finally {
				importRunning = false
				try {
					GodotLib.ondestroy()
				} catch (t: Throwable) {
					Log.w(TAG, "Engine cleanup failed", t)
				}
				runOnUiThread { onImportLoopEnded() }
			}
		}, "GodotSafeImport").apply {
			isDaemon = true
			start()
		}
	}

	private fun onImportLoopEnded() {
		if (destroyed) {
			return
		}

		stopLogPolling()
		while (true) {
			val lines = try {
				readNewLogLines()
			} catch (t: Throwable) {
				Log.w(TAG, "Unable to read final safe import log", t)
				emptyList()
			}
			if (lines.isEmpty()) {
				break
			}
			for (line in lines) {
				appendLogLine(line)
				parseLine(line)
			}
		}

		if (sawDone) {
			setStatus(getString(R.string.safe_import_done))
			progressBar.progress = progressBar.max
			spinner.advance()
			Handler(Looper.getMainLooper()).postDelayed({
				if (!destroyed) {
					finishAndKillProcess()
				}
			}, SUCCESS_RETURN_DELAY_MS)
		} else {
			setStatus(getString(R.string.safe_import_failed))
			exitButton.isEnabled = true
			reloadButton.isEnabled = true
		}
	}

	private fun onImportFailed(message: String) {
		importRunning = false
		setStatus(message)
		appendLogLine("ERROR: $message")
		reloadButton.isEnabled = true
		exitButton.isEnabled = true
	}

	private fun startLogPolling() {
		logHandler.removeCallbacks(logPoller)
		logHandler.post(logPoller)
	}

	private fun stopLogPolling() {
		logHandler.removeCallbacks(logPoller)
	}

	private fun pollLog() {
		val lines = try {
			readNewLogLines()
		} catch (t: Throwable) {
			Log.w(TAG, "Unable to read safe import log", t)
			emptyList()
		}
		for (line in lines) {
			appendLogLine(line)
			parseLine(line)
		}
	}

	private fun readNewLogLines(): List<String> {
		val file = logFile
		if (file == null || !file.exists()) {
			return emptyList()
		}

		val lines = ArrayList<String>()
		val raf = RandomAccessFile(file, "r")
		try {
			val length = raf.length()
			if (length < logOffset) {
				logOffset = 0L
				pendingLog.setLength(0)
			}
			if (length > logOffset) {
				raf.seek(logOffset)
				val toRead = (length - logOffset).coerceAtMost(64L * 1024L).toInt()
				val bytes = ByteArray(toRead)
				val read = raf.read(bytes)
				if (read > 0) {
					logOffset += read
					val chunk = String(bytes, 0, read, Charsets.UTF_8)
					pendingLog.append(chunk)
					var newline = pendingLog.indexOf("\n")
					while (newline >= 0) {
						lines.add(pendingLog.substring(0, newline))
						pendingLog.delete(0, newline + 1)
						newline = pendingLog.indexOf("\n")
					}
				}
			}
		} finally {
			raf.close()
		}
		return lines
	}

	private fun appendLogLine(line: String) {
		logView.append(line)
		logView.append("\n")

		if (logView.length() > MAX_LOG_CHARS) {
			val text = logView.text.toString()
			logView.text = text.substring(text.length - MAX_LOG_CHARS + 20_000)
		}

		logScroll.post { logScroll.fullScroll(View.FOCUS_DOWN) }
	}

	private fun parseLine(line: String) {
		val clean = line.trimEnd('\r')

		when {
			clean.startsWith("SAFE_IMPORT_BEGIN ") -> {
				sawDone = false
				totalFiles = clean.removePrefix("SAFE_IMPORT_BEGIN ").trim().toIntOrNull() ?: 0
				currentFile = 0
				progressBar.max = totalFiles.coerceAtLeast(1)
				progressBar.progress = 0
				setStatus(getString(R.string.safe_import_progress, 0, totalFiles, ""))
			}

			clean.startsWith("SAFE_IMPORT_PROGRESS ") -> {
				val payload = clean.removePrefix("SAFE_IMPORT_PROGRESS ").trim()
				val slash = payload.indexOf('/')
				if (slash <= 0) {
					return
				}

				val current = payload.substring(0, slash).toIntOrNull() ?: currentFile
				val rest = payload.substring(slash + 1)
				val space = rest.indexOf(' ')
				val total = (if (space > 0) rest.substring(0, space) else rest).toIntOrNull() ?: totalFiles
				val file = if (space > 0) rest.substring(space + 1) else ""

				totalFiles = total
				currentFile = current
				progressBar.max = total.coerceAtLeast(1)
				progressBar.progress = current.coerceIn(0, progressBar.max)
				setStatus(getString(R.string.safe_import_progress, current, total, file))
				spinner.advance()
			}

			clean == "SAFE_IMPORT_DONE" -> {
				sawDone = true
				setStatus(getString(R.string.safe_import_done))
				progressBar.progress = progressBar.max
				spinner.advance()
			}

			clean.startsWith("SAFE_IMPORT_FILE_FAILED ") -> {
				val file = clean.removePrefix("SAFE_IMPORT_FILE_FAILED ").trim()
				setStatus(getString(R.string.safe_import_file_failed, file))
			}
		}
	}

	private fun setStatus(text: String) {
		statusView.text = text
	}

	private fun onExitPressed() {
		if (!importRunning) {
			finishAndKillProcess()
			return
		}

		AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
			.setTitle(R.string.safe_import_exit_confirm_title)
			.setMessage(R.string.safe_import_exit_confirm_message)
			.setPositiveButton(android.R.string.yes) { _, _ -> stopAndExit() }
			.setNegativeButton(android.R.string.no, null)
			.show()
	}

	private fun stopAndExit() {
		stopLogPolling()
		destroyed = true
		finishAndRemoveTask()
		Process.killProcess(Process.myPid())
		Runtime.getRuntime().exit(0)
	}

	private fun finishAndKillProcess() {
		destroyed = true
		stopLogPolling()
		finishAndRemoveTask()
		Handler(Looper.getMainLooper()).postDelayed({
			Process.killProcess(Process.myPid())
		}, 150L)
	}

	private fun onReloadPressed() {
		if (!importRunning) {
			restartImport()
			return
		}

		AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
			.setTitle(R.string.safe_import_reload_confirm_title)
			.setMessage(R.string.safe_import_reload_confirm_message)
			.setPositiveButton(android.R.string.yes) { _, _ -> restartImport() }
			.setNegativeButton(android.R.string.no, null)
			.show()
	}

	private fun restartImport() {
		stopLogPolling()
		destroyed = true
		val restartIntent = Intent(this, GodotImportActivity::class.java).apply {
			putExtra(EXTRA_PROJECT_PATH, projectPath)
			addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK)
		}
		ProcessPhoenix.triggerRebirth(this, restartIntent)
	}

	@Suppress("DEPRECATION")
	override fun onBackPressed() {
		onExitPressed()
	}

	override fun onDestroy() {
		destroyed = true
		stopLogPolling()
		window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
		super.onDestroy()
	}

	override fun getActivity(): Activity = this

	override fun getGodot(): Godot? = godot

	override fun getCommandLine(): MutableList<String> = commandLine

	override fun onGodotSetupCompleted() {
		runOnUiThread {
			if (!destroyed) {
				setStatus(getString(R.string.safe_import_running))
			}
		}
	}

	override fun onGodotMainLoopStarted() {
		runOnUiThread {
			if (!destroyed) {
				setStatus(getString(R.string.safe_import_running))
			}
		}
	}
}
