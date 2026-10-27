package com.codeaayu.airplaytv;

import android.content.Context;
import android.content.SharedPreferences;
import android.net.wifi.WifiManager;
import android.util.Log;

import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
import java.util.Locale;

/**
 * Runs UxPlay's AirPlay server (native), which also announces itself on the network with
 * UxPlay's own mDNS responder, so Macs, iPhones and iPads list this TV under Screen Mirroring.
 */
public class AirPlayServer {
    private static final String TAG = "AirPlayTV";

    static {
        System.loadLibrary("airplaytv");
    }

    public interface Listener {
        void onVideoFormat(boolean h265, int width, int height);
        void onVideoData(byte[] data);
        void onAudioFormat(int compressionType, int samplesPerFrame);
        void onAudioData(byte[] data);
        void onAudioVolume(float decibels);
        void onAudioFlush();
        void onMirrorRunning(boolean running);

        /** 16-bit stereo PCM, already decoded (ALAC music streams). */
        void onAudioPcm(byte[] pcm);
        void onTrackInfo(String title, String artist, String album);
        void onCoverArt(byte[] jpeg);
        /** The music stream ended. */
        void onAudioStopped();

        /** An app's AirPlay button sent a video URL to play from {@code startSeconds}. */
        void onVideoPlay(String url, float startSeconds);
        void onVideoSeek(float seconds);
        void onVideoRate(float rate);
        void onVideoStop();
        /** Pause and return the current position in seconds. */
        float onVideoPause();
        /** { duration, position, rate } in seconds; duration -1 when finished, position -1 while loading. */
        double[] getVideoPlaybackInfo();
    }

    private final Context context;
    private final Listener listener;
    private final WifiManager.MulticastLock multicastLock;
    private boolean running;

    public AirPlayServer(Context context, Listener listener) {
        this.context = context.getApplicationContext();
        this.listener = listener;
        WifiManager wifi = (WifiManager) this.context.getSystemService(Context.WIFI_SERVICE);
        // without this lock Android's Wi-Fi drops the mDNS queries Apple devices send
        multicastLock = wifi.createMulticastLock("airplay-mdns");
        multicastLock.setReferenceCounted(false);
    }

    /** Starts the server and announces it as {@code name}; returns false if the native server failed. */
    public synchronized boolean start(String name, VideoQuality quality) {
        if (running) {
            return true;
        }
        multicastLock.acquire();
        String keyfile = context.getFilesDir().getAbsolutePath() + "/airplay-key.pem";
        String language = Locale.getDefault().getLanguage();
        if (!nativeStart(name, deviceAddress(), keyfile, quality.width, quality.height, quality.fps, quality.h265,
                language)) {
            multicastLock.release();
            return false;
        }
        running = true;
        Log.i(TAG, "announced \"" + name + "\" on port " + nativeGetPort() + ", offering " + quality);
        return true;
    }

    public synchronized void stop() {
        if (!running) {
            return;
        }
        nativeStop();
        multicastLock.release();
        running = false;
    }

    /** A random, locally administered MAC address, kept so Apple devices remember this receiver. */
    private byte[] deviceAddress() {
        SharedPreferences prefs = context.getSharedPreferences("airplay", Context.MODE_PRIVATE);
        String saved = prefs.getString("hwaddr", null);
        if (saved != null) {
            byte[] bytes = new byte[6];
            for (int i = 0; i < 6; i++) {
                bytes[i] = (byte) Integer.parseInt(saved.substring(2 * i, 2 * i + 2), 16);
            }
            return bytes;
        }
        byte[] bytes = new byte[6];
        new SecureRandom().nextBytes(bytes);
        bytes[0] = (byte) ((bytes[0] & 0xfe) | 0x02);
        prefs.edit().putString("hwaddr", hex(bytes)).apply();
        return bytes;
    }

    private static String hex(byte[] bytes) {
        StringBuilder sb = new StringBuilder();
        for (byte b : bytes) {
            sb.append(String.format("%02X", b & 0xff));
        }
        return sb.toString();
    }

    // ---- called from native code, on the server's threads ----

    void onVideoFormat(boolean h265, int width, int height) {
        listener.onVideoFormat(h265, width, height);
    }

    void onVideoData(byte[] data, long ntpTime) {
        listener.onVideoData(data);
    }

    void onAudioFormat(int compressionType, int samplesPerFrame) {
        listener.onAudioFormat(compressionType, samplesPerFrame);
    }

    void onAudioData(byte[] data) {
        listener.onAudioData(data);
    }

    void onAudioVolume(float decibels) {
        listener.onAudioVolume(decibels);
    }

    void onAudioFlush() {
        listener.onAudioFlush();
    }

    void onConnection(boolean connected) {
        Log.i(TAG, connected ? "client connected" : "client disconnected");
    }

    void onMirrorRunning(boolean running) {
        listener.onMirrorRunning(running);
    }

    void onAudioPcm(byte[] pcm) {
        listener.onAudioPcm(pcm);
    }

    /** Track info arrives as a DMAP "mlit" listing: 4-byte tag, 4-byte length, value, repeated. */
    void onAudioMetadata(byte[] dmap) {
        if (dmap.length < 8 || !"mlit".equals(new String(dmap, 0, 4, StandardCharsets.US_ASCII))) {
            return;
        }
        String title = null;
        String artist = null;
        String album = null;
        for (int i = 8; i + 8 <= dmap.length; ) {
            String tag = new String(dmap, i, 4, StandardCharsets.US_ASCII);
            int len = ((dmap[i + 4] & 0xff) << 24) | ((dmap[i + 5] & 0xff) << 16)
                    | ((dmap[i + 6] & 0xff) << 8) | (dmap[i + 7] & 0xff);
            if (len < 0 || i + 8 + len > dmap.length) {
                break;
            }
            String value = new String(dmap, i + 8, len, StandardCharsets.UTF_8);
            switch (tag) {
                case "minm":
                    title = value;
                    break;
                case "asar":
                    artist = value;
                    break;
                case "asal":
                    album = value;
                    break;
                default:
                    break;
            }
            i += 8 + len;
        }
        listener.onTrackInfo(title, artist, album);
    }

    void onAudioCoverArt(byte[] jpeg) {
        listener.onCoverArt(jpeg);
    }

    void onAudioCoverArtStop() {
        listener.onAudioStopped();
    }

    void onVideoPlay(String url, float startSeconds) {
        listener.onVideoPlay(url, startSeconds);
    }

    void onVideoScrub(float seconds) {
        listener.onVideoSeek(seconds);
    }

    void onVideoRate(float rate) {
        listener.onVideoRate(rate);
    }

    void onVideoStop() {
        listener.onVideoStop();
    }

    float onVideoPlaylistRemove() {
        return listener.onVideoPause();
    }

    double[] getPlaybackInfo() {
        return listener.getVideoPlaybackInfo();
    }

    private native boolean nativeStart(String name, byte[] hwaddr, String keyfile, int width, int height, int fps,
                                       boolean h265, String language);
    private native void nativeStop();
    private native int nativeGetPort();
}
