# Fremdcode und Lizenzen

Dieses Projekt steht als Ganzes unter der **GNU General Public License v3.0** (siehe `LICENSE`), weil es UxPlay-Code (GPL-3.0) enthält.
Die folgenden Bestandteile stammen von Dritten. Ihre Lizenzdateien liegen jeweils im Ordner daneben.

## Im Repository enthalten

| Bestandteil | Ordner | Herkunft | Lizenz |
|---|---|---|---|
| UxPlay-Protokollbibliothek (`lib/`) | `third_party/uxplay-lib` | <https://github.com/FDH2/UxPlay>, Commit `ec20312` (siehe `UPSTREAM_REV`) | GPL-3.0 (`LICENSE-UxPlay`) |
| playfair (FairPlay-Handshake) | `third_party/uxplay-lib/playfair` | Teil von UxPlay | GPL-3.0 (`LICENSE.md`) |
| llhttp (HTTP-Parser) | `third_party/uxplay-lib/llhttp` | Teil von UxPlay, ursprünglich <https://github.com/nodejs/llhttp> | MIT (`LICENSE-MIT`) |
| mdnsd (mDNS-Responder) | `third_party/uxplay-lib/mdnsd` | Teil von UxPlay, © kgbook | LGPL-2.1 oder neuer (Dateikopf) |
| libplist 2.7.0 | `third_party/libplist` | <https://github.com/libimobiledevice/libplist> | LGPL-2.1 oder neuer (`COPYING.LESSER`) |

**Änderungen gegenüber UxPlay:** Für Windows/UWP angepasste Stellen sind im Quelltext mit „UWP-Anpassung“ markiert
(u. a. `httpd.c`, `raop_rtp.c`, `raop_ntp.c`, `mdnsd/mdnsd.c`). Dazu kommt die Exportfunktion `raop_hls_export_playlists`
(`http_handlers.h`, `raop.h`). `third_party/compat` ist eigener Code (POSIX-Ersatz für Windows).

## Beim Bauen per vcpkg geholt (nicht im Repository)

| Bestandteil | Version | Lizenz | Hinweis |
|---|---|---|---|
| FFmpeg (`avcodec`, `avutil`, `swresample`) | 9.0.2 | LGPL-2.1 oder neuer | ohne GPL-Teile gebaut, dynamisch gelinkt (DLLs im Paket) |
| OpenSSL | 3.6.5 | Apache-2.0 | dynamisch gelinkt |
| Microsoft C++-Laufzeit (`*_app.dll` aus `Microsoft.VCLibs.140.00`) | 14.0 | Microsoft-Lizenz für verteilbare Dateien | wird von `tools/package.ps1` ins Paket kopiert |

Die Lizenztexte von FFmpeg und OpenSSL legt vcpkg unter `vcpkg_installed/x64-uwp/share/<paket>/copyright` ab.

## Marken

AirPlay, iPhone, iPad und Mac sind Marken von Apple Inc. Xbox ist eine Marke der Microsoft Corporation. Dieses Projekt ist ein privates
Hobbyprojekt und steht in keiner Verbindung zu Apple oder Microsoft und wird von ihnen weder unterstützt noch geprüft.
