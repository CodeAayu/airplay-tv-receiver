# AirPlay TV Receiver

An AirPlay receiver for Android TV / Google TV. Install it on the TV and your Mac, iPhone and iPad
can mirror their screen to it, or send music with the AirPlay button, the same way they would to
an Apple TV.

It is a thin Android app around [UxPlay](https://github.com/FDH2/UxPlay)'s AirPlay server, with
video and audio decoded by the TV's own hardware.

## Download

Get the APK from the [latest release](https://github.com/CodeAayu/airplay-tv-receiver/releases/latest),
then install it on the TV as described in [Installing on the TV](#installing-on-the-tv). It runs on
ARM TVs with Android 8.0 or newer. To build it yourself instead, see [Building](#building).

## What works

| From | How | Status |
|---|---|---|
| Mac | Control Center → Screen Mirroring, up to 4K 60 fps with sound | Tested |
| iPhone / iPad | AirPlay music from Apple Music, YouTube Music, Spotify, … (Apple Lossless) | Tested |
| iPhone / iPad | Control Center → Screen Mirroring | Should work (same path as the Mac) |
| iPhone / iPad | AirPlay button for video in apps (YouTube, Safari) | Built, not tested yet |

Tested on a Skyworth-made Google TV (`SWTV-24AE-4K NF`, Android TV 14, MediaTek MT5896) with a
MacBook Pro (M4 Pro) and an iPhone.

**Won't work:** Netflix, Prime Video, Disney+, Apple TV+ and other DRM-protected apps. Their streams
are protected with Apple's FairPlay DRM, which only Apple TVs and licensed TVs can decrypt.

## How it works

```
Apple device ──AirPlay──▶ UxPlay server (native, lib/raop) ──JNI──▶ Java
                              │                                      ├─ H.264/H.265 → MediaCodec (hardware) → SurfaceView
                              │                                      ├─ AAC-ELD / AAC → MediaCodec → AudioTrack
                              │                                      ├─ ALAC (music) → Apple ALAC decoder (native) → AudioTrack
                              │                                      └─ video links (HLS / URL) → MediaPlayer
                              └─ mDNS announcer (UxPlay lib/mdnsd) — makes the TV show up in AirPlay lists
```

- **Picture quality** is picked at startup from what the TV's hardware decoders support, best first:
  4K 60 fps H.265, 4K 30, 1080p 60, 1080p 30. The sender then chooses the codec and bitrate.
- **Discovery** uses UxPlay's own mDNS responder rather than Android's `NsdManager`, with a small
  patch (`patches/`) that also answers every query directly to the asking device. That works on
  home routers that drop multicast between Wi-Fi clients.
- **Name:** the TV is announced as `<TV name> (AirPlay)`, because Android TV's built-in cast
  services already use the plain device name.

## Building

Build scripts are for macOS (Apple Silicon or Intel). They call the Android SDK tools directly, so
no Gradle download is needed.

### 1. Tools

```bash
# Java 17 (any distribution; Azul Zulu shown), and the Android command-line tools
brew install --cask zulu@17 android-commandlinetools

# Android SDK packages
sdkmanager --sdk_root="$HOME/Library/Android/sdk" --licenses
sdkmanager --sdk_root="$HOME/Library/Android/sdk" \
    "platforms;android-35" "build-tools;35.0.0" "ndk;27.2.12479018" "cmake;3.22.1" "platform-tools"
```

The scripts find Java 17 with `/usr/libexec/java_home -v 17` and expect the SDK in
`~/Library/Android/sdk`. Set `JAVA_HOME` and `ANDROID_HOME` to use other locations.

### 2. Dependencies

```bash
./fetch-deps.sh      # downloads UxPlay, libplist, Apple ALAC and OpenSSL at pinned versions into third_party/
./build-openssl.sh   # builds OpenSSL's libcrypto for 32-bit ARM Android (a few minutes)
```

### 3. App

```bash
./build-apk.sh       # → build/AirPlayTV.apk
```

The output is signed with your local Android debug key, which the script creates if missing.

`settings.gradle` and `app/build.gradle` are included so the project opens in Android Studio, but
the Gradle build is untested; `build-apk.sh` is the tested path.

## Installing on the TV

1. On the TV, turn on developer options: **Settings → System → About**, then select
   **Android TV OS build** seven times.
2. Turn on **Settings → System → Developer options → Wireless debugging**.
3. Pair once. Select **Pair device with pairing code**, then on your Mac:
   ```bash
   adb pair <tv-ip>:<pairing-port> <6-digit-code>
   ```
4. Connect with the port shown on the main Wireless debugging screen, install and launch. Use the
   downloaded APK's path, or `build/AirPlayTV.apk` if you built it yourself:
   ```bash
   adb connect <tv-ip>:<port>
   adb install -r AirPlayTV-0.1.apk
   adb shell am start -n com.codeaayu.airplaytv/.MainActivity
   ```
5. Turn wireless debugging off again when you're done.

The app then appears on the TV's home screen as **AirPlay Receiver**.

## Using it

1. Open **AirPlay Receiver** on the TV. It only accepts connections while it is open on screen.
2. On a Mac: **Control Center → Screen Mirroring → `<TV name> (AirPlay)`**. For the sharpest picture,
   choose **Use As Separate Display** or set the TV display to its highest resolution in
   **System Settings → Displays**.
3. On iPhone / iPad: **Control Center → Screen Mirroring**, or the AirPlay button in a music app.

Any device on the same Wi-Fi network can connect, with no PIN. One device at a time: a new
connection takes over from the current one.

## Troubleshooting

**The TV doesn't appear in the AirPlay list.** Check, in order:

1. The AirPlay Receiver app is open on the TV.
2. The Apple device is on the same Wi-Fi network as the TV.
3. **Router Wi-Fi security.** If the network uses **WPA/WPA2 mixed mode with TKIP+AES**, recent
   Apple devices can't receive the router's broadcast traffic, so they never see any AirPlay or Cast
   devices, while ordinary connections keep working. Set the router's Wi-Fi security to
   **WPA2-PSK** with **AES** encryption, on every band (2.4 GHz and 5 GHz). This was the cause on
   the network this app was developed on.
4. AP / client isolation is off on the router.

**Reading logs:**

```bash
adb logcat -s AirPlayTV:* UxPlay:*
```

Some TVs have a small log buffer that other apps fill within minutes; `adb logcat -G 16M` makes it
larger until the next restart.

## Limitations

- 32-bit ARM (`armeabi-v7a`) only: the TV it was built for runs 32-bit apps only. Adding
  `arm64-v8a` needs OpenSSL built for that ABI too.
- The receiver stops when another app takes the screen, for example the TV's built-in Cast.
- AirPlay video links use Android's built-in `MediaPlayer`, which may not handle every HLS stream
  YouTube sends. For YouTube, the TV's built-in Google Cast works well.
- No PIN or password option yet.

## Project layout

```
app/src/main/cpp/
  native-lib.c        JNI bridge: starts UxPlay's server and forwards video, audio and video-link events
  dnssd_android.c     UxPlay dnssd backend using lib/mdnsd, with an Android-friendly host name
  alac_bridge.cpp     C wrapper around Apple's ALAC decoder
  CMakeLists.txt      builds libplist, UxPlay, ALAC and the JNI library
app/src/main/java/com/codeaayu/airplaytv/
  MainActivity.java   the full-screen receiver UI and its four states: idle, mirroring, video, music
  AirPlayServer.java  Java side of the native server, plus track-info parsing
  VideoDecoder.java   hardware H.264/H.265 decoding of the mirrored screen
  AudioPlayer.java    AAC decoding and PCM playback
  VideoLinkPlayer.java  plays video URLs sent by an app's AirPlay button
  VideoQuality.java   picks the best format the TV decodes in hardware
patches/              the one change to UxPlay
fetch-deps.sh  build-openssl.sh  build-apk.sh
```

## Credits and licenses

This app is licensed under the **GNU GPL v3** (see `LICENSE`), because it links UxPlay.

| Component | License |
|---|---|
| [UxPlay](https://github.com/FDH2/UxPlay) — AirPlay server (lib/raop, lib/mdnsd, playfair, llhttp) | GPL-3.0 (parts LGPL-2.1 / MIT) |
| [libplist](https://github.com/libimobiledevice/libplist) | LGPL-2.1 |
| [Apple ALAC](https://github.com/macosforge/alac) — Apple Lossless decoder | Apache-2.0 |
| [OpenSSL](https://github.com/openssl/openssl) 3.0.15 | Apache-2.0 |

AirPlay, Apple TV, Mac, iPhone and iPad are trademarks of Apple Inc. This project is not
affiliated with or endorsed by Apple.
