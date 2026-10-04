# Katkıda bulunma

PHStore2 ücretsiz bir topluluk projesidir. Issue açarak hata bildirimi veya Pull Request ile kaynak katkısı yapabilirsin. Mevcut PH tasarımı ve PH/SP kaynak ayrımını koruyan küçük, incelenebilir değişiklikler tercih edilir.

1. Depoyu fork et ve değişiklik için bir branch aç.
2. [BUILDING.md](BUILDING.md) üzerinden harici SDK/toolchain'i kur. SDK'yı veya kişisel anahtarlarını commit etme.
3. İlgili native/frontend kodunu güncelle. Katalog şema alanları eklerken eski kayıtları desteklemeye devam et.
4. İlgili testleri, Release build'i ve `python tools/check_project.py` komutunu çalıştır.
5. PR açıklamasına problemi, değişen davranışı ve test sonucunu yaz. PC testi, native derleme ve gerçek PS5 testi sonuçlarını ayrı belirt.

Hata bildiriminde build ID, firmware/yükleyici bilgisi, PH/SP seçimi, dosya türü ve redakte edilmiş log özeti yararlıdır. IP, token, paylaşım query'si, OAuth veya kullanıcı bilgilerini logdan kaldır. Oyun gövdelerini issue'ya yükleme.

Katkı alanları: kaynak kodlu SP konteyner downloader, indirme hızı/bellek optimizasyonu, TLS ve yönlendirme testleri, firmware kurulum uyumluluğu, kuyruk davranışı, kapak önbelleği, erişilebilirlik ve dokümantasyon.

Native PH indirmede `SSL_VERIFYPEER=1` / `SSL_VERIFYHOST=2` korunsun. TLS doğrulamasını kapatan deneyler production build'e taşınmasın. Bir indirme tamamlandı olayını başarılı PKG kurulumu veya oynanabilirlik olarak raporlama.

PH kaynak katkıları mevcut GPL-3.0-or-later lisansı ile paylaşılır. Üçüncü taraf kaynakları için özgün lisans başlıklarını ve provenance notlarını koru. Lisansı bilinmeyen SP binary'sine yeni bir lisans atanmaz.
