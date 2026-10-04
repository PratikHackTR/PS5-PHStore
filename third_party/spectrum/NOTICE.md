# Spectrum legacy helper provenance

`exfat-helper.elf` was extracted from the locally supplied SpectrumLibrary_1.4.8.elf. The build uses this supplied binary; its source was not supplied. The adapted file changes the fixed-length `/data/SpectrumLibrary/download.log` string to `/data/phstore2/download.log` using NUL padding. Per-download configuration slots are changed in a disposable memory copy before launch.

No source-code or redistribution license was supplied for this helper. The PH Store GPL license does not cover this binary. The current `dist/PHStore2.elf` embeds it. A fully source-rebuildable open-source release requires a source replacement or provision of the helper source/license.

The helper contains curl/wolfSSL implementation code. The bundled `SPECTRUM_INDIRME_RAPORU.md` is a historical static analysis report, not a current PHStore2 runtime verification report. Its earlier test-status statements refer to the analyzed sample at that time.

The native PH Google Drive/Archive path uses separately source-built curl/wolfSSL and copies no Spectrum binary code. The original Spectrum launcher, UI, updater, DPI executable and branded shortcut are not integrated.
