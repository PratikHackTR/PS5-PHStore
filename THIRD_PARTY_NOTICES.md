# Third-party components and license scope

PH Store's own sources are supplied under GPL-3.0-or-later; see [LICENSE](LICENSE). Individual third-party notices take precedence for their components. This file records supplied provenance; it does not grant missing rights.

| Component | Supplied materials | License / provenance |
| --- | --- | --- |
| curl 8.22.0 | Modified source, headers, PS5 `libcurl.a` | curl license; [COPYING](third_party/gdrive-native/CURL_COPYING) and vendor notices |
| wolfSSL 5.9.4-stable | Modified source, headers, user settings, PS5 `libwolfssl.a` | GPLv3; [COPYING](third_party/gdrive-native/WOLFSSL_COPYING) and vendor file notices |
| PS5 PKG Manager integration | Installer/helper/stream sources and headers | Supplied [GPLv3 license](third_party/ps5-pkg-manager/LICENSE), original file notices retained; PH adapters are separate sources |
| SQLite 3.46.0 | Embedded amalgamation in PKG Manager | Public-domain dedication in the supplied source |
| miniz | Embedded source in PKG Manager | Public-domain / unlicense notice in the supplied source |
| PS5 additional metadata | 147 saved records only | Supplied [GPL-3.0-or-later notice](config/examples/ph-ps5-metadata-NOTICE.md) and [license](config/examples/ph-ps5-metadata-LICENSE.txt); no upstream UI/runtime included |
| Mozilla root CA bundle | Public cert bundle and C header | From curl CA Extract; certificates are public root data, not private keys; source/hash in BUILDING.md |
| Spectrum legacy helper | `third_party/spectrum/exfat-helper.elf`; linked into current `dist/PHStore2.elf` | Source and redistribution license were not supplied. **Not assigned the PH GPL license.** See [NOTICE.md](third_party/spectrum/NOTICE.md) |

The Spectrum helper is an extracted binary from the supplied SpectrumLibrary 1.4.8 ELF, with a fixed-length log path adaptation. The native PH Google Drive/Archive downloader is built independently from supplied curl/wolfSSL source and does not copy Spectrum code. The legacy SP helper is a different runtime route.

The current release therefore has an explicitly documented **closed / license-unverified binary dependency**. A completely source-rebuildable open-source runtime requires replacing this helper with a source implementation or obtaining its source/license. The supplied full PH source cannot regenerate that helper.

Vendor test certificates/keys are upstream public fixtures, not PH credentials. Root bundle strings such as PEM parser begin/end markers in an ELF are not embedded PH private keys. Publisher game names, external cover URLs and provider metadata remain separate from the PH code license. No game data files are bundled.

SDKs and compiler distributions are intentionally external; their licenses and installation instructions belong to their upstream projects.
