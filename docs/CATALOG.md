# Katalog ve native dosya indirme

Başlangıç PH verisi `config/examples/ph-live-snapshot.json`, ek 147 PS5 kaydı `config/examples/ph-ps5-extra-source.json`, SP saved katalog/manifest girdileri `server/data/` içindedir. `tools/prepare_embedded_catalog.py` bu yerel girdileri normalize eder; build sırasında internetten yeni katalog çekmez.

Üretilenler: `config/generated/embedded-catalog.json`, `embedded-manifests.json` ve `ph-ps5-covers.txt`. `tools/build_assets.py` bunları frontend ile ELF'e gömer. Canlı PH kaynağı mevcut yapılandırmada `http://148.135.181.3/phstore/catalog.json`; gateway/manifest yolları `include/phstore_source_map.h` içinde kayıtlıdır. Bu public hizmetler operatörden bağımsız bir üçüncü taraf için hizmet garantisi değildir. Fork kendi servis adreslerini açıkça yapılandırmalıdır.

Google Drive optional alanlar:

```json
{
  "id": "ph-gdrive-example",
  "type": "base",
  "platform": "ps5",
  "filename": "denemedosyasi.exfat",
  "action_type": "download_file",
  "source_type": "google_drive_public",
  "file_id": "1H_iIPrvBa8NSgsutbnkxr_a_JHMi-fN3",
  "size_bytes": 125369652,
  "sha256": "77aac9bdb0e37992535702e68bbe0c5828773cc0a3c0a4e8e6f028de7fd66f5b"
}
```

Runtime URL `https://drive.usercontent.google.com/download?id=<file_id>&export=download&confirm=t`. ID dosyayı public hale getirmez; Google izin/kota/HTML yanıtları ayrıca kontrol edilir. Bu test verisi oyun kurulumu değildir.

Eski `source_group` + `filename` veya `direct_http` kayıtları aynı şema içinde desteklenir. Ek PS5 verisi `archive_public` + `download_file` kullanır. Bu 147 kaydın beklenen publisher SHA-256 alanı yoktur; indirme sonunda hesaplanan hash, bağımsız beklenen hash doğrulaması yerine geçmez.

SP manifestlerinde toplam dosya boyutu, parça offset/boyut/url/hash değerleri bulunur. Alternatif kaynak/backport paketleri ayrı seçeneklerdir; PH yöntemiyle aynı dosya modeli değildir. PKG stream kurulumu ile konteyner dosyası kaydı ayrı akışlardır.

## One-time rclone metadata

Kendi mevcut Drive remote'unu kullanarak yalnız ID/path/size listesi üret:

```powershell
python tools/gdrive_metadata_generator.py --group PS5Games `
  --remote "MY_REMOTE:PS5Games" --output build/drive-id-map.json
```

Alternatif: VDS üzerinde `rclone lsjson` ile kaydedilmiş bir listeyi PC'de `--listing build/listing.json` ile işle. Oluşan ID'leri son katalog kayıtlarında doğrudan sakla; bu generator runtime bağımlılığı değildir. Dosya adı çakışmaları sessizce ilk kayıt seçilerek çözülmez. Generator SHA-256 üretmez, dosya gövdesini indirmez ve paylaşım/izin ayarını değiştirmez.

## Kapaklar ve dosyalar

Ek PS5 kapak URL'leri ELF içinde saklanır. Konsolda 97 PlayStation ve 50 ProsperoPatches URL'si mevcut önbelleğe alınır; kapak görsel gövdeleri pakete gömülü değildir. TLS doğrulaması, host sınırı, HTTP/body boyutu ve dosya imzası kontrolü uygulanır.

Dosyalar `/data/homebrew/`, kapak önbelleği `/data/phstore2/resimler`, genel loglar `/data/phstore2/logs/`, native indirme logu `/data/phstore2/gdrive/download.log`. Disk alanını indirmeden önce kontrol et. Süreç içindeki duraklat/devam desteği, uygulama yeniden başladıktan sonra byte seviyesinde resume garantisi değildir.
