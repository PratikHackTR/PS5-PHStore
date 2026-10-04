# Derleme ve araçlar

## Harici SDK / toolchain

Bu klasör SDK, LLVM, CMake, Ninja, Python, Node.js, MSVC veya Windows SDK dağıtımı içermez. `third_party/gdrive-native/lib/*.a` dosyaları uygulamanın kaynakları ayrıca sağlanan bağımlılıklarıdır; SDK değildir.

Homebrew SDK: [ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk), [releases](https://github.com/ps5-payload-dev/sdk/releases). Upstream başlangıç dokümanı POSIX dağıtımını anlatır. Buradaki PH build script'i Windows için hazırlanmıştır; rastgele bir upstream ZIP'in Windows linker içerdiği varsayılmamalıdır. Uyumlu Windows portu/derlemesi ve aşağıdaki dosyalar gerekir:

```text
SDK/
  win/prospero-lld.exe
  target/include/...
  target/lib/crt1.o
  target/lib/libc.a
  target/lib/libpthread.a
  target/lib/libSce*.a
  target/lib/libkernel_web.a
  ldscripts/elf_x86_64.x
```

Bu teslimde kullanılan yerel SDK kopyasının doğrulanabilir upstream tag/commit kaydı yoktur. Bir sürüm numarası uydurulmaz; kullanılan CRT/linker/stub hash'leri [toolchain-reference.json](docs/toolchain-reference.json) içindedir. Standart upstream Linux/WSL build'inin bu projenin PowerShell build'iyle doğrudan uyumluluğu test edilmemiştir.

Derleyici: [LLVM](https://www.llvm.org/), `clang.exe`, `llvm-strip.exe`, `llvm-objdump.exe`, `llvm-ar.exe`, `llvm-ranlib.exe`. Doğrulanan yerel Clang sürümü **23.1.1**, kaynak revizyonu `6dfe1677ab8dffbc6ec13d53a1e0215d75147689`. Target `x86_64-sie-ps5`, PIE ve PS5 linker script'i kullanılır. Farklı LLVM sürümleri ayrıca doğrulanmalıdır.

Python tarafında bu teslim **Python 3.14.6** ile doğrulandı. Paketler:

```powershell
python -m pip install -r requirements-build.txt
# İsteğe bağlı signer/kapak araçları:
python -m pip install -r requirements-dev.txt
```

## PH ELF build

Depo kökünde veya başka bir çalışma klasöründen:

```powershell
.\tools\build.ps1 -Configuration Release `
  -SdkRoot 'C:\SDK\ps5-payload-sdk' `
  -LlvmBin 'C:\Program Files\LLVM\bin'
```

Alternatif olarak `PS5_PAYLOAD_SDK` ve `LLVM_BIN` ortam değişkenlerini ayarla ve `PHStore2_Build.bat` çalıştır. SDK yolu verilmezse script açıklayıcı hata ile durur.

Script önce `tools/prepare_embedded_catalog.py`, ardından `tools/build_assets.py` çalıştırır. Sonra PH native kodu ve kaynak kodlu PKG Manager helper'ını derler; curl/wolfSSL static kütüphanelerini ve sağlanan legacy SP helper'ını linkler. `tools/verify_ps5_installer_build.py` native çağrıları, stack sınırlarını ve gömülü kurulum helper'ının eşleşmesini kontrol eder.

`build/phstr.elf` ana çıktı, `build/install-helper.elf` bu derlemenin PKG kurulum yardımcısıdır. `dist/PHStore2.elf` değişmez. `BootstrapUrl` eski yapı uyumluluk parametresidir; default `https://phstore.invalid/bootstrap.json` gerçek bir hizmet adresi değildir. Gerçek katalog endpoint'leri `include/phstore_catalog_config.h` ve kaynak haritası dosyalarında tanımlıdır.

## curl / wolfSSL kaynak derlemesi

Tam değiştirilmiş kaynaklar dahil:

- `third_party/gdrive-native/vendor/curl-8.22.0`
- `third_party/gdrive-native/vendor/wolfssl-5.9.4-stable`
- `third_party/gdrive-native/config/user_settings.h`
- entropy, read-ahead/pending-drain ve AES-NI kontrol kodları.

[CMake](https://cmake.org/) ve [Ninja](https://ninja-build.org/) kurulu olmalı. İnternet gerekmeden sağlanan kaynaklardan:

```powershell
python tools/rebuild_native_deps.py `
  --sdk 'C:\SDK\ps5-payload-sdk' `
  --llvm 'C:\Program Files\LLVM\bin' `
  --cmake cmake --ninja ninja --jobs 8
.\tools\build.ps1 -SdkRoot 'C:\SDK\ps5-payload-sdk'
```

Bu komut yalnız bu checkout'un `build/native-deps/` ve `third_party/gdrive-native/lib/` dosyalarını günceller. Config/build logları `build/native-deps/logs/` içinde kalır. SDK kurulumu değiştirilmez. Normal PH build için hazır `.a` dosyaları yeterlidir.

CA: `third_party/gdrive-native/cacert.pem` ile `src/embedded_ca.h` aynı 188900 byte public root bundle'ı temsil eder. İndirme kaynağı [curl CA Extract](https://curl.se/ca/cacert.pem); mevcut bundle hash'i `a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505`. Dosyada private key bulunmaz. CA yenilemesi C header'ını ve hash kaydını birlikte güncellemelidir.

## Testler

```powershell
# PS5 build'den sonra; konsola bağlanmaz:
python tools/check_project.py
# İsteğe bağlı tüm Python testleri (cryptography kurulmuş olmalı):
python -m unittest discover -s tests -p "test_*.py" -v
# Windows C host parser testi; MSVC/Windows SDK ortamı açık terminalde:
clang -Iinclude tests/gdrive_protocol_harness.c src/phstore_gdrive_protocol.c -o build/protocol-test.exe
.\build\protocol-test.exe
```

`check_project.py` Node.js varsa frontend testlerini de çalıştırır; Node.js yoksa bunları **SKIP** raporlar. Bu teslimde Node **24.16.0** kullanıldı. Host test başarısı PS5 üzerinde TLS/indirme/kurulum başarısı anlamına gelmez.

## Metadata ve optional gateway

`tools/gdrive_metadata_generator.py` var olan rclone Drive remote'undan **salt okunur** dosya ID/path/size eşlemesi üretir. Remote ve kimlik bilgileri depoya konmaz. Ayrıntılar [docs/CATALOG.md](docs/CATALOG.md). [rclone lsjson](https://rclone.org/commands/rclone_lsjson/) verisi; runtime rclone'a bağımlı değildir.

`server/gateway.py` standart Python kütüphanesi kullanır. Yerel snapshot servis örneği:

```powershell
python server/gateway.py --bind 127.0.0.1 --port 8762
```

Bu localhost portunu açmak konsoldaki VDS hizmetini değiştirmez. SP PKG relay davranışının çalışması için konsolun kaynak haritasındaki gateway adresine erişmesi gerekir; PC localhost örneği otomatik production kurulum değildir. `--refresh-sp --token-file` opsiyonu operatörün **kendi** sağlayıcı erişim dosyasını gerektirir; depoda API token'ı bulunmaz.
