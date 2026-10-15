# YerelAjan 0.1

Windows x64 için Türkçe ve İngilizce müşteri hizmetleri uygulaması. Qwen yerel araçları seçer; işletme yanıtları SQLite bilgi tabanından, müsaitlik ve işlem sonuçları gerçek yerel araçlardan gelir. Randevu oluşturma/değiştirme/iptal, mesaj bırakma, çalışan devri ve JSON-RPC eklentileri çalışır.

## Başlatma

`YerelAjan-Setup.exe` tüm modelleri içerir; kurulum ve ilk çalıştırma model indirmez. Başlat menüsünden YerelAjan’ı açın. Taşınabilir kullanım için `app/YerelAjan.exe` dosyasını çalıştırabilirsiniz. Gerekli Microsoft runtime kurulmamışsa installer paket içindeki imzalı `vc_redist.x64.exe` dosyasını çalıştırır; bu önkoşul yönetici izni isteyebilir.

Windows 10/11 x64, AVX2/FMA/F16C destekli CPU ve yaklaşık 2 GB boş disk gerekir. Kaynak paketi ve yeniden derleme için ayrıca disk ayırın. GPU, Python veya PyTorch çalışma uygulamasına gerekmez. İşletmenin Windows saat dilimini İstanbul olarak ayarlayın.

Örnek konuşma: “Randevu istiyorum” → “yarın” → sunulan bir saat → ad/soyad → “evet”. Yazma işlemleri müşterinin açık onayından sonra yapılır. “Randevumu … olarak değiştir” veya “Randevumu iptal et, kod …” ile devam edebilirsiniz. “Mesaj bırakmak istiyorum: …” mesajı onaya sunar. “çalışan” görüşmeyi devreder. `/en` ve `/tr` dili değiştirir; yaygın İngilizce açılışlar da tanınır.

## İşletmeyi özelleştirme

Tek çalışma yapılandırması `app/config.yaml` dosyasıdır. Dosya **JSON sözdiziminde YAML 1.2** kullanır; genel YAML girintili yazımı desteklenmez. `business.profile` ve `business.facts` yollarını `profiles/shop`, `clinic` veya `restaurant` içindeki örneklere yönlendirin. Profilde hizmet, açıklama, ücret (kuruş), süre, personel/kaynak, kapasite, çalışma saatleri ve ton bulunur. Bilgi dosyası JSON, Markdown veya Markdown/JSON dizini olabilir; başlangıçta FTS5’e aktarılır. Dosyaları değiştirdikten sonra uygulamayı yeniden başlatın.

Örnek profiller `demo:true` ve örnek fiyat/adres içerir. Gerçek verilerinizi girdikten sonra `demo:false` yapın; demo profille canlı kanallar açılamaz. Eklenti eklemek için `plugin-example.json` nesnesini config içindeki `plugins` listesine alın. `InventoryPlugin.exe` ve `plugins/inventory.json` örnek stok işlevidir. Şema zorunlu alanları ve türleri doğrular. Yazma eklentileri `permission:write` ister ve açık onaya sunulur. Belirsiz dış işlem sonucu otomatik tekrarlanmaz.

## Kanallar

Varsayılan bütün kanallar kapalıdır. SIP: yerel PBX bilgilerini girin, LAN için `bind_address` ekleyin; `staff_uri` çalışan aktarım adresidir. İngilizce başlayan hat için `channels.sip.language:"en"` kullanın. SMS: Gammu worker’ı SIM modem ve `device/connection` ile çalışır. WhatsApp: resmî Meta Cloud API için numara, hesap, herkese açık HTTPS ve sertifika gerekir; `configure-whatsapp.ps1` yerel HTTP.sys bağlamasını hazırlar. AI çekirdeğinde bulut API’si ve indirme yoktur; WhatsApp ayrı süreçte açık ağ istisnasıdır. SIP ve GSM bağlantıları iletişim altyapısı gerektirir.

Sırları config’e yazmayın. `YerelAjan.exe --secret NAME` stdin’den alıp aynı Windows hesabının DPAPI deposuna kaydeder. Servis için yükseltilmiş PowerShell’de `install-service.ps1` çalıştırın; sırları kaydeden hesap kullanılmalıdır. Servis scripti AI çekirdeğinin dış ağ erişimini Windows Firewall ile engeller. Masaüstü modunda da uygulama kendi ağ indirme/telemetri kodunu içermez; ONNX telemetrisi kapatılır.

## Test ve ölçüm

`ajan-tests.exe`, `YerelAjan.exe --speech-test`, `YerelAjan.exe --doctor` ve `benchmark.ps1 -Runs 6` kullanılabilir. Benchmark çıkış kodu 1 işlevsel hata, 2 ölçülen gecikme hedefinin aşılmasıdır. Python’lu kaynak testleri `integration_tests.py` ve `sip_integration.py`, yapay müşteri verisiyle ayrı dizinlerde çalışır. `--tts "metin" "ses.wav"` ve `--tts-en "text" "voice.wav"` dinleme örneği üretir.

38 güvenlik/entegrasyon kontrolü ve 14/14 yerel kabul senaryosu geçti. Türkçe EMA TTS hazır durum p95 yaklaşık **76 ms**, süreç tepe RAM **125 MB**. İngilizce Kokoro p95 **631 ms** ile 250 ms hedefini henüz karşılamıyor. LLM gerektiren metin senaryolarında maksimum **11.5 sn** ölçüldü. Ayrıntılı ölçümler, yöntem ve tamamlanmamış doğrulamalar `docs/VALIDATION.md` içindedir. Genel sesli konuşmaların tamamında <1,5 sn, 4/8 GB cihazlar, insan dinleme kalitesi ve canlı modem/Meta/PBX testleri henüz doğrulanmadı.

Kaynaklar ve derleme talimatları `docs/BUILD.md`, mimari `docs/ARCHITECTURE.md`, araştırma `docs/RESEARCH.md` içindedir. Uygulama GPL-3.0-or-later; ayrı SMS/Gammu süreci GPL-2.0-or-later. Modeller ve bağımlılıklar kendi lisanslarıyla dağıtılır; `app/licenses` ve `docs/THIRD_PARTY.md` bildirimleri içerir. Kaldırıcı işletme profillerini, config’i ve müşteri veri dizinini korur. Servis kurduysanız kaldırmadan önce `install-service.ps1 -Remove` kullanın.
