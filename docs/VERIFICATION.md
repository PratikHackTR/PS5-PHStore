# Paylaşım doğrulaması — 2026-10-04

Bu tablo yalnız bu paylaşım kopyasında gerçekten yapılan kontrolleri gösterir.

| Kontrol | Sonuç |
| --- | --- |
| Başka bir çalışma klasöründen, external SDK ile Release PH build | PASS |
| Native PKG worker çağrıları, host mock yokluğu, gömülü kaynak kodlu helper eşleşmesi | PASS |
| Worker stack 11128 byte / serve_connection stack 36072 byte | PASS; ölçülen statik stack değerleri |
| Sağlanan vendor kaynaklarıyla curl + wolfSSL PS5 static library derlemesi | PASS; `--no-install` ile paketlenen hazır `.a` dosyaları korunarak |
| Python testleri: gateway / metadata / embedded snapshot / signer | PASS; 15 test |
| Node.js virtual grid / Türkçe-dublaj filtresi / İndirilenler arayüzü | PASS; 3 suite |
| PH native `src/` ve `include/` dosyalarının güncel proje ile byte eşliği | PASS |
| Frontend dosyalarının eski PH/SP tasarımıyla byte eşliği | PASS |
| Kataloglardaki credential query değerlerinin kaldırılması | PASS; 5 JSON kopyasındaki iki bağlantı |
| Paylaşım ELF'inde önceki erişim token değerlerinin bulunmaması | Paket finalize denetiminde ayrıca kontrol edilir; özet `release-manifest.json` |
| SDK/toolchain dağıtımlarının paylaşım klasörüne dahil olmaması | Paket finalize denetiminde ayrıca kontrol edilir |
| Bu paylaşım ELF'iyle PS5 TLS/indirme/kurulum | **NOT_TESTED** |
| Bütün oyun URL'lerinin/kapaklarının çalışması | **NOT_TESTED** |

PH kaynağındaki eski kota UI testi, geri dönülmüş frontend'de bulunmayan `downloadErrorMessage` fonksiyonuna bağlı olduğu için paylaşım test takımına alınmadı. Aktif arayüzün download ve Türkçe filtre regresyonları korunup çalıştırıldı. Kaynak projedeki dosyalar değiştirilmedi.

Public ELF SHA-256: `b9b188c5f94549de8887770a5acd07305098e02875e2c56c02d1b2b3e41e5699`.

Sonuçlar gerçek konsol kanıtı yerine geçmez. Önceki sürümlerin kullanıcı PS5 bildirimleri, bu paylaşım build'ine otomatik PASS olarak aktarılmadı. İki eski token'lı paket paylaşım başlangıç kataloğunda devre dışı; canlı katalog sağlayıcının verisini ayrıca güncelleyebilir.
