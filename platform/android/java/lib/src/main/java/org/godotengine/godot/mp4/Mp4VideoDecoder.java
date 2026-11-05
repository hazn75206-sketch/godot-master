package org.godotengine.godot.mp4;

import android.graphics.Rect;
import android.media.AudioFormat;
import android.media.Image;
import android.media.MediaCodec;
import android.media.MediaDataSource;
import android.media.MediaExtractor;
import android.media.MediaFormat;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;

/**
 * MP4 (HEVC/H.264/AAC) decoder backed by Android's MediaCodec.
 *
 * The whole file is kept in memory and exposed through MediaDataSource so it
 * does not matter whether the original file lives in an APK asset or on the
 * device filesystem. All public methods are called from the Godot main thread.
 */
public final class Mp4VideoDecoder implements MediaDataSource {
	private static final long NO_TIMEOUT_US = 0;
	private static final long VIDEO_LOOKAHEAD_US = 100_000;
	private static final long AUDIO_LOOKAHEAD_US = 1_000_000;
	private static final int AUDIO_OUT_CAPACITY_FRAMES = 8192;

	private final byte[] source;

	private MediaExtractor videoExtractor;
	private MediaExtractor audioExtractor;
	private MediaCodec videoCodec;
	private MediaCodec audioCodec;
	private MediaFormat videoFormat;
	private MediaFormat audioFormat;
	private String videoMime;
	private String audioMime;

	private int videoTrack = -1;
	private int audioTrack = -1;

	private int width;
	private int height;
	private int rotation;
	private long durationUs;

	private int channels;
	private int sampleRate;

	private boolean playing;
	private boolean paused;
	private boolean finished;
	private boolean videoInputDone;
	private boolean videoOutputDone;
	private boolean audioInputDone;
	private boolean audioOutputDone;

	private long positionUs;

	private final MediaCodec.BufferInfo videoInfo = new MediaCodec.BufferInfo();
	private final MediaCodec.BufferInfo audioInfo = new MediaCodec.BufferInfo();

	private ByteBuffer yuvBuffer;
	private int frameWidth;
	private int frameHeight;
	private int frameSerial;

	private ByteBuffer audioBuffer;
	private int audioOutCapacityFrames;
	private boolean pcmFloat;

	private float[] audioRing;
	private int audioRead;
	private int audioWrite;
	private int audioCount;

	public Mp4VideoDecoder(byte[] data) throws IOException {
		source = data;
		setup();
	}

	private void setup() throws IOException {
		videoExtractor = new MediaExtractor();
		videoExtractor.setDataSource(this);

		for (int i = 0; i < videoExtractor.getTrackCount(); i++) {
			MediaFormat format = videoExtractor.getTrackFormat(i);
			String mime = format.getString(MediaFormat.KEY_MIME);
			if (mime != null && mime.startsWith("video/")) {
				videoTrack = i;
				break;
			}
		}
		if (videoTrack < 0) {
			throw new IOException("File tidak punya track video");
		}

		videoFormat = videoExtractor.getTrackFormat(videoTrack);
		videoExtractor.selectTrack(videoTrack);
		videoMime = videoFormat.getString(MediaFormat.KEY_MIME);
		width = videoFormat.getInteger(MediaFormat.KEY_WIDTH);
		height = videoFormat.getInteger(MediaFormat.KEY_HEIGHT);
		rotation = videoFormat.containsKey(MediaFormat.KEY_ROTATION) ? videoFormat.getInteger(MediaFormat.KEY_ROTATION) : 0;
		durationUs = videoFormat.containsKey(MediaFormat.KEY_DURATION) ? videoFormat.getLong(MediaFormat.KEY_DURATION) : 0;

		videoCodec = MediaCodec.createDecoderByType(videoMime);
		videoCodec.configure(videoFormat, null, null, 0);
		videoCodec.start();

		audioExtractor = new MediaExtractor();
		audioExtractor.setDataSource(this);
		for (int i = 0; i < audioExtractor.getTrackCount(); i++) {
			MediaFormat format = audioExtractor.getTrackFormat(i);
			String mime = format.getString(MediaFormat.KEY_MIME);
			if (mime != null && mime.startsWith("audio/")) {
				audioTrack = i;
				break;
			}
		}

		if (audioTrack >= 0) {
			audioFormat = audioExtractor.getTrackFormat(audioTrack);
			audioExtractor.selectTrack(audioTrack);
			audioMime = audioFormat.getString(MediaFormat.KEY_MIME);
			channels = audioFormat.containsKey(MediaFormat.KEY_CHANNEL_COUNT) ? audioFormat.getInteger(MediaFormat.KEY_CHANNEL_COUNT) : 2;
			sampleRate = audioFormat.containsKey(MediaFormat.KEY_SAMPLE_RATE) ? audioFormat.getInteger(MediaFormat.KEY_SAMPLE_RATE) : 48000;
			if (audioFormat.containsKey(MediaFormat.KEY_DURATION)) {
				durationUs = Math.max(durationUs, audioFormat.getLong(MediaFormat.KEY_DURATION));
			}

			audioCodec = MediaCodec.createDecoderByType(audioMime);
			audioCodec.configure(audioFormat, null, null, 0);
			audioCodec.start();

			audioRing = new float[sampleRate * channels * 2];
			audioOutCapacityFrames = AUDIO_OUT_CAPACITY_FRAMES;
			audioBuffer = ByteBuffer.allocateDirect(audioOutCapacityFrames * channels * Float.BYTES);
			audioBuffer.order(ByteOrder.nativeOrder());
		} else {
			channels = 0;
			sampleRate = 0;
			audioOutCapacityFrames = 0;
		}

		int chromaWidth = (width + 1) / 2;
		int chromaHeight = (height + 1) / 2;
		yuvBuffer = ByteBuffer.allocateDirect(width * height * 2 + chromaWidth * chromaHeight * 2);
		yuvBuffer.order(ByteOrder.nativeOrder());
		frameWidth = width;
		frameHeight = height;
	}

	// MediaDataSource implementation.
	@Override
	public int readAt(long position, byte[] buffer, int offset, int size) {
		if (position >= source.length) {
			return -1;
		}
		int length = (int) Math.min((long) size, (long) source.length - position);
		if (length <= 0) {
			return -1;
		}
		System.arraycopy(source, (int) position, buffer, offset, length);
		return length;
	}

	@Override
	public long getSize() {
		return source.length;
	}

	@Override
	public void close() {
	}

	public void play() {
		if (finished || (durationUs > 0 && positionUs >= durationUs)) {
			seekInternal(0);
		}
		playing = true;
		paused = false;
	}

	public void stopPlayback() {
		playing = false;
		paused = false;
		seekInternal(0);
	}

	public void setPaused(boolean value) {
		paused = value;
	}

	public boolean isPlaying() {
		return playing && !finished;
	}

	public void seekTo(double seconds) {
		long us = (long) (seconds * 1_000_000.0);
		if (us < 0) {
			us = 0;
		}
		if (durationUs > 0 && us > durationUs) {
			us = durationUs;
		}
		seekInternal(us);
	}

	public double getDuration() {
		return durationUs / 1_000_000.0;
	}

	public double getPosition() {
		return positionUs / 1_000_000.0;
	}

	public int getVideoWidth() {
		return width;
	}

	public int getVideoHeight() {
		return height;
	}

	public int getRotation() {
		return rotation;
	}

	public int getChannels() {
		return channels;
	}

	public int getSampleRate() {
		return sampleRate;
	}

	public int getFrameWidth() {
		return frameWidth;
	}

	public int getFrameHeight() {
		return frameHeight;
	}

	public int getFrameSerial() {
		return frameSerial;
	}

	public ByteBuffer getYuvBuffer() {
		return yuvBuffer;
	}

	public ByteBuffer getAudioBuffer() {
		return audioBuffer;
	}

	/**
	 * Advances the playback clock and decodes whatever is needed.
	 *
	 * @return true when a new video frame was written to the YUV buffer.
	 */
	public boolean update(double delta) {
		if (!playing || paused || finished) {
			return false;
		}

		positionUs += (long) (delta * 1_000_000.0);
		if (durationUs > 0 && positionUs > durationUs) {
			positionUs = durationUs;
		}

		boolean newFrame = decodeVideo();
		fillAudio();

		if (videoOutputDone && (audioCodec == null || audioOutputDone) && (durationUs <= 0 || positionUs >= durationUs)) {
			finished = true;
		}

		return newFrame;
	}

	public int mixAudio(int frames) {
		if (channels <= 0 || audioBuffer == null || frames <= 0) {
			return 0;
		}
		if (frames > audioOutCapacityFrames) {
			frames = audioOutCapacityFrames;
		}

		audioBuffer.clear();
		FloatBuffer out = audioBuffer.asFloatBuffer();

		int availableFrames = audioCount / channels;
		int copyFrames = Math.min(frames, availableFrames);
		int copySamples = copyFrames * channels;
		for (int i = 0; i < copySamples; i++) {
			out.put(audioRing[audioRead]);
			audioRead = (audioRead + 1) % audioRing.length;
		}
		audioCount -= copySamples;

		for (int i = copySamples; i < frames * channels; i++) {
			out.put(0.0f);
		}

		return frames;
	}

	public void release() {
		playing = false;
		paused = false;

		if (videoCodec != null) {
			try {
				videoCodec.stop();
			} catch (Exception ignored) {
			}
			try {
				videoCodec.release();
			} catch (Exception ignored) {
			}
			videoCodec = null;
		}
		if (audioCodec != null) {
			try {
				audioCodec.stop();
			} catch (Exception ignored) {
			}
			try {
				audioCodec.release();
			} catch (Exception ignored) {
			}
			audioCodec = null;
		}
		if (videoExtractor != null) {
			try {
				videoExtractor.release();
			} catch (Exception ignored) {
			}
			videoExtractor = null;
		}
		if (audioExtractor != null) {
			try {
				audioExtractor.release();
			} catch (Exception ignored) {
			}
			audioExtractor = null;
		}
	}

	private void seekInternal(long us) {
		positionUs = us;
		finished = false;
		videoInputDone = false;
		videoOutputDone = false;
		audioInputDone = false;
		audioOutputDone = false;
		audioRead = 0;
		audioWrite = 0;
		audioCount = 0;

		videoCodec = recreateCodec(videoCodec, videoFormat, videoMime);
		if (audioFormat != null) {
			audioCodec = recreateCodec(audioCodec, audioFormat, audioMime);
		}

		if (videoExtractor != null) {
			videoExtractor.seekTo(us, MediaExtractor.SEEK_TO_PREVIOUS_SYNC);
		}
		if (audioExtractor != null && audioTrack >= 0) {
			audioExtractor.seekTo(us, MediaExtractor.SEEK_TO_PREVIOUS_SYNC);
		}
	}

	private MediaCodec recreateCodec(MediaCodec codec, MediaFormat format, String mime) {
		if (format == null || mime == null) {
			return codec;
		}
		if (codec != null) {
			try {
				codec.stop();
			} catch (Exception ignored) {
			}
			try {
				codec.release();
			} catch (Exception ignored) {
			}
		}
		try {
			MediaCodec fresh = MediaCodec.createDecoderByType(mime);
			fresh.configure(format, null, null, 0);
			fresh.start();
			return fresh;
		} catch (Exception e) {
			return null;
		}
	}

	private boolean decodeVideo() {
		if (videoCodec == null || videoOutputDone) {
			return false;
		}

		feedVideo();

		boolean newFrame = false;
		while (true) {
			int index = videoCodec.dequeueOutputBuffer(videoInfo, NO_TIMEOUT_US);
			if (index == MediaCodec.INFO_TRY_AGAIN_LATER) {
				break;
			}
			if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
				continue;
			}
			if (index == MediaCodec.INFO_OUTPUT_BUFFERS_CHANGED) {
				continue;
			}
			if (index < 0) {
				break;
			}

			if ((videoInfo.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
				videoOutputDone = true;
			}

			if (videoInfo.size > 0) {
				Image image = null;
				try {
					image = videoCodec.getOutputImage(index);
				} catch (Exception ignored) {
				}
				if (image != null) {
					copyImage(image);
					image.close();
					newFrame = true;
				}
			}

			videoCodec.releaseOutputBuffer(index, false);

			if (videoOutputDone) {
				break;
			}
		}

		return newFrame;
	}

	private void feedVideo() {
		if (videoInputDone || videoExtractor == null) {
			return;
		}

		for (int i = 0; i < 16; i++) {
			long sampleTime = videoExtractor.getSampleTime();
			if (sampleTime < 0) {
				queueVideoEndOfStream();
				return;
			}
			if (sampleTime > positionUs + VIDEO_LOOKAHEAD_US) {
				return;
			}

			int index = videoCodec.dequeueInputBuffer(NO_TIMEOUT_US);
			if (index < 0) {
				return;
			}

			ByteBuffer buffer = videoCodec.getInputBuffer(index);
			if (buffer == null) {
				return;
			}
			buffer.clear();

			int size = videoExtractor.readSampleData(buffer, 0);
			if (size < 0) {
				queueVideoEndOfStream();
				return;
			}

			videoCodec.queueInputBuffer(index, 0, size, sampleTime, 0);
			videoExtractor.advance();
		}
	}

	private void queueVideoEndOfStream() {
		if (videoInputDone) {
			return;
		}
		int index = videoCodec.dequeueInputBuffer(NO_TIMEOUT_US);
		if (index >= 0) {
			videoCodec.queueInputBuffer(index, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
			videoInputDone = true;
		}
	}

	private void fillAudio() {
		if (audioCodec == null || audioOutputDone) {
			return;
		}

		int target = Math.min(audioRing.length, sampleRate * channels);
		int guard = 0;
		while (audioCount < target && guard++ < 8) {
			feedAudio();
			drainAudio();
		}
	}

	private void feedAudio() {
		if (audioInputDone || audioExtractor == null) {
			return;
		}

		for (int i = 0; i < 32; i++) {
			if (audioCount > audioRing.length - sampleRate * channels) {
				return;
			}

			long sampleTime = audioExtractor.getSampleTime();
			if (sampleTime < 0) {
				queueAudioEndOfStream();
				return;
			}
			if (sampleTime > positionUs + AUDIO_LOOKAHEAD_US) {
				return;
			}

			int index = audioCodec.dequeueInputBuffer(NO_TIMEOUT_US);
			if (index < 0) {
				return;
			}

			ByteBuffer buffer = audioCodec.getInputBuffer(index);
			if (buffer == null) {
				return;
			}
			buffer.clear();

			int size = audioExtractor.readSampleData(buffer, 0);
			if (size < 0) {
				queueAudioEndOfStream();
				return;
			}

			audioCodec.queueInputBuffer(index, 0, size, sampleTime, 0);
			audioExtractor.advance();
		}
	}

	private void queueAudioEndOfStream() {
		if (audioInputDone) {
			return;
		}
		int index = audioCodec.dequeueInputBuffer(NO_TIMEOUT_US);
		if (index >= 0) {
			audioCodec.queueInputBuffer(index, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
			audioInputDone = true;
		}
	}

	private void drainAudio() {
		while (true) {
			int index = audioCodec.dequeueOutputBuffer(audioInfo, NO_TIMEOUT_US);
			if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
				MediaFormat outFormat = audioCodec.getOutputFormat();
				if (outFormat.containsKey(MediaFormat.KEY_PCM_ENCODING)) {
					pcmFloat = outFormat.getInteger(MediaFormat.KEY_PCM_ENCODING) == AudioFormat.ENCODING_PCM_FLOAT;
				}
				continue;
			}
			if (index == MediaCodec.INFO_OUTPUT_BUFFERS_CHANGED) {
				continue;
			}
			if (index == MediaCodec.INFO_TRY_AGAIN_LATER) {
				return;
			}
			if (index < 0) {
				return;
			}

			if ((audioInfo.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
				audioOutputDone = true;
			}

			if (audioInfo.size > 0) {
				ByteBuffer buffer = audioCodec.getOutputBuffer(index);
				if (buffer != null) {
					buffer.position(audioInfo.offset);
					buffer.limit(audioInfo.offset + audioInfo.size);
					appendPcm(buffer);
				}
			}

			audioCodec.releaseOutputBuffer(index, false);

			if (audioOutputDone) {
				return;
			}
		}
	}

	private void appendPcm(ByteBuffer buffer) {
		int bytesPerSample = pcmFloat ? 4 : 2;
		int totalSamples = buffer.remaining() / bytesPerSample;
		int capacity = audioRing.length;

		for (int i = 0; i < totalSamples; i++) {
			float value;
			if (pcmFloat) {
				value = buffer.getFloat();
			} else {
				value = buffer.getShort() / 32768.0f;
			}
			if (audioCount >= capacity) {
				break;
			}
			audioRing[audioWrite] = value;
			audioWrite = (audioWrite + 1) % capacity;
			audioCount++;
		}
	}

	private void copyImage(Image image) {
		Rect crop = image.getCropRect();
		int cropLeft = 0;
		int cropTop = 0;
		int cropWidth = image.getWidth();
		int cropHeight = image.getHeight();
		if (crop != null) {
			cropLeft = crop.left;
			cropTop = crop.top;
			cropWidth = crop.width();
			cropHeight = crop.height();
		}
		if (cropWidth <= 0 || cropHeight <= 0) {
			return;
		}

		int chromaWidth = (cropWidth + 1) / 2;
		int chromaHeight = (cropHeight + 1) / 2;
		int needed = cropWidth * cropHeight + chromaWidth * chromaHeight * 2;
		if (yuvBuffer.capacity() < needed) {
			return;
		}

		Image.Plane[] planes = image.getPlanes();
		ByteBuffer yPlane = planes[0].getBuffer();
		ByteBuffer uPlane = planes[1].getBuffer();
		ByteBuffer vPlane = planes[2].getBuffer();

		int yRowStride = planes[0].getRowStride();
		int yPixelStride = planes[0].getPixelStride();
		int uRowStride = planes[1].getRowStride();
		int uPixelStride = planes[1].getPixelStride();
		int vRowStride = planes[2].getRowStride();
		int vPixelStride = planes[2].getPixelStride();

		yuvBuffer.clear();

		int yRowOffset = cropTop * yRowStride;
		int yColumnOffset = cropLeft * yPixelStride;
		for (int row = 0; row < cropHeight; row++) {
			int base = yRowOffset + row * yRowStride + yColumnOffset;
			for (int column = 0; column < cropWidth; column++) {
				yuvBuffer.put(yPlane.get(base + column * yPixelStride));
			}
		}

		int uRowOffset = (cropTop / 2) * uRowStride;
		int uColumnOffset = (cropLeft / 2) * uPixelStride;
		for (int row = 0; row < chromaHeight; row++) {
			int base = uRowOffset + row * uRowStride + uColumnOffset;
			for (int column = 0; column < chromaWidth; column++) {
				yuvBuffer.put(uPlane.get(base + column * uPixelStride));
			}
		}

		int vRowOffset = (cropTop / 2) * vRowStride;
		int vColumnOffset = (cropLeft / 2) * vPixelStride;
		for (int row = 0; row < chromaHeight; row++) {
			int base = vRowOffset + row * vRowStride + vColumnOffset;
			for (int column = 0; column < chromaWidth; column++) {
				yuvBuffer.put(vPlane.get(base + column * vPixelStride));
			}
		}

		yuvBuffer.rewind();
		frameWidth = cropWidth;
		frameHeight = cropHeight;
		frameSerial++;
	}
}
