/*
 * JNI bridge between UxPlay's AirPlay server (lib/raop) and the Java app.
 *
 * The server runs on UxPlay's own threads; every callback attaches that thread to the JVM
 * once and forwards the event to com.codeaayu.airplaytv.AirPlayServer.
 *
 * GPL-3.0 (links UxPlay).
 */

#include <jni.h>
#include <android/log.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "raop.h"
#include "dnssd.h"
#include "logger.h"
#include "stream.h"
#include "alac_bridge.h"

#define TAG "AirPlayTV"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static JavaVM *g_vm;
static jobject g_server;          /* global ref to the AirPlayServer instance */
static pthread_key_t g_env_key;

static jmethodID m_on_video_format;
static jmethodID m_on_video_data;
static jmethodID m_on_audio_format;
static jmethodID m_on_audio_data;
static jmethodID m_on_audio_volume;
static jmethodID m_on_audio_flush;
static jmethodID m_on_connection;
static jmethodID m_on_mirror_running;
static jmethodID m_on_audio_pcm;
static jmethodID m_on_audio_metadata;
static jmethodID m_on_audio_coverart;
static jmethodID m_on_audio_coverart_stop;
static jmethodID m_on_video_play;
static jmethodID m_on_video_scrub;
static jmethodID m_on_video_rate;
static jmethodID m_on_video_stop;
static jmethodID m_on_video_playlist_remove;
static jmethodID m_get_playback_info;

static raop_t *g_raop;
static dnssd_t *g_dnssd;
static unsigned short g_port;
static bool g_ntp_initialized;

/* ALAC decoder for audio-only AirPlay (ct=2); Android has none built in */
#define CT_ALAC 2
#define ALAC_FRAME_LENGTH 352
#define ALAC_SAMPLE_RATE 44100
static pthread_mutex_t g_alac_lock = PTHREAD_MUTEX_INITIALIZER;
static void *g_alac;

/* raop_set_lang() keeps these pointers, so they must outlive the server */
static char g_lang[16];

static void detach_thread(void *env)
{
    if (env && g_vm) {
        (*g_vm)->DetachCurrentThread(g_vm);
    }
}

/* Returns a JNIEnv for the calling thread, attaching UxPlay's threads on first use. */
static JNIEnv *get_env(void)
{
    JNIEnv *env = NULL;
    if ((*g_vm)->GetEnv(g_vm, (void **) &env, JNI_VERSION_1_6) == JNI_OK) {
        return env;
    }
    if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) {
        return NULL;
    }
    pthread_setspecific(g_env_key, env);
    return env;
}

static void clear_exception(JNIEnv *env)
{
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }
}

static void call_void(jmethodID method, ...)
{
    JNIEnv *env = get_env();
    if (!env || !g_server) return;
    va_list args;
    va_start(args, method);
    (*env)->CallVoidMethodV(env, g_server, method, args);
    va_end(args);
    clear_exception(env);
}

static void send_bytes(jmethodID method, const unsigned char *data, int len, jlong extra, bool with_extra)
{
    JNIEnv *env = get_env();
    if (!env || !g_server || len <= 0) return;
    jbyteArray array = (*env)->NewByteArray(env, len);
    if (!array) {
        clear_exception(env);
        return;
    }
    (*env)->SetByteArrayRegion(env, array, 0, len, (const jbyte *) data);
    if (with_extra) {
        (*env)->CallVoidMethod(env, g_server, method, array, extra);
    } else {
        (*env)->CallVoidMethod(env, g_server, method, array);
    }
    clear_exception(env);
    (*env)->DeleteLocalRef(env, array);
}

/* ---- UxPlay callbacks ---- */

static void cb_conn_init(void *cls)
{
    call_void(m_on_connection, JNI_TRUE);
}

static void cb_conn_destroy(void *cls)
{
    call_void(m_on_connection, JNI_FALSE);
}

static void cb_conn_reset(void *cls, int reason)
{
    LOGI("connection reset, reason %d", reason);
    call_void(m_on_connection, JNI_FALSE);
}

static void cb_conn_feedback(void *cls) {}

static void cb_video_reset(void *cls, reset_type_t reset_type)
{
    LOGI("video reset, type %d", (int) reset_type);
    switch (reset_type) {
    case RESET_TYPE_NOHOLD:
        /* a new client took over: drop any video it had sent us */
        raop_destroy_airplay_video(g_raop, -1);
        call_void(m_on_video_stop);
        break;
    case RESET_TYPE_HLS_EOS:
        call_void(m_on_video_stop);
        break;
    case RESET_TYPE_HLS_SHUTDOWN:
        call_void(m_on_video_stop);
        raop_destroy_airplay_video(g_raop, -1);
        raop_remove_hls_connections(g_raop);
        break;
    default:
        /* mirroring stops are reported through mirror_video_running;
           RESET_TYPE_ON_VIDEO_PLAY is handled by on_video_play */
        break;
    }
}

static int cb_video_set_codec_and_size(void *cls, video_codec_t codec, unsigned short width_source,
                                       unsigned short height_source, unsigned short x_offset,
                                       unsigned short y_offset, unsigned short width, unsigned short height)
{
    LOGI("video format: %s %ux%u (source %ux%u)", codec == VIDEO_CODEC_H265 ? "h265" : "h264",
         width, height, width_source, height_source);
    call_void(m_on_video_format, (jboolean) (codec == VIDEO_CODEC_H265), (jint) width, (jint) height);
    return 0;
}

static void cb_video_process(void *cls, raop_ntp_t *ntp, video_decode_struct *data)
{
    send_bytes(m_on_video_data, data->data, data->data_len, (jlong) data->ntp_time_remote, true);
}

static void cb_video_flush(void *cls) {}
static void cb_video_pause(void *cls) {}
static void cb_video_resume(void *cls) {}

static void cb_mirror_video_running(void *cls, bool is_running)
{
    call_void(m_on_mirror_running, (jboolean) is_running);
}

static void cb_audio_get_format(void *cls, unsigned char *ct, unsigned short *spf, bool *usingScreen,
                                bool *isMedia, uint64_t *audioFormat)
{
    LOGI("audio format: ct=%d spf=%d screen=%d media=%d", *ct, *spf, *usingScreen, *isMedia);
    pthread_mutex_lock(&g_alac_lock);
    if (g_alac) {
        alac_decoder_destroy(g_alac);
        g_alac = NULL;
    }
    if (*ct == CT_ALAC) {
        g_alac = alac_decoder_create(*spf ? *spf : ALAC_FRAME_LENGTH, ALAC_SAMPLE_RATE);
        if (!g_alac) {
            LOGE("could not create ALAC decoder");
        }
    }
    pthread_mutex_unlock(&g_alac_lock);
    call_void(m_on_audio_format, (jint) *ct, (jint) *spf);
}

static void cb_audio_process(void *cls, raop_ntp_t *ntp, audio_decode_struct *data)
{
    if (data->ct != CT_ALAC) {
        send_bytes(m_on_audio_data, data->data, data->data_len, 0, false);
        return;
    }
    /* ALAC is decoded here; Java gets PCM */
    unsigned char pcm[4096 * 4];
    int pcm_len = 0;
    pthread_mutex_lock(&g_alac_lock);
    if (g_alac) {
        pcm_len = alac_decoder_decode(g_alac, data->data, data->data_len, pcm, sizeof(pcm));
    }
    pthread_mutex_unlock(&g_alac_lock);
    send_bytes(m_on_audio_pcm, pcm, pcm_len, 0, false);
}

static void cb_audio_flush(void *cls)
{
    call_void(m_on_audio_flush);
}

static double cb_audio_set_client_volume(void *cls)
{
    return 0.0;
}

/* volume arrives in dB, from -30.0 (quiet) to 0.0; -144.0 means mute */
static void cb_audio_set_volume(void *cls, float volume)
{
    call_void(m_on_audio_volume, (jfloat) volume);
}

/* DMAP-encoded track info (title, artist, album); parsed on the Java side */
static void cb_audio_set_metadata(void *cls, const void *buffer, int buflen)
{
    send_bytes(m_on_audio_metadata, buffer, buflen, 0, false);
}

/* album art as JPEG */
static void cb_audio_set_coverart(void *cls, const void *buffer, int buflen)
{
    send_bytes(m_on_audio_coverart, buffer, buflen, 0, false);
}

static void cb_audio_stop_coverart_rendering(void *cls)
{
    call_void(m_on_audio_coverart_stop);
}

static void cb_audio_set_progress(void *cls, uint32_t *start, uint32_t *curr, uint32_t *end) {}

/* ---- AirPlay video (the AirPlay button in YouTube, Safari, ...): the client sends a URL ---- */

static void cb_on_video_play(void *cls, const char *location, const float start_position)
{
    LOGI("play video %s from %.1f s", location, start_position);
    JNIEnv *env = get_env();
    if (!env || !g_server) return;
    jstring url = (*env)->NewStringUTF(env, location);
    (*env)->CallVoidMethod(env, g_server, m_on_video_play, url, (jfloat) start_position);
    clear_exception(env);
    (*env)->DeleteLocalRef(env, url);
}

static void cb_on_video_scrub(void *cls, const float position)
{
    call_void(m_on_video_scrub, (jfloat) position);
}

/* rate 1 plays, 0 pauses */
static void cb_on_video_rate(void *cls, const float rate)
{
    call_void(m_on_video_rate, (jfloat) rate);
}

static void cb_on_video_stop(void *cls)
{
    call_void(m_on_video_stop);
}

static float cb_on_video_playlist_remove(void *cls)
{
    JNIEnv *env = get_env();
    if (!env || !g_server) return 0.0f;
    jfloat position = (*env)->CallFloatMethod(env, g_server, m_on_video_playlist_remove);
    clear_exception(env);
    return position;
}

/* Java returns { duration, position, rate }: duration -1 when finished, position -1 while loading */
static void cb_on_video_acquire_playback_info(void *cls, playback_info_t *info)
{
    info->duration = -1.0;
    info->position = -1.0;
    info->rate = 0.0f;
    JNIEnv *env = get_env();
    if (!env || !g_server) return;
    jdoubleArray result = (jdoubleArray) (*env)->CallObjectMethod(env, g_server, m_get_playback_info);
    if ((*env)->ExceptionCheck(env) || !result) {
        clear_exception(env);
        return;
    }
    jdouble values[3];
    (*env)->GetDoubleArrayRegion(env, result, 0, 3, values);
    (*env)->DeleteLocalRef(env, result);

    info->duration = values[0];
    info->position = values[1];
    info->rate = (float) values[2];
    info->seek_start = 0.0;
    info->seek_duration = values[0] > 0 ? values[0] : 0.0;
    info->ready_to_play = true;
    info->playback_likely_to_keep_up = true;
    info->playback_buffer_empty = false;
    info->playback_buffer_full = true;
}
static void cb_audio_remote_control_id(void *cls, const char *dacp_id, const char *active_remote_header) {}
static void cb_export_dacp(void *cls, const char *active_remote, const char *dacp_id) {}

static void cb_report_client_request(void *cls, char *deviceid, char *model, char *name, bool *admit)
{
    LOGI("client: %s (%s)", name ? name : "?", model ? model : "?");
    *admit = true;
}

static void cb_log(void *cls, int level, const char *msg)
{
    __android_log_print(level <= LOGGER_ERR ? ANDROID_LOG_ERROR :
                        level <= LOGGER_NOTICE ? ANDROID_LOG_WARN : ANDROID_LOG_INFO, "UxPlay", "%s", msg);
}

/* ---- JNI ---- */

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
    g_vm = vm;
    pthread_key_create(&g_env_key, detach_thread);
    return JNI_VERSION_1_6;
}

static void set_features(dnssd_t *dnssd, bool h265)
{
    /* UxPlay's feature set with HLS on: bits 0 (video) and 4 (HTTP live streaming) let apps
       send a video URL through their AirPlay button instead of mirroring the screen */
    static const int on_bits[] = { 0, 1, 2, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
                                   21, 22, 25, 27, 28, 30 };
    for (int bit = 0; bit < 32; bit++) {
        dnssd_set_airplay_features(dnssd, bit, 0);
    }
    for (size_t i = 0; i < sizeof(on_bits) / sizeof(on_bits[0]); i++) {
        dnssd_set_airplay_features(dnssd, on_bits[i], 1);
    }
    /* bit 42, "Supports Screen Multi Codec": lets the client send h265, needed for 4K */
    dnssd_set_airplay_features(dnssd, 42, h265 ? 1 : 0);
}

static void stop_server(JNIEnv *env)
{
    if (g_raop) {
        raop_destroy(g_raop);
        g_raop = NULL;
    }
    pthread_mutex_lock(&g_alac_lock);
    if (g_alac) {
        alac_decoder_destroy(g_alac);
        g_alac = NULL;
    }
    pthread_mutex_unlock(&g_alac_lock);
    if (g_dnssd) {
        dnssd_unregister_raop(g_dnssd);
        dnssd_unregister_airplay(g_dnssd);
        dnssd_destroy(g_dnssd);
        g_dnssd = NULL;
    }
    if (g_server) {
        (*env)->DeleteGlobalRef(env, g_server);
        g_server = NULL;
    }
    g_port = 0;
}

JNIEXPORT jboolean JNICALL
Java_com_codeaayu_airplaytv_AirPlayServer_nativeStart(JNIEnv *env, jobject thiz, jstring jname,
                                                      jbyteArray jhwaddr, jstring jkeyfile,
                                                      jint width, jint height, jint fps, jboolean h265,
                                                      jstring jlang)
{
    if (g_raop) {
        return JNI_TRUE;
    }

    jclass cls = (*env)->GetObjectClass(env, thiz);
    m_on_video_format = (*env)->GetMethodID(env, cls, "onVideoFormat", "(ZII)V");
    m_on_video_data = (*env)->GetMethodID(env, cls, "onVideoData", "([BJ)V");
    m_on_audio_format = (*env)->GetMethodID(env, cls, "onAudioFormat", "(II)V");
    m_on_audio_data = (*env)->GetMethodID(env, cls, "onAudioData", "([B)V");
    m_on_audio_volume = (*env)->GetMethodID(env, cls, "onAudioVolume", "(F)V");
    m_on_audio_flush = (*env)->GetMethodID(env, cls, "onAudioFlush", "()V");
    m_on_connection = (*env)->GetMethodID(env, cls, "onConnection", "(Z)V");
    m_on_mirror_running = (*env)->GetMethodID(env, cls, "onMirrorRunning", "(Z)V");
    m_on_audio_pcm = (*env)->GetMethodID(env, cls, "onAudioPcm", "([B)V");
    m_on_audio_metadata = (*env)->GetMethodID(env, cls, "onAudioMetadata", "([B)V");
    m_on_audio_coverart = (*env)->GetMethodID(env, cls, "onAudioCoverArt", "([B)V");
    m_on_audio_coverart_stop = (*env)->GetMethodID(env, cls, "onAudioCoverArtStop", "()V");
    m_on_video_play = (*env)->GetMethodID(env, cls, "onVideoPlay", "(Ljava/lang/String;F)V");
    m_on_video_scrub = (*env)->GetMethodID(env, cls, "onVideoScrub", "(F)V");
    m_on_video_rate = (*env)->GetMethodID(env, cls, "onVideoRate", "(F)V");
    m_on_video_stop = (*env)->GetMethodID(env, cls, "onVideoStop", "()V");
    m_on_video_playlist_remove = (*env)->GetMethodID(env, cls, "onVideoPlaylistRemove", "()F");
    m_get_playback_info = (*env)->GetMethodID(env, cls, "getPlaybackInfo", "()[D");
    if ((*env)->ExceptionCheck(env)) {
        clear_exception(env);
        return JNI_FALSE;
    }
    g_server = (*env)->NewGlobalRef(env, thiz);

    if (!g_ntp_initialized) {
        ntp_global_init();
        g_ntp_initialized = true;
    }

    const char *name = (*env)->GetStringUTFChars(env, jname, NULL);
    const char *keyfile = (*env)->GetStringUTFChars(env, jkeyfile, NULL);
    jsize hw_len = (*env)->GetArrayLength(env, jhwaddr);
    char hwaddr[MAX_HWADDR_LEN];
    (*env)->GetByteArrayRegion(env, jhwaddr, 0, MAX_HWADDR_LEN, (jbyte *) hwaddr);

    char device_id[3 * MAX_HWADDR_LEN];
    snprintf(device_id, sizeof(device_id), "%02x:%02x:%02x:%02x:%02x:%02x",
             (unsigned char) hwaddr[0], (unsigned char) hwaddr[1], (unsigned char) hwaddr[2],
             (unsigned char) hwaddr[3], (unsigned char) hwaddr[4], (unsigned char) hwaddr[5]);

    bool ok = false;
    int error = 0;
    g_dnssd = dnssd_init(name, (int) strlen(name), hwaddr, hw_len, 0, &error);
    if (error || !g_dnssd) {
        LOGE("dnssd_init failed: %d", error);
        goto done;
    }
    set_features(g_dnssd, h265);

    raop_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.conn_init = cb_conn_init;
    cbs.conn_destroy = cb_conn_destroy;
    cbs.conn_reset = cb_conn_reset;
    cbs.conn_feedback = cb_conn_feedback;
    cbs.audio_process = cb_audio_process;
    cbs.video_process = cb_video_process;
    cbs.audio_flush = cb_audio_flush;
    cbs.video_flush = cb_video_flush;
    cbs.video_pause = cb_video_pause;
    cbs.video_resume = cb_video_resume;
    cbs.audio_set_client_volume = cb_audio_set_client_volume;
    cbs.audio_set_volume = cb_audio_set_volume;
    cbs.audio_get_format = cb_audio_get_format;
    cbs.audio_set_metadata = cb_audio_set_metadata;
    cbs.audio_set_coverart = cb_audio_set_coverart;
    cbs.audio_stop_coverart_rendering = cb_audio_stop_coverart_rendering;
    cbs.audio_set_progress = cb_audio_set_progress;
    cbs.audio_remote_control_id = cb_audio_remote_control_id;
    cbs.export_dacp = cb_export_dacp;
    cbs.report_client_request = cb_report_client_request;
    cbs.video_reset = cb_video_reset;
    cbs.video_set_codec_and_size = cb_video_set_codec_and_size;
    cbs.mirror_video_running = cb_mirror_video_running;
    cbs.on_video_play = cb_on_video_play;
    cbs.on_video_scrub = cb_on_video_scrub;
    cbs.on_video_rate = cb_on_video_rate;
    cbs.on_video_stop = cb_on_video_stop;
    cbs.on_video_playlist_remove = cb_on_video_playlist_remove;
    cbs.on_video_acquire_playback_info = cb_on_video_acquire_playback_info;

    g_raop = raop_init(&cbs);
    if (!g_raop) {
        LOGE("raop_init failed");
        goto done;
    }
    raop_set_log_callback(g_raop, cb_log, NULL);
    raop_set_log_level(g_raop, LOGGER_INFO);
    if (raop_init2(g_raop, 1, device_id, keyfile)) {
        LOGE("raop_init2 failed");
        goto done;
    }

    raop_set_plist(g_raop, "hls", 1);
    const char *lang = (*env)->GetStringUTFChars(env, jlang, NULL);
    snprintf(g_lang, sizeof(g_lang), "%s", lang);
    (*env)->ReleaseStringUTFChars(env, jlang, lang);
    raop_set_lang(g_raop, g_lang, "", g_lang);

    raop_set_plist(g_raop, "width", width);
    raop_set_plist(g_raop, "height", height);
    raop_set_plist(g_raop, "refreshRate", fps);
    raop_set_plist(g_raop, "maxFPS", fps);

    unsigned short tcp[2] = { 0, 0 };
    unsigned short udp[3] = { 0, 0, 0 };
    raop_set_tcp_ports(g_raop, tcp);
    raop_set_udp_ports(g_raop, udp);

    g_port = raop_get_port(g_raop);
    if (raop_start_httpd(g_raop, &g_port) < 0) {
        LOGE("raop_start_httpd failed");
        goto done;
    }
    raop_set_port(g_raop, g_port);
    raop_set_dnssd(g_raop, g_dnssd);

    if ((error = dnssd_register_raop(g_dnssd, g_port)) || (error = dnssd_register_airplay(g_dnssd, g_port))) {
        LOGE("announcing on the network (mDNS, UDP 5353) failed: %d", error);
        goto done;
    }
    LOGI("AirPlay server listening on port %u as \"%s\", offering %dx%d@%d %s", g_port, name,
         width, height, fps, h265 ? "h265" : "h264");
    ok = true;

done:
    (*env)->ReleaseStringUTFChars(env, jname, name);
    (*env)->ReleaseStringUTFChars(env, jkeyfile, keyfile);
    if (!ok) {
        stop_server(env);
    }
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_codeaayu_airplaytv_AirPlayServer_nativeStop(JNIEnv *env, jobject thiz)
{
    stop_server(env);
}

JNIEXPORT jint JNICALL
Java_com_codeaayu_airplaytv_AirPlayServer_nativeGetPort(JNIEnv *env, jobject thiz)
{
    return (jint) g_port;
}

