[![PS5 Bedava Oyun Mağazası](https://youtube.com)](https://www.youtube.com/watch?v=9TjY7C2KV9w)


# PHStore2

PS5 için ücretsiz PH Store istemcisi. C ile yazılmış native servis, ELF içine gömülü HTML/CSS/JavaScript arayüzü ve PH / SP kaynak seçimi içerir. PH kaynak kodu geliştirme ve katkı için GPL-3.0-or-later kapsamında paylaşılır.

**Lisans kapsamı:** Mevcut SP konteyner indirme yolu, kaynağı ve yeniden dağıtım lisansı sağlanmamış bir Spectrum yardımcı ELF'i içerir. Bu parça PH Store'un GPL lisansı kapsamında değildir. Dolayısıyla mevcut ELF'in bütün bileşenlerinin açık kaynak olduğu iddia edilmez. Ayrıntılar [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) ve [yardımcı binary notunda](third_party/spectrum/NOTICE.md). Açık kaynak bir SP downloader ile bu parçayı değiştirmek katkı hedeflerinden biridir.

## Kullanım

1. [dist/PHStore2.elf](dist/PHStore2.elf) dosyasını mevcut PS5 ELF yükleyicinle çalıştır. `elfldr` aktarım portu genellikle 9021'dir; kendi yükleyicindeki portu kullan.
2. Bildirimden servis/kısayol durumunu kontrol et. Kısayol kimliği `PHST00002`, uygulama adresi `http://127.0.0.1:1903/`.
3. Ana ekrandaki PHStore2 kısayolunu aç. Uygulama servisinin portu **1903**; bu port yükleyicinin aktarım portundan farklıdır. Native servis loopback adresinde dinler.
4. PH veya SP yöntemini seç; indirme durumlarını **İndirilenler** sekmesinden izle.

Frontend, katalog başlangıç kopyası, PKG kurulum yardımcısı ve native indirme yolunun public CA bundle'ı ELF içinde bulunur. Native CA dosyasını kullanıcı elle kopyalamaz. Çevrimiçi katalog ve dosya sağlayıcıları için ağ erişimi gerekir; kaynak sunucuların erişilebilirliği bu depodan bağımsızdır.

PKG kurulumunun firmware/yükleyici/servis uyumluluğu konsolda doğrulanmalıdır. `.exfat`, `.ffpfsc`, `.ffpkg`, `.ffpfs` gibi konteyner indirmeleri `/data/homebrew/` altına kaydedilir. Dosyanın tamamlanması mount veya oyunun çalışması için kanıt değildir; ShadowMount ayrı bir bileşendir ve bu pakete dahil değildir.

## Özellikler

- PH / SP iki ayrı kaynak, PS2 / PS4 / PS5 ve Türkçe oyun filtreleri.
- Türkçe oyun görünümünde dublaj işaretli oyunları da gösterme.
- İndirilenler sekmesi, ilerleme, hız, kalan süre, tamamlandı durumu ve mevcut indirme yollarının desteklediği duraklat/devam/iptal kontrolleri.
- PH kaydında `file_id` varsa PS5'ten doğrudan public Google Drive indirmesi.
- PH altında ek 147 PS5 metadata kaydı; üçüncü bir kaynak seçici yok.
- PKG için native kurulum akışı; diğer konteynerler için dosyaya indirme.
- Kapak önbelleği, yerleşik katalog başlangıç kopyası ve kurulu oyun sorgusu.

Native Google Drive / Archive yolu curl + wolfSSL kullanır; peer ve hostname doğrulaması açıktır. SP legacy yardımcı binary'nin kendi TLS davranışı bundan ayrıdır ve aynı güvenceye sahip olduğu varsayılmaz.

## Geliştirme

SDK ve derleyiciler depoya dahil değildir. Gerekli kurulum ve komutlar: **[BUILDING.md](BUILDING.md)**.

- [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk): homebrew sysroot, CRT, linker script ve SCE import stub'ları. Windows derleme için ayrıca uyumlu `win/prospero-lld.exe` gerekir. Sony'nin ticari SDK'sı kullanılmaz.
- [LLVM / Clang](https://www.llvm.org/): C ve assembly derleyicisi, strip ve artifact araçları.
- Python + Pillow: gömülü katalog/frontend/logo üretimi.
- CMake + Ninja: curl/wolfSSL'i yeniden kaynak koddan derlemek için.
- Node.js: isteğe bağlı frontend testleri; uygulamanın frontend'i için npm/React build'i gerekmez.
- MSVC Build Tools + Windows SDK: yalnız Windows üzerinde native **host testlerini** linklemek için; PS5 ELF derlemesinin zorunlu bağımlılığı değildir.

Başlangıç:

```powershell
python -m pip install -r requirements-build.txt
$env:PS5_PAYLOAD_SDK = 'C:\SDK\ps5-payload-sdk'
$env:LLVM_BIN = 'C:\Program Files\LLVM\bin'
.\tools\build.ps1
```

Çıktı `build/phstr.elf`. Hazır paylaşım ELF'i `dist/PHStore2.elf` içinde ayrıca korunur; build komutu bu dosyayı değiştirmez. `PHStore2_Build.bat` aynı build komutunu çağırır.

Masaüstü tasarım önizlemesi:

```powershell
python tools/preview.py --port 8763
```

`http://127.0.0.1:8763/` yalnız arayüz önizlemesidir; konsol kurulumunu veya ağ indirmesini gerçekleştirmez.

## Klasörler

| Yol | İçerik |
| --- | --- |
| `dist/` | Paylaşıma özel temiz katalogla derlenmiş hazır ELF ve SHA-256 |
| `src/`, `include/` | PS5 native servis ve downloader kodu |
| `frontend/` | Gömülü HTML/CSS/JavaScript arayüzü |
| `tools/` | Build, metadata, önizleme, test ve kapak araçları |
| `config/` | Katalog şeması örnekleri, başlangıç snapshot'ı ve 147 PS5 metadata |
| `server/` | Kaynakları ve manifestleri normalize eden Python gateway; snapshot girdileri |
| `third_party/` | curl/wolfSSL kaynakları ve PS5 static kütüphaneleri, PKG Manager kaynakları, SP yardımcı binary |
| `assets/`, `resim/` | Kısayol parametreleri ve PH görselleri |
| `tests/` | Host ve frontend kontrolleri |
| `docs/` | Mimari, katalog, doğrulama ve paylaşım notları |

## Bu paylaşımın durumu

Temel sürüm: `PHStore2_PH_CACHE_DUBBED_FASTSTART_PS5_SECURE`, build ID `phstore2-faststart-ph-ps5-20261004`. Eski PH/SP tasarımı korunmuştur. Başlangıç snapshot'ında 1011 PH + 531 SP oyun ve 305 gömülü SP manifesti bulunur. Oyun dosyalarının gövdeleri bu depoda bulunmaz.

Paylaşım kopyasında iki eski bağlantının erişim query değerleri çıkarılmış ve bu iki paket devre dışı bırakılmıştır. Bu yüzden `dist/PHStore2.elf` önceki teslim ELF'iyle byte olarak aynı değildir; görünüm ve downloader kodu korunur. Runtime katalog güncellemesi sağlayıcının güncel verisini ayrıca alır. Ayrıntılar [public-export.json](docs/public-export.json).

Derleme, host kontrolleri ve konsol testinin sınırları [VERIFICATION.md](docs/VERIFICATION.md) içinde, hazır ELF hash'i [dist/SHA256SUMS.txt](dist/SHA256SUMS.txt) içindedir. Bu **paylaşım ELF'i konsolda henüz denenmedi**.

Yayın klasörünün bütünlüğünü SDK veya ağ olmadan `python tools/verify_release.py` ile doğrulayabilirsin. [GitHub'a yükleme adımları](docs/GITHUB_UPLOAD.md) ayrı belgede bulunur.

## Katkı ve lisans

Issue ve Pull Request ile katkı yapabilirsin: [CONTRIBUTING.md](CONTRIBUTING.md). Özellikle downloader, SP helper'ın kaynak kodlu karşılığı, kurulum uyumluluğu, kapak önbelleği ve arayüz iyileştirmeleri için yardım beklenir.

PH Store kaynak lisansı [GPL-3.0-or-later](LICENSE). Üçüncü taraf lisansları kendi dosyalarında korunur. Katalog metadata'sı, oyun adları ve yayıncı kapaklarının sahipliği PH kod lisansından ayrı değerlendirilir; PH GPL lisansı oyun dosyaları veya yayıncı görselleri için hak vermez.
