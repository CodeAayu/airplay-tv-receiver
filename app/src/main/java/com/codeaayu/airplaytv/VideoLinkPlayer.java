package com.codeaayu.airplaytv;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.MediaPlayer;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Surface;

/**
 * Plays a video URL sent by an app's AirPlay button (an HLS stream relayed by the server
 * from localhost, or a direct link such as Safari sends), and reports progress back to
 * the sending device. All MediaPlayer calls run on the main thread.
 */
public class VideoLinkPlayer {
    private static final String TAG = "AirPlayTV";

    public interface Listener {
        void onVideoSize(int width, int height);
        /** Playback started (true) or ended (false). */
        void onActive(boolean active);
    }

    private final Context context;
    private final Listener listener;
    private final Handler main = new Handler(Looper.getMainLooper());

    private Surface surface;
    private MediaPlayer player;
    private String pendingUrl;
    private float pendingStart;

    /* read from the server's threads when the sender polls for progress */
    private volatile boolean prepared;
    private volatile boolean finished;
    private volatile boolean paused;

    public VideoLinkPlayer(Context context, Listener listener) {
        this.context = context.getApplicationContext();
        this.listener = listener;
    }

    public void setSurface(Surface surface) {
        main.post(() -> {
            this.surface = surface;
            if (player != null) {
                player.setSurface(surface);
            } else if (surface != null && pendingUrl != null) {
                open(pendingUrl, pendingStart);
            }
        });
    }

    public void play(String url, float startSeconds) {
        main.post(() -> {
            release();
            if (surface == null) {
                // the video view is still being created; open once its surface exists
                pendingUrl = url;
                pendingStart = startSeconds;
                listener.onActive(true);
                return;
            }
            open(url, startSeconds);
        });
    }

    public void seek(float seconds) {
        main.post(() -> {
            if (player != null && prepared) {
                player.seekTo((long) (seconds * 1000), MediaPlayer.SEEK_CLOSEST);
            }
        });
    }

    /** AirPlay rate: 1 plays, 0 pauses. */
    public void setRate(float rate) {
        main.post(() -> {
            if (player == null || !prepared) {
                return;
            }
            if (rate == 0f) {
                player.pause();
                paused = true;
            } else {
                player.start();
                paused = false;
            }
        });
    }

    public void stop() {
        main.post(() -> {
            boolean wasActive = player != null || pendingUrl != null;
            release();
            if (wasActive) {
                listener.onActive(false);
            }
        });
    }

    /** Pauses and returns the position in seconds (called from a server thread). */
    public float pause() {
        float position = (float) positionSeconds();
        setRate(0f);
        return position;
    }

    /** { duration, position, rate }: duration -1 once finished, position -1 while loading. */
    public double[] playbackInfo() {
        MediaPlayer current = player;
        if (finished) {
            return new double[] { -1, -1, 0 };
        }
        if (current == null || !prepared) {
            return new double[] { 0, -1, 0 };
        }
        double duration;
        try {
            duration = current.getDuration() / 1000.0;
        } catch (IllegalStateException e) {
            return new double[] { 0, -1, 0 };
        }
        return new double[] { Math.max(duration, 0), positionSeconds(), paused ? 0 : 1 };
    }

    private double positionSeconds() {
        MediaPlayer current = player;
        if (current == null || !prepared) {
            return 0;
        }
        try {
            return current.getCurrentPosition() / 1000.0;
        } catch (IllegalStateException e) {
            return 0;
        }
    }

    private void open(String url, float startSeconds) {
        pendingUrl = null;
        finished = false;
        prepared = false;
        paused = false;
        Log.i(TAG, "opening video link " + url + " at " + startSeconds + " s");

        MediaPlayer created = new MediaPlayer();
        player = created;
        created.setAudioAttributes(new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
                .build());
        created.setSurface(surface);
        created.setScreenOnWhilePlaying(true);
        created.setOnVideoSizeChangedListener((mp, width, height) -> listener.onVideoSize(width, height));
        created.setOnPreparedListener(mp -> {
            prepared = true;
            if (startSeconds > 0) {
                mp.seekTo((long) (startSeconds * 1000), MediaPlayer.SEEK_CLOSEST);
            }
            mp.start();
        });
        created.setOnCompletionListener(mp -> finish("finished"));
        created.setOnErrorListener((mp, what, extra) -> {
            finish("error " + what + "/" + extra);
            return true;
        });
        listener.onActive(true);
        try {
            created.setDataSource(context, Uri.parse(url));
            created.prepareAsync();
        } catch (Exception e) {
            Log.e(TAG, "cannot open video link", e);
            finish("cannot open");
        }
    }

    /** The video ended; the sender learns this on its next progress poll and closes the session. */
    private void finish(String reason) {
        Log.i(TAG, "video link " + reason);
        finished = true;
        if (player != null) {
            player.release();
            player = null;
        }
        prepared = false;
        listener.onActive(false);
    }

    private void release() {
        pendingUrl = null;
        if (player != null) {
            player.release();
            player = null;
        }
        prepared = false;
        paused = false;
        finished = false;
    }
}
