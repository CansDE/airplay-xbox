# AirPlay Xbox

[Deutsche Version](README.de.md)

An AirPlay receiver for the **Xbox Series X**, built as a UWP app that runs in Developer Mode. iPhone, iPad and Mac can mirror their
screen and audio to the Xbox via *Control Center → Screen Mirroring*. Audio-only AirPlay (music) works as well, and so does
**AirPlay video** (the AirPlay button in apps such as YouTube or Twitch).

**Status: runs on the Xbox Series X (Developer Mode).** Tested with an iPhone; see “Test status” for details.

The app's on-screen text (status screen, error messages) follows the system language: German on German systems, English
everywhere else.

## Architecture

| Component | Implementation |
|---|---|
| AirPlay protocol (RTSP/HTTP, pairing, FairPlay, decryption, NTP timing) | [UxPlay](https://github.com/FDH2/UxPlay) `lib/` (GPL-3.0) in `third_party/uxplay-lib`, adapted for UWP |
| Bonjour/mDNS advertisement | UxPlay's built-in `mdnsd` responder (no Bonjour needed) |
| plist parsing | [libplist](https://github.com/libimobiledevice/libplist) (LGPL-2.1) in `third_party/libplist` |
| POSIX gaps on Windows | `third_party/compat/` (pthread replacement, `ssize_t`, …) |
| Video | FFmpeg H.264, preferring the D3D11VA hardware decoder with a software fallback; output through the D3D11 video processor (letterboxed) |
| Audio | FFmpeg (AAC-ELD, AAC-LC, ALAC) → XAudio2; picture and sound are kept in sync using the sender's timestamps (200 ms buffer) |
| AirPlay video (HLS/URL) | UxPlay's HLS part (`/play`, FCUP playlists from the iPhone) → Windows `MediaPlayer` with `AdaptiveMediaSource`; frames go into the swap chain via frame server mode |
| UI | UWP without XAML: `CoreApplication` + D3D11 swap chain; Direct2D status screen |

App source code: `src/` (`AirPlayServer`, `VideoPipeline`, `AudioPipeline`, `HlsPlayer`, `Renderer`, `main.cpp`).

## Building (Windows)

Requirements: Visual Studio 2026 **Build Tools** (C++ workload, Windows 11 SDK 10.0.26100), Git.

```bat
git clone --depth 1 https://github.com/microsoft/vcpkg.git vcpkg
vcpkg\bootstrap-vcpkg.bat -disableMetrics
set VCPKG_ROOT=%CD%\vcpkg
vcpkg\vcpkg.exe install --triplet x64-uwp --host-triplet x64-windows   :: OpenSSL + FFmpeg, about 10 min
tools\build.bat                                                      :: CMake + Ninja -> build\AirPlayXbox.exe
python -I tools\make_assets.py package\Assets                         :: only needed if the icons are missing
powershell -NoProfile -ExecutionPolicy Bypass -File tools\package.ps1 :: -> build\AirPlayXbox.msix + build\AirPlayXboxDev.cer
```

On its first run, `package.ps1` creates a developer certificate `AirPlayXboxDev.pfx` in the project folder (do not commit it) and
copies the C++ runtime (`*_app.dll`) from the `Microsoft.VCLibs.140.00` package installed on the PC into the app package.

The paths to Visual Studio (`tools/build.bat`) and the Windows SDK (`tools/package.ps1`) are hard-coded; adjust them if your
installation differs. Tested with vcpkg commit `e456309` (FFmpeg 9.0.2, OpenSSL 3.6.5).

**Signing key:** A fresh clone gets its own key on the first `package.ps1` run. An installed app can only be updated with the same key,
so back up `AirPlayXboxDev.pfx`. With a new key, the old app has to be uninstalled first, and its settings are lost.

## Installing on the Xbox

1. **Create a developer account (before step 2, because activation codes expire):** go to <https://storedeveloper.microsoft.com> →
   “Get started for free” → **Individual developer**. Free for individuals since 2025; requires identity verification with a
   government ID and a selfie. Other entry points (directly through Partner Center or the Xbox) lead to the old, paid sign-up flow.
2. **Activate Developer Mode:** On the Xbox, install and open the **Xbox Dev Mode** app from the Store. Enter the code it shows at
   <https://partner.microsoft.com/xboxconfig/devices> (short link `aka.ms/activatexbox`) → “Agree and activate”. Then choose
   **Switch and restart** in the app. Plan for at least 5 GB of developer storage. If you are in the Xbox Insider program (Update
   Preview), leave it first, otherwise you get error E208. Warning: switching modes deletes Quick Resume states and local captures.
3. In **Dev Home**, under *Remote Access Settings*, enable the **Xbox Device Portal** (a username and password are recommended). The
   address (`https://<Xbox-IP>:11443`) is shown there. Open it in a browser on your PC and accept the certificate warning.
4. In the Device Portal, under **My games & apps → Add**, upload `build\AirPlayXbox.msix`. No dependencies are needed (the C++ runtime
   is inside the package). If you are asked for a certificate, provide `build\AirPlayXboxDev.cer`. Install.
5. Recommended: in Dev Home, highlight the app → View button → *View details* → **App type: Game**. Games get considerably more memory
   and GPU share than apps.
6. Start “AirPlay Xbox” from Dev Home. The device name appears on screen.
7. On the iPhone/Mac: *Control Center → Screen Mirroring* (Mac: menu bar → Control Center) → choose “Xbox Series X”.

Back to normal mode (games, Store apps): **Dev Home → Quick Actions → Leave Dev Mode**. To return to Developer Mode: *Xbox Dev Mode*
app → *Switch and restart*. The AirPlay app stays installed but only runs in Developer Mode.

Important: the iPhone/Mac and the Xbox must be on the **same network/VLAN**, and the Wi-Fi must not block multicast/Bonjour (no
“client isolation”).

## Usage and files

- **Controller button B** disconnects all clients.
- **Volume:** The app remembers the volume last set on the iPhone/Mac (LocalSettings `VolumeDb`) and reports it as `initialVolume` when
  a device connects, so the sender continues at that level. Without a saved value it starts at half volume (−15 dB); “mute” is not
  carried over.
- The device name can be changed: put a file `name.txt` (one line, UTF-8) into the app's `LocalState` folder (Device Portal →
  *File explorer* → `LocalAppData` → `AirPlayXbox.Receiver_…` → `LocalState`). Restart the app afterwards.
- **Log file:** `LocalState\airplay.log` in the same folder. It also contains UxPlay's protocol messages. Please attach it when
  reporting problems.
- The pairing key is stored in `LocalState\airplay_key.pem`. Deleting it gives the receiver a new identity (devices have to pair again).

## Known limitations

- **DRM content** (Netflix, Apple TV+, Disney+ …) stays black when mirroring. Apple enforces this.
- **AirPlay video:** The iPhone only hands over the video URL; the Xbox downloads the video from the internet itself. Playback is
  controlled from the iPhone (pause, seek, stop). UxPlay mainly tested this path with YouTube; other apps (Twitch, TikTok …) may behave
  differently. Protected content (FairPlay DRM, `fp-setup2`) cannot be played.
- A Developer Mode app only runs while the Xbox is in Developer Mode. For games and Store apps you have to switch back to Retail Mode.
- Mirroring latency is about 200–300 ms (a deliberate buffer for smooth audio and lip-synced video).
- License: because of UxPlay, the project as a whole is **GPL-3.0**.

## Test status

- **Xbox Series X (Developer Mode):** app installed via the Device Portal; discovery in the iPhone's Control Center (mDNS) and mirroring
  with picture and sound work. The bundled C++ runtime (VCLibs 14.0.33519) is sufficient.
- **Windows 11 PC** (Developer Mode, `Add-AppxPackage -Register build\layout\AppxManifest.xml`): mirroring, AirPlay video from Safari
  and Twitch, resuming from a saved position, suspending/resuming the app.
- **Known not to work:** TikTok sends no video request when its AirPlay button is used; only mirroring keeps running.
- **Not yet tested:** the YouTube path (playlists from the iPhone, `raop_hls_export_playlists`) and the MacBook.

Porting the UxPlay library required fixing four Windows-specific bugs, marked “UWP adaptation” in the source:
an uninitialized mutex in `httpd.c`, an `unsigned` packet length in `raop_rtp.c` (`recv` often returns -1 on Windows), a wrong
`recvfrom` call in `raop_ntp.c`, and `mdnsd_stop` in `mdnsd.c`, which closed sockets while the mDNS thread was still using them.
UWP checks handles strictly, so this crashed the app with `0xC0000008` when it was suspended (minimized, Xbox Home button).

If the D3D11VA hardware decoder fails, the app automatically falls back to the software decoder (noted in the log).

## License and notices

GPL-3.0, see [`LICENSE`](LICENSE). Third-party code, versions and licenses: [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

AirPlay, iPhone, iPad and Mac are trademarks of Apple Inc.; Xbox is a trademark of Microsoft Corporation. This is a private hobby
project with no affiliation to Apple or Microsoft. FairPlay compatibility comes from the open-source project UxPlay; the app is meant
for private use in Developer Mode, not for distribution through a store.
