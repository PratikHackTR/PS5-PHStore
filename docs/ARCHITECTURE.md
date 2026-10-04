# Mimari

```mermaid
flowchart TD
    ELF[PHStore2 ELF] --> UI[Gömülü frontend · loopback 1903]
    ELF --> CAT[Gömülü katalog + canlı PH güncellemesi]
    UI --> PH[PH yöntemi]
    UI --> SP[SP yöntemi]
    PH --> NATIVE[Kaynak kodlu curl + wolfSSL downloader]
    NATIVE --> DRIVE[Google Drive public]
    NATIVE --> ARCHIVE[Archive HTTPS]
    NATIVE --> DISK[/data/homebrew]
    PH --> PKG[Native PKG kurulum / stream]
    SP --> PKG
    SP --> HELPER[Legacy SP konteyner helper binary]
    HELPER --> DISK
    PKG --> APP[AppInstUtil + kaynak kodlu kurulum helper]
    CAT --> CACHE[Kapak önbelleği]
```

Frontend saf JavaScript'tir. Native servis katalog doğrulama, kaynak çözümleme, kurulum ve indirme durumunu API üzerinden arayüze verir. Desktop `preview.py` yalnız sahte idle durumuyla tasarım önizlemesi sağlar.

Google Drive / Archive native HTTPS yolunun certificate/hostname kontrolü açıktır. Range segmentleri exact HTTP 206, Content-Range ve uzunluk kontrolleriyle alınır; kayıt mevcut pause/cancel kontrollerini kullanır. Hız sabit değildir; PS5 ağı, sağlayıcı, disk ve worker seçimine bağlıdır.

SP binary downloader ile kaynak kodlu PKG kurulum yardımcısı iki farklı bileşendir. Native PKG helper her build'de kaynak koddan derlenir. SP helper'ın yeniden üretilebilir kaynağı bu depoda yoktur. Fonksiyon/süreç ayrıntıları ilgili kaynak ve provenance notlarında bulunur.

İç stream/IPC portları 1925/1926; native ELF yükleyici 9021 yapılandırması ayrı bağımlılıktır. Aktif Spectrum helper UDP progress yolu 9877 kullanır. Çakışmalar konsol servis durumuyla birlikte değerlendirilmelidir.
