package com.codeaayu.airplaytv;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.util.Log;

import java.nio.ByteBuffer;

/**
 * Plays AirPlay audio: AAC-ELD while mirroring and AAC-LC for some media are decoded here;
 * ALAC, used for music (audio-only AirPlay), is decoded natively and arrives as PCM.
 */
public class AudioPlayer {
    private static final String TAG = "AirPlayTV";
    private static final int SAMPLE_RATE = 44100;
    private static final int CHANNELS = 2;

    /* AirPlay "ct" values */
    private static final int CT_ALAC = 2;
    private static final int CT_AAC_LC = 4;
    private static final int CT_AAC_ELD = 8;

    /* AudioSpecificConfig for 44.1 kHz stereo */
    private static final byte[] ASC_AAC_ELD = { (byte) 0xF8, (byte) 0xE8, 0x50, 0x00 };
    private static final byte[] ASC_AAC_LC = { 0x12, 0x10 };

    private MediaCodec codec;
    private AudioTrack track;
    private Thread outputThread;
    private volatile boolean playing;
    private float volume = 1f;

    public synchronized void start(int compressionType, int samplesPerFrame) {
        release();
        if (compressionType == CT_ALAC) {
            // music: no lip-sync to keep, so a deeper buffer rides out Wi-Fi hiccups
            track = createTrack(4);
            track.play();
            Log.i(TAG, "playing ALAC music as PCM");
            return;
        }
        byte[] config;
        if (compressionType == CT_AAC_ELD) {
            config = ASC_AAC_ELD;
        } else if (compressionType == CT_AAC_LC) {
            config = ASC_AAC_LC;
        } else {
            Log.w(TAG, "unsupported audio format ct=" + compressionType);
            return;
        }

        MediaFormat format = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_AAC, SAMPLE_RATE, CHANNELS);
        format.setByteBuffer("csd-0", ByteBuffer.wrap(config));
        try {
            codec = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_AUDIO_AAC);
            codec.configure(format, null, null, 0);
            codec.start();
        } catch (Exception e) {
            Log.e(TAG, "could not start AAC decoder", e);
            if (codec != null) {
                codec.release();
                codec = null;
            }
            return;
        }

        track = createTrack(2);
        track.play();

        playing = true;
        final MediaCodec runningCodec = codec;
        final AudioTrack runningTrack = track;
        outputThread = new Thread(() -> drain(runningCodec, runningTrack), "audio-output");
        outputThread.start();
        Log.i(TAG, "playing audio ct=" + compressionType + " with " + codec.getName());
    }

    /** Plays already-decoded 16-bit stereo PCM (ALAC music). */
    public void playPcm(byte[] pcm) {
        AudioTrack current;
        synchronized (this) {
            current = track;
        }
        if (current != null && codec == null) {
            current.write(pcm, 0, pcm.length);
        }
    }

    private AudioTrack createTrack(int bufferMultiple) {
        int minBuffer = AudioTrack.getMinBufferSize(SAMPLE_RATE, AudioFormat.CHANNEL_OUT_STEREO,
                AudioFormat.ENCODING_PCM_16BIT);
        AudioTrack created = new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
                        .build())
                .setAudioFormat(new AudioFormat.Builder()
                        .setSampleRate(SAMPLE_RATE)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .build())
                .setBufferSizeInBytes(minBuffer * bufferMultiple)
                .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
                .build();
        created.setVolume(volume);
        return created;
    }

    public synchronized void decode(byte[] data) {
        if (codec == null) {
            return;
        }
        try {
            int index = codec.dequeueInputBuffer(10_000);
            if (index < 0) {
                return;
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
            Log.w(TAG, "audio decoder error", e);
        }
    }

    /** AirPlay volume in dB: 0 is full, -30 is quietest, -144 is mute. */
    public synchronized void setVolume(float decibels) {
        volume = decibels <= -30f ? 0f : (float) Math.pow(10, decibels / 20f);
        if (track != null) {
            track.setVolume(volume);
        }
    }

    /** Drops queued sound. The decoder is left alone: flushing it would race the output thread. */
    public synchronized void flush() {
        if (track != null) {
            track.pause();
            track.flush();
            track.play();
        }
    }

    public synchronized void release() {
        playing = false;
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
        if (track != null) {
            track.release();
            track = null;
        }
    }

    private void drain(MediaCodec runningCodec, AudioTrack runningTrack) {
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        while (playing) {
            try {
                int index = runningCodec.dequeueOutputBuffer(info, 10_000);
                if (index < 0) {
                    continue;
                }
                ByteBuffer pcm = runningCodec.getOutputBuffer(index);
                if (pcm != null && info.size > 0) {
                    pcm.position(info.offset);
                    pcm.limit(info.offset + info.size);
                    runningTrack.write(pcm, info.size, AudioTrack.WRITE_BLOCKING);
                }
                runningCodec.releaseOutputBuffer(index, false);
            } catch (IllegalStateException e) {
                return;
            }
        }
    }
}
