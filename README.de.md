# AirPlay Xbox

[English version](README.md)

AirPlay-Empfänger als UWP-App für die **Xbox Series X** (Developer Mode). iPhone, iPad und Mac können über
*Kontrollzentrum → Bildschirmsynchronisierung* Bild und Ton auf die Xbox spiegeln. Reines Audio-AirPlay (Musik) geht ebenfalls,
außerdem **AirPlay-Video** („Wiedergabe auf Xbox Series X“ in Apps wie YouTube oder Twitch).

**Stand: läuft auf der Xbox Series X (Developer Mode).** Getestet mit einem iPhone; Details unter „Teststand“.

## Aufbau

| Baustein | Umsetzung |
|---|---|
| AirPlay-Protokoll (RTSP/HTTP, Pairing, FairPlay, Entschlüsselung, NTP-Timing) | [UxPlay](https://github.com/FDH2/UxPlay)-`lib/` (GPL-3.0) in `third_party/uxplay-lib`, für UWP angepasst |
| Bonjour/mDNS-Ankündigung | UxPlays eingebauter `mdnsd`-Responder (kein Bonjour nötig) |
| plist-Parsing | [libplist](https://github.com/libimobiledevice/libplist) (LGPL-2.1) in `third_party/libplist` |
| Windows-Lücken zu POSIX | `third_party/compat/` (pthread-Ersatz, `ssize_t`, …) |
| Video | FFmpeg H.264, bevorzugt D3D11VA-Hardwaredecoder, sonst Software; Ausgabe über den D3D11-Video-Processor (Letterbox) |
| Audio | FFmpeg (AAC-ELD, AAC-LC, ALAC) → XAudio2; Bild und Ton werden über die Sender-Zeitstempel synchron ausgegeben (200 ms Puffer) |
| AirPlay-Video (HLS/URL) | UxPlays HLS-Teil (`/play`, FCUP-Playlists vom iPhone) → Windows-`MediaPlayer` mit `AdaptiveMediaSource`; Bilder per Frame-Server in die Swapchain |
| UI | UWP ohne XAML: `CoreApplication` + D3D11-Swapchain; Direct2D-Statusbildschirm |

Quellcode der App: `src/` (`AirPlayServer`, `VideoPipeline`, `AudioPipeline`, `HlsPlayer`, `Renderer`, `main.cpp`).

## Bauen (Windows)

Voraussetzungen: Visual Studio 2026 **Build Tools** (C++-Workload, Windows 11 SDK 10.0.26100), Git.

```bat
git clone --depth 1 https://github.com/microsoft/vcpkg.git vcpkg
vcpkg\bootstrap-vcpkg.bat -disableMetrics
set VCPKG_ROOT=%CD%\vcpkg
vcpkg\vcpkg.exe install --triplet x64-uwp --host-triplet x64-windows   :: OpenSSL + FFmpeg, ca. 10 Min
tools\build.bat                                                      :: CMake + Ninja -> build\AirPlayXbox.exe
python -I tools\make_assets.py package\Assets                         :: nur nötig, wenn die Icons fehlen
powershell -NoProfile -ExecutionPolicy Bypass -File tools\package.ps1 :: -> build\AirPlayXbox.msix + build\AirPlayXboxDev.cer
```

`package.ps1` legt beim ersten Lauf ein Entwicklerzertifikat `AirPlayXboxDev.pfx` im Projektordner an (nicht einchecken) und kopiert
die C++-Laufzeit (`*_app.dll`) aus dem auf dem PC installierten `Microsoft.VCLibs.140.00` app-lokal ins Paket.

Pfade zu Visual Studio (`tools/build.bat`) und zum Windows SDK (`tools/package.ps1`) sind fest eingetragen; bei anderer Installation
dort anpassen. Getestet mit vcpkg-Commit `e456309` (FFmpeg 9.0.2, OpenSSL 3.6.5).

**Signaturschlüssel:** Wer das Repository frisch klont, bekommt beim ersten `package.ps1` einen eigenen Schlüssel. Updates einer schon
installierten App gehen nur mit demselben Schlüssel, deshalb `AirPlayXboxDev.pfx` sichern. Mit einem neuen Schlüssel muss die alte
App zuerst deinstalliert werden (dabei gehen ihre Einstellungen verloren).

## Auf die Xbox bringen

1. **Entwicklerkonto anlegen (vor Schritt 2, Aktivierungscodes laufen ab):** über <https://storedeveloper.microsoft.com> →
   „Get started for free“ → **Individual developer**. Für Privatpersonen kostenlos (seit 2025), dafür Identitätsprüfung mit Ausweis und
   Selfie. Andere Einstiege (direkt über Partner Center oder Xbox) führen in den alten, kostenpflichtigen Ablauf.
2. **Developer Mode aktivieren:** Auf der Xbox im Store die App **Xbox Dev Mode** installieren und starten, den angezeigten Code unter
   <https://partner.microsoft.com/xboxconfig/devices> (Kurzlink `aka.ms/activatexbox`) eingeben → „Agree and activate“. Danach in der App
   **Switch and restart**. Mindestens 5 GB Entwicklerspeicher einplanen. Wer am Xbox-Insider-Programm (Update Preview) teilnimmt, verlässt
   es vorher (sonst Fehler E208). Achtung: Der Moduswechsel löscht Quick-Resume-Stände und lokale Aufnahmen.
3. In **Dev Home** unter *Remote Access Settings* das **Xbox Device Portal** einschalten (Benutzername/Passwort empfohlen). Die Adresse
   (`https://<Xbox-IP>:11443`) steht dort. Im PC-Browser öffnen, Zertifikatswarnung bestätigen.
4. Im Device Portal unter **My games & apps → Add** `build\AirPlayXbox.msix` hochladen. Abhängigkeiten sind keine nötig (C++-Laufzeit
   liegt im Paket). Falls nach einem Zertifikat gefragt wird, `build\AirPlayXboxDev.cer` angeben. Installieren.
5. Empfohlen: In Dev Home die App markieren → Ansicht-Taste → *View details* → **App type: Game**. Spiele bekommen deutlich mehr
   Arbeitsspeicher und GPU-Anteil als Apps.
6. App „AirPlay Xbox“ in Dev Home starten. Auf dem Bildschirm erscheint der Gerätename.
7. Auf iPhone/Mac: *Kontrollzentrum → Bildschirmsynchronisierung* (Mac: Menüleiste → Kontrollzentrum) → „Xbox Series X“ wählen.

Zurück zum normalen Modus (Spiele, Store-Apps): **Dev Home → Quick Actions → Leave Dev Mode**. Wieder in den Entwicklermodus: App
*Xbox Dev Mode* → *Switch and restart*. Die AirPlay-App bleibt installiert, läuft aber nur im Entwicklermodus.

Wichtig: iPhone/Mac und Xbox müssen im **selben Netzwerk/VLAN** sein, und das WLAN darf Multicast/Bonjour nicht blockieren (keine „Client-Isolation“).

## Bedienung und Dateien

- **Controller-Taste B** trennt alle Verbindungen.
- **Lautstärke:** Die App merkt sich die zuletzt am iPhone/Mac eingestellte Lautstärke (LocalSettings `VolumeDb`) und meldet sie beim Verbinden als `initialVolume`, sodass der Sender dort weitermacht. Ohne gespeicherten Wert startet sie bei halber Lautstärke (−15 dB), „Stumm“ wird nicht übernommen.
- Der Gerätename lässt sich ändern: Datei `name.txt` (eine Zeile, UTF-8) in den `LocalState`-Ordner der App legen (Device Portal →
  *File explorer* → `LocalAppData` → `AirPlayXbox.Receiver_…` → `LocalState`). Neustart der App nötig.
- **Logdatei:** `LocalState\airplay.log` im selben Ordner. Sie enthält auch die Protokollmeldungen von UxPlay. Bei Problemen bitte mitschicken.
- Der Pairing-Schlüssel liegt in `LocalState\airplay_key.pem`. Löschen = neue Geräte-Identität (Geräte müssen sich neu koppeln).

## Bekannte Grenzen

- **DRM-Inhalte** (Netflix, Apple TV+, Disney+ …) bleiben beim Spiegeln schwarz. Das erzwingt Apple.
- **AirPlay-Video:** Das iPhone übergibt nur die Video-Adresse, die Xbox lädt das Video selbst aus dem Internet. Gesteuert wird am iPhone
  (Pause, Spulen, Beenden). UxPlay hat diesen Weg vor allem mit YouTube erprobt; andere Apps (Twitch, TikTok …) können abweichen.
  Geschützte Inhalte (FairPlay-DRM, `fp-setup2`) lassen sich nicht abspielen.
- Eine App im Dev Mode läuft nur, solange die Xbox im Developer Mode ist. Für Spiele/Store muss in den Retail-Modus zurückgewechselt werden.
- Latenz beim Spiegeln ca. 200–300 ms (bewusster Puffer für gleichmäßigen Ton und lippensynchrones Bild).
- Lizenz: wegen UxPlay ist das Projekt insgesamt **GPL-3.0**.

## Teststand

- **Xbox Series X (Developer Mode):** App per Device Portal installiert; Erkennung im iPhone-Kontrollzentrum (mDNS), Spiegelung mit Bild
  und Ton laufen. Die app-lokale C++-Laufzeit (VCLibs 14.0.33519) reicht.
- **Windows-11-PC** (Entwicklermodus, `Add-AppxPackage -Register build\layout\AppxManifest.xml`): Spiegelung, AirPlay-Video aus Safari und
  Twitch, Weiterschauen ab gespeicherter Position, Pausieren/Fortsetzen der App.
- **Bekannt nicht möglich:** TikTok schickt beim AirPlay-Symbol keine Video-Anfrage, nur die Spiegelung läuft weiter.
- **Noch offen:** der YouTube-Weg (Playlists vom iPhone, `raop_hls_export_playlists`) und das MacBook.

Beim Portieren der UxPlay-Bibliothek waren vier Windows-spezifische Fehler zu beheben, im Quelltext mit „UWP-Anpassung“ markiert:
nicht initialisierter Mutex in `httpd.c`, `unsigned` Paketlänge in `raop_rtp.c` (`recv` liefert unter Windows oft -1), ein falscher
`recvfrom`-Aufruf in `raop_ntp.c` und `mdnsd_stop` in `mdnsd.c`, das Sockets schloss, während der mDNS-Thread sie noch benutzte. UWP prüft
Handles strikt, das führte beim Pausieren der App (Minimieren, Xbox-Home-Taste) zum Absturz mit `0xC0000008`.

Fällt der D3D11VA-Hardwaredecoder aus, schaltet die App automatisch auf den Software-Decoder (steht im Log).

## Lizenz und Hinweise

GPL-3.0, siehe [`LICENSE`](LICENSE). Fremdcode, Versionen und Lizenzen: [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

AirPlay, iPhone, iPad und Mac sind Marken von Apple Inc., Xbox ist eine Marke der Microsoft Corporation. Privates Hobbyprojekt ohne
Verbindung zu Apple oder Microsoft. Die FairPlay-Kompatibilität stammt aus dem Open-Source-Projekt UxPlay; die App ist für den
privaten Gebrauch im Developer Mode gedacht und nicht für den Vertrieb über einen Store.
