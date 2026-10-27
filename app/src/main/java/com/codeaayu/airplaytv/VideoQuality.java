package com.codeaayu.airplaytv;

import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Build;
import android.util.Log;

/** The mirroring format offered to Apple devices: the best one this TV decodes in hardware. */
public final class VideoQuality {
    private static final String TAG = "AirPlayTV";

    /* best first; 4K needs h265, which Apple only sends when the receiver asks for 4K */
    private static final VideoQuality[] CANDIDATES = {
            new VideoQuality(3840, 2160, 60, true),
            new VideoQuality(3840, 2160, 30, true),
            new VideoQuality(1920, 1080, 60, false),
            new VideoQuality(1920, 1080, 30, false),
    };

    public final int width;
    public final int height;
    public final int fps;
    public final boolean h265;

    private VideoQuality(int width, int height, int fps, boolean h265) {
        this.width = width;
        this.height = height;
        this.fps = fps;
        this.h265 = h265;
    }

    public static VideoQuality best() {
        for (VideoQuality quality : CANDIDATES) {
            String decoder = hardwareDecoder(quality);
            if (decoder != null) {
                Log.i(TAG, quality + " supported by " + decoder);
                return quality;
            }
        }
        return CANDIDATES[CANDIDATES.length - 1];
    }

    /** Name of a hardware decoder that handles this size and frame rate, or null. */
    private static String hardwareDecoder(VideoQuality quality) {
        String mime = quality.h265 ? MediaFormat.MIMETYPE_VIDEO_HEVC : MediaFormat.MIMETYPE_VIDEO_AVC;
        for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
            if (info.isEncoder() || !isHardware(info)) {
                continue;
            }
            for (String type : info.getSupportedTypes()) {
                if (!type.equalsIgnoreCase(mime)) {
                    continue;
                }
                MediaCodecInfo.VideoCapabilities caps = info.getCapabilitiesForType(type).getVideoCapabilities();
                if (caps != null && caps.areSizeAndRateSupported(quality.width, quality.height, quality.fps)) {
                    return info.getName();
                }
            }
        }
        return null;
    }

    private static boolean isHardware(MediaCodecInfo info) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            return info.isHardwareAccelerated();
        }
        String name = info.getName().toLowerCase();
        return !name.startsWith("omx.google.") && !name.startsWith("c2.android.");
    }

    @Override
    public String toString() {
        return (height == 2160 ? "4K" : height + "p") + " " + fps + " fps " + (h265 ? "H.265" : "H.264");
    }
}
