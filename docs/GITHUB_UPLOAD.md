# GitHub'a yükleme

Repo kökü bu klasörün kendisi olmalı: `README.md`, `src/`, `frontend/`, `dist/` GitHub'da üst seviyede görünmeli. İçinde SDK yok; `.gitignore` sonraki build/log/secret dosyalarını da hariç tutar. ZIP yedek/aktarım içindir; kodu GitHub'da gezilebilir paylaşmak için ZIP'i aç ve bu klasörün içeriğini yükle.

Çok sayıda vendored kaynak dosyası bulunduğundan Git veya GitHub Desktop ile mevcut bir boş repoya eklemek kolaydır. Git ile, **bu klasörde**:

```powershell
git init -b main
git add .
git status --short
git commit -m "Initial PHStore2 source and ELF release"
git remote add origin https://github.com/YOUR_ACCOUNT/PHStore2.git
git push -u origin main
```

`YOUR_ACCOUNT` yerine kendi hesabını kullan ve GitHub'da boş repository oluştur. Bu komutlar otomatik çalıştırılmadı; GitHub'da yayın işlemi kullanıcıya aittir. Mevcut başka bir repoda/çalışma klasöründe bu komutları çalıştırma.

`dist/PHStore2.elf` repository içine alınacak şekilde `.gitignore` hazırlandı. İstersen ayrıca GitHub Release asset'i olarak aynı ELF ve SHA256SUMS dosyasını ekle. SDK, yerel build klasörü, kullanıcı logları veya geçmiş test arşivlerini ekleme.

PH kaynakları GPL-3.0-or-later altında paylaşılır. README'deki SP legacy yardımcı binary'nin kaynak/lisans eksikliği açıklamasını kaldırma; mevcut ELF'in tamamını açık kaynak olarak etiketleme. İlgili lisans notları repository içinde korunur.
