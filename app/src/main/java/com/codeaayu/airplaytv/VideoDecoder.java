package com.codeaayu.airplaytv;

import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Build;
import android.util.Log;
import android.view.Surface;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;

/**
 * Decodes the mirrored H.264/H.265 stream (Annex-B, parameter sets in-band before each
 * keyframe) onto a Surface, showing every frame as soon as it is decoded.
 */
public class VideoDecoder {
    private static final String TAG = "AirPlayTV";
    /* large enough for a 4K keyframe */
    private static final int MAX_INPUT_SIZE = 4 * 1024 * 1024;

    private Surface surface;
    private boolean h265;
    private int width = 1920;
    private int height = 1080;

    private MediaCodec codec;
    private Thread outputThread;
    private volatile boolean decoding;

    public synchronized void setSurface(Surface surface) {
        if (this.surface != surface) {
            release();
            this.surface = surface;
        }
    }

    /** A new stream format; the decoder is rebuilt from the next keyframe's parameter sets. */
    public synchronized void setFormat(boolean h265, int width, int height) {
        if (codec != null && (this.h265 != h265 || this.width != width || this.height != height)) {
            release();
        }
        this.h265 = h265;
        this.width = width;
        this.height = height;
    }

    public synchronized void decode(byte[] data) {
        if (codec == null && !start(data)) {
            return; // wait for a keyframe with parameter sets
        }
        try {
            int index = codec.dequeueInputBuffer(20_000);
            if (index < 0) {
                return; // decoder is behind; drop this frame
            }
            ByteBuffer buffer = codec.getInputBuffer(index);
            if (buffer == null || data.length > buffer.capacity()) {
                codec.queueInputBuffer(index, 0, 0, 0, 0);
                return;
            }
            buffer.clear();
            buffer.put(data);
            codec.queueInputBuffer(index, 0, data.length, System.nanoTime() / 1000, 0);
        } catch (IllegalStateException e) {
            Log.w(TAG, "video decoder error, restarting", e);
            release();
        }
    }

    public synchronized void release() {
        decoding = false;
        if (outputThread != null) {
            try {
                outputThread.join(500);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
            outputThread = null;
        }
        if (codec != null) {
            try {
                codec.stop();
            } catch (IllegalStateException ignored) {
            }
            codec.release();
            codec = null;
        }
    }

    private boolean start(byte[] data) {
        if (surface == null || !surface.isValid()) {
            return false;
        }
        byte[] parameterSets = parameterSets(data);
        if (parameterSets == null) {
            return false;
        }
        String mime = h265 ? MediaFormat.MIMETYPE_VIDEO_HEVC : MediaFormat.MIMETYPE_VIDEO_AVC;
        MediaFormat format = MediaFormat.createVideoFormat(mime, width, height);
        format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, MAX_INPUT_SIZE);
        format.setInteger(MediaFormat.KEY_PRIORITY, 0);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1);
        }
        if (h265) {
            format.setByteBuffer("csd-0", ByteBuffer.wrap(parameterSets));
        } else {
            splitAvcParameterSets(parameterSets, format);
        }

        try {
            codec = MediaCodec.createDecoderByType(mime);
            codec.configure(format, surface, null, 0);
            codec.start();
        } catch (Exception e) {
            Log.e(TAG, "could not start " + mime + " decoder", e);
            if (codec != null) {
                codec.release();
                codec = null;
            }
            return false;
        }
        Log.i(TAG, "decoding " + mime + " " + width + "x" + height + " with " + codec.getName());

        decoding = true;
        final MediaCodec running = codec;
        outputThread = new Thread(() -> drain(running), "video-output");
        outputThread.start();
        return true;
    }

    private void drain(MediaCodec running) {
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        while (decoding) {
            try {
                int index = running.dequeueOutputBuffer(info, 20_000);
                if (index >= 0) {
                    running.releaseOutputBuffer(index, true);
                }
            } catch (IllegalStateException e) {
                return;
            }
        }
    }

    /** Returns the VPS/SPS/PPS NAL units (with start codes) at the front of a keyframe, or null. */
    private byte[] parameterSets(byte[] data) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        int start = nextStartCode(data, 0);
        while (start >= 0) {
            int nalStart = start + 4;
            if (nalStart >= data.length) {
                break;
            }
            int type = h265 ? (data[nalStart] >> 1) & 0x3f : data[nalStart] & 0x1f;
            boolean isParameterSet = h265 ? (type >= 32 && type <= 34) : (type == 7 || type == 8);
            if (!isParameterSet) {
                break;
            }
            int next = nextStartCode(data, nalStart);
            int end = next < 0 ? data.length : next;
            out.write(data, start, end - start);
            start = next;
        }
        return out.size() > 0 ? out.toByteArray() : null;
    }

    private static void splitAvcParameterSets(byte[] sets, MediaFormat format) {
        int pps = -1;
        for (int i = nextStartCode(sets, 0); i >= 0; i = nextStartCode(sets, i + 4)) {
            if ((sets[i + 4] & 0x1f) == 8) {
                pps = i;
                break;
            }
        }
        if (pps < 0) {
            format.setByteBuffer("csd-0", ByteBuffer.wrap(sets));
            return;
        }
        byte[] sps = new byte[pps];
        byte[] ppsBytes = new byte[sets.length - pps];
        System.arraycopy(sets, 0, sps, 0, pps);
        System.arraycopy(sets, pps, ppsBytes, 0, ppsBytes.length);
        format.setByteBuffer("csd-0", ByteBuffer.wrap(sps));
        format.setByteBuffer("csd-1", ByteBuffer.wrap(ppsBytes));
    }

    /** Index of the next 4-byte start code (00 00 00 01) at or after {@code from}, or -1. */
    private static int nextStartCode(byte[] data, int from) {
        for (int i = from; i + 3 < data.length; i++) {
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
                return i;
            }
        }
        return -1;
    }
}
