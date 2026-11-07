package org.godotengine.godot.mp4;

import android.graphics.Bitmap;
import android.graphics.Matrix;
import android.media.MediaDataSource;
import android.media.MediaMetadataRetriever;

import java.io.IOException;

/**
 * Extracts a video frame used as the editor thumbnail/icon for MP4 files.
 *
 * Uses MediaMetadataRetriever so thumbnails can be generated without setting
 * up the full playback pipeline. Returns null when the device cannot decode
 * the file; the editor then keeps the generic VideoStreamMP4 icon.
 */
public final class Mp4Thumbnail {
	private static final long THUMBNAIL_TIME_US = 1_000_000L;

	private Mp4Thumbnail() {
	}

	public static byte[] extractThumbnail(byte[] data, int maxWidth, int maxHeight, int[] outSize) {
		if (data == null || data.length == 0 || maxWidth <= 0 || maxHeight <= 0 || outSize == null || outSize.length < 2) {
			return null;
		}

		MediaMetadataRetriever retriever = new MediaMetadataRetriever();
		try {
			retriever.setDataSource(new ByteArraySource(data));

			Bitmap frame = retriever.getFrameAtTime(THUMBNAIL_TIME_US, MediaMetadataRetriever.OPTION_CLOSEST_SYNC);
			if (frame == null) {
				// Very short clips may not have a frame near the one second mark.
				frame = retriever.getFrameAtTime(0, MediaMetadataRetriever.OPTION_CLOSEST_SYNC);
			}
			if (frame == null) {
				return null;
			}

			frame = applyRotation(retriever, frame);
			frame = scaleToFit(frame, maxWidth, maxHeight);

			int width = frame.getWidth();
			int height = frame.getHeight();
			if (width <= 0 || height <= 0) {
				frame.recycle();
				return null;
			}

			int[] argb = new int[width * height];
			frame.getPixels(argb, 0, width, 0, 0, width, height);

			byte[] rgba = new byte[width * height * 4];
			for (int i = 0; i < argb.length; i++) {
				int pixel = argb[i];
				rgba[i * 4] = (byte)((pixel >> 16) & 0xFF);
				rgba[i * 4 + 1] = (byte)((pixel >> 8) & 0xFF);
				rgba[i * 4 + 2] = (byte)(pixel & 0xFF);
				rgba[i * 4 + 3] = (byte)((pixel >>> 24) & 0xFF);
			}

			outSize[0] = width;
			outSize[1] = height;
			frame.recycle();
			return rgba;
		} catch (Throwable t) {
			return null;
		} finally {
			try {
				retriever.release();
			} catch (IOException e) {
				// Ignore: a failed release must not replace the thumbnail result.
			}
		}
	}

	private static Bitmap applyRotation(MediaMetadataRetriever retriever, Bitmap frame) {
		String rotation = retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_ROTATION);
		if (rotation == null) {
			return frame;
		}

		int degrees;
		try {
			degrees = Integer.parseInt(rotation);
		} catch (NumberFormatException e) {
			return frame;
		}
		if (degrees == 0) {
			return frame;
		}

		Matrix matrix = new Matrix();
		matrix.postRotate(degrees);
		Bitmap rotated = Bitmap.createBitmap(frame, 0, 0, frame.getWidth(), frame.getHeight(), matrix, true);
		if (rotated != frame) {
			frame.recycle();
		}
		return rotated;
	}

	private static Bitmap scaleToFit(Bitmap frame, int maxWidth, int maxHeight) {
		int width = frame.getWidth();
		int height = frame.getHeight();
		if (width <= maxWidth && height <= maxHeight) {
			return frame;
		}

		float scale = Math.min((float)maxWidth / width, (float)maxHeight / height);
		int scaledWidth = Math.max(1, Math.round(width * scale));
		int scaledHeight = Math.max(1, Math.round(height * scale));
		Bitmap scaled = Bitmap.createScaledBitmap(frame, scaledWidth, scaledHeight, true);
		if (scaled != frame) {
			frame.recycle();
		}
		return scaled;
	}

	private static final class ByteArraySource extends MediaDataSource {
		private final byte[] data;

		ByteArraySource(byte[] data) {
			this.data = data;
		}

		@Override
		public int readAt(long position, byte[] buffer, int offset, int size) {
			if (position >= data.length) {
				return -1;
			}
			int length = (int)Math.min((long)size, (long)data.length - position);
			if (length <= 0) {
				return -1;
			}
			System.arraycopy(data, (int)position, buffer, offset, length);
			return length;
		}

		@Override
		public long getSize() {
			return data.length;
		}

		@Override
		public void close() {
		}
	}
}
