# Third-party code and licenses

This project as a whole is licensed under the **GNU General Public License v3.0** (see `LICENSE`) because it contains UxPlay code
(GPL-3.0). The following components come from third parties. Their license files are located next to them.

## Included in the repository

| Component | Folder | Origin | License |
|---|---|---|---|
| UxPlay protocol library (`lib/`) | `third_party/uxplay-lib` | <https://github.com/FDH2/UxPlay>, commit `ec20312` (see `UPSTREAM_REV`) | GPL-3.0 (`LICENSE-UxPlay`) |
| playfair (FairPlay handshake) | `third_party/uxplay-lib/playfair` | part of UxPlay | GPL-3.0 (`LICENSE.md`) |
| llhttp (HTTP parser) | `third_party/uxplay-lib/llhttp` | part of UxPlay, originally <https://github.com/nodejs/llhttp> | MIT (`LICENSE-MIT`) |
| mdnsd (mDNS responder) | `third_party/uxplay-lib/mdnsd` | part of UxPlay, © kgbook | LGPL-2.1 or later (file header) |
| libplist 2.7.0 | `third_party/libplist` | <https://github.com/libimobiledevice/libplist> | LGPL-2.1 or later (`COPYING.LESSER`) |

### Changes to UxPlay

All changes are marked “UWP adaptation” in the source:

- `httpd.c`: initialize and destroy `run_mutex` (a Windows `CRITICAL_SECTION` must be initialized).
- `raop_rtp.c`: signed packet length (`recv` often returns -1 on Windows), skip errors/empty packets, a few diagnostic log lines.
- `raop_ntp.c`: corrected `recvfrom` call (address length passed as a pointer, not a pointer to a pointer).
- `mdnsd/mdnsd.c`: `mdnsd_stop` joins the thread before closing the sockets (fixes a crash with `0xC0000008` on suspend).
- `crypto.c`: read/write the key file through a memory BIO (OpenSSL for UWP has no `FILE*` functions).
- `compat.c`, `compat.h`: `FormatMessageA` instead of `FormatMessage`; removed the `snprintf` → `_snprintf` macro.
- `http_handlers.h`: compile fix for `plist_get_data_val` (returns `void`) and the new export function
  `raop_hls_export_playlists` (declared in `raop.h`).

`third_party/compat` is original code (POSIX replacement for Windows).

## Fetched via vcpkg at build time (not in the repository)

| Component | Version | License | Note |
|---|---|---|---|
| FFmpeg (`avcodec`, `avutil`, `swresample`) | 9.0.2 | LGPL-2.1 or later | built without GPL parts, dynamically linked (DLLs in the package) |
| OpenSSL | 3.6.5 | Apache-2.0 | dynamically linked |
| Microsoft C++ runtime (`*_app.dll` from `Microsoft.VCLibs.140.00`) | 14.0 | Microsoft license for redistributable files | copied into the package by `tools/package.ps1` |

vcpkg places the license texts of FFmpeg and OpenSSL in `vcpkg_installed/x64-uwp/share/<package>/copyright`.

## Trademarks

AirPlay, iPhone, iPad and Mac are trademarks of Apple Inc. Xbox is a trademark of Microsoft Corporation. This project is a private
hobby project and is not affiliated with, endorsed by or reviewed by Apple or Microsoft.
