package com.codeaayu.airplaytv;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.provider.Settings;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.TextView;

/**
 * Full-screen receiver. Shows how to connect while idle; otherwise the mirrored screen,
 * a video sent by an app's AirPlay button, or a now-playing card for music.
 */
public class MainActivity extends Activity implements AirPlayServer.Listener, VideoLinkPlayer.Listener {
    private static final int CT_ALAC = 2;

    private enum Screen { IDLE, MIRROR, VIDEO_LINK, MUSIC }

    private SurfaceView videoView;
    private SurfaceView linkVideoView;
    private View idleView;
    private View nowPlayingView;
    private TextView statusView;
    private ImageView coverArtView;
    private TextView titleView;
    private TextView artistView;
    private TextView albumView;

    private final VideoDecoder video = new VideoDecoder();
    private final AudioPlayer audio = new AudioPlayer();
    private VideoLinkPlayer linkPlayer;
    private AirPlayServer server;
    private HandlerThread serverThread;
    private Handler serverHandler;
    private String receiverName;

    /* UI thread only */
    private boolean mirroring;
    private boolean videoLinkActive;
    private boolean musicActive;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        setContentView(R.layout.activity_main);

        videoView = findViewById(R.id.video);
        linkVideoView = findViewById(R.id.link_video);
        idleView = findViewById(R.id.idle);
        nowPlayingView = findViewById(R.id.now_playing);
        statusView = findViewById(R.id.status);
        coverArtView = findViewById(R.id.cover_art);
        titleView = findViewById(R.id.track_title);
        artistView = findViewById(R.id.track_artist);
        albumView = findViewById(R.id.track_album);

        String deviceName = Settings.Global.getString(getContentResolver(), Settings.Global.DEVICE_NAME);
        if (deviceName == null || deviceName.isEmpty()) {
            deviceName = getString(R.string.default_receiver_name);
        }
        // the TV's built-in cast services already use the plain device name
        receiverName = getString(R.string.receiver_name, deviceName);
        ((TextView) findViewById(R.id.receiver_name)).setText(receiverName);
        ((TextView) findViewById(R.id.hint)).setText(getString(R.string.hint, receiverName));

        linkPlayer = new VideoLinkPlayer(this, this);
        videoView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                video.setSurface(holder.getSurface());
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                video.setSurface(null);
            }
        });
        linkVideoView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                linkPlayer.setSurface(holder.getSurface());
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                linkPlayer.setSurface(null);
            }
        });

        server = new AirPlayServer(this, this);
        serverThread = new HandlerThread("airplay-control");
        serverThread.start();
        serverHandler = new Handler(serverThread.getLooper());
    }

    @Override
    protected void onStart() {
        super.onStart();
        statusView.setText(R.string.status_starting);
        serverHandler.post(() -> {
            VideoQuality quality = VideoQuality.best();
            boolean started = server.start(receiverName, quality);
            runOnUiThread(() -> statusView.setText(started
                    ? getString(R.string.status_ready, quality.toString())
                    : getString(R.string.status_failed)));
        });
    }

    @Override
    protected void onStop() {
        super.onStop();
        serverHandler.post(() -> {
            server.stop();
            video.release();
            audio.release();
        });
        linkPlayer.stop();
        mirroring = false;
        videoLinkActive = false;
        musicActive = false;
        updateScreen();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        serverThread.quitSafely();
    }

    /** Mirroring wins over an app's video, which wins over music. */
    private void updateScreen() {
        Screen screen = mirroring ? Screen.MIRROR
                : videoLinkActive ? Screen.VIDEO_LINK
                : musicActive ? Screen.MUSIC
                : Screen.IDLE;
        // only one SurfaceView is shown at a time so they never fight over z-order
        videoView.setVisibility(screen == Screen.VIDEO_LINK ? View.GONE : View.VISIBLE);
        linkVideoView.setVisibility(screen == Screen.VIDEO_LINK ? View.VISIBLE : View.GONE);
        nowPlayingView.setVisibility(screen == Screen.MUSIC ? View.VISIBLE : View.GONE);
        idleView.setVisibility(screen == Screen.IDLE ? View.VISIBLE : View.GONE);
    }

    /** Sizes a video view to the stream's aspect ratio, letterboxed in the screen. */
    private void fitVideo(SurfaceView view, int width, int height) {
        View root = (View) view.getParent();
        int screenW = root.getWidth();
        int screenH = root.getHeight();
        if (screenW == 0 || screenH == 0 || width == 0 || height == 0) {
            return;
        }
        int viewW = screenW;
        int viewH = (int) ((long) screenW * height / width);
        if (viewH > screenH) {
            viewH = screenH;
            viewW = (int) ((long) screenH * width / height);
        }
        FrameLayout.LayoutParams params = (FrameLayout.LayoutParams) view.getLayoutParams();
        params.width = viewW;
        params.height = viewH;
        view.setLayoutParams(params);
    }

    // ---- AirPlayServer.Listener, called on the server's threads ----

    @Override
    public void onVideoFormat(boolean h265, int width, int height) {
        video.setFormat(h265, width, height);
        runOnUiThread(() -> {
            fitVideo(videoView, width, height);
            mirroring = true;
            updateScreen();
        });
    }

    @Override
    public void onVideoData(byte[] data) {
        video.decode(data);
    }

    @Override
    public void onAudioFormat(int compressionType, int samplesPerFrame) {
        audio.start(compressionType, samplesPerFrame);
        boolean music = compressionType == CT_ALAC;
        runOnUiThread(() -> {
            if (music && !musicActive) {
                titleView.setText(R.string.now_playing);
                artistView.setText(null);
                albumView.setText(null);
                coverArtView.setImageDrawable(null);
            }
            musicActive = music;
            updateScreen();
        });
    }

    @Override
    public void onAudioData(byte[] data) {
        audio.decode(data);
    }

    @Override
    public void onAudioPcm(byte[] pcm) {
        audio.playPcm(pcm);
    }

    @Override
    public void onAudioVolume(float decibels) {
        audio.setVolume(decibels);
    }

    @Override
    public void onAudioFlush() {
        audio.flush();
    }

    @Override
    public void onAudioStopped() {
        runOnUiThread(() -> {
            musicActive = false;
            updateScreen();
        });
    }

    @Override
    public void onTrackInfo(String title, String artist, String album) {
        runOnUiThread(() -> {
            titleView.setText(title);
            artistView.setText(artist);
            albumView.setText(album);
        });
    }

    @Override
    public void onCoverArt(byte[] jpeg) {
        // album art can be large; decode at most ~720 px to spare the TV's memory
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeByteArray(jpeg, 0, jpeg.length, bounds);
        BitmapFactory.Options options = new BitmapFactory.Options();
        options.inSampleSize = 1;
        while (bounds.outWidth / (options.inSampleSize * 2) >= 720) {
            options.inSampleSize *= 2;
        }
        Bitmap art = BitmapFactory.decodeByteArray(jpeg, 0, jpeg.length, options);
        runOnUiThread(() -> coverArtView.setImageBitmap(art));
    }

    @Override
    public void onMirrorRunning(boolean running) {
        if (!running) {
            video.release();
        }
        runOnUiThread(() -> {
            mirroring = running;
            updateScreen();
        });
    }

    @Override
    public void onVideoPlay(String url, float startSeconds) {
        linkPlayer.play(url, startSeconds);
    }

    @Override
    public void onVideoSeek(float seconds) {
        linkPlayer.seek(seconds);
    }

    @Override
    public void onVideoRate(float rate) {
        linkPlayer.setRate(rate);
    }

    @Override
    public void onVideoStop() {
        linkPlayer.stop();
    }

    @Override
    public float onVideoPause() {
        return linkPlayer.pause();
    }

    @Override
    public double[] getVideoPlaybackInfo() {
        return linkPlayer.playbackInfo();
    }

    // ---- VideoLinkPlayer.Listener, called on the UI thread ----

    @Override
    public void onVideoSize(int width, int height) {
        fitVideo(linkVideoView, width, height);
    }

    @Override
    public void onActive(boolean active) {
        videoLinkActive = active;
        updateScreen();
    }
}
