# İşletme Ajanı

Windows x64 için, küçük işletmelerin müşteri hizmetlerini yürüten Türkçe öncelikli ve İngilizce destekli yerel bir sesli/metin ajanı. Model çıkarımı ve işletme verileri bilgisayarda kalır; yapay zekâ çekirdeği bulut API’si kullanmaz ve telemetri kapalıdır.

[En güncel sürümden](https://github.com/Apollonny/isletme-ajani/releases/latest) çevrimdışı kurulum dosyasını indirin. `YerelAjan-Setup.exe` uygulama ve modelleri içerir; ilk çalıştırmada model indirmez. Yaklaşık 2 GB boş alan ve AVX2/FMA/F16C destekli Windows x64 CPU gerekir. Kurulum, eksik Microsoft çalışma zamanını yanında gelen resmî imzalı paketten kurabilir.

## Müşteri hizmetleri

- Dükkan, klinik ve restoran profilleri; hizmet, ücret, süre, personel, kapasite, çalışma saati ve konuşma tonu ile özelleştirilir.
- Bilgi yanıtları yerel SQLite FTS5 bilgi tabanından verilir. Kaynakta doğrulanmayan işletme bilgisi tahmin edilmez; gerekirse çalışan devri sunulur.
- Ajan randevu oluşturma, değiştirme ve iptal etme; hizmetleri karşılaştırma; mesaj kaydetme ve çalışan aktarımı gibi çok adımlı işleri yürütür. Yazma işlemlerini müşterinin açık onayından sonra yapar.
- SIP/PJSIP telefon, SIM modem üzerinden SMS ve Meta’nın resmî WhatsApp Cloud API geçidi desteklenir. WhatsApp geçidi internet ve Meta hesabı gerektiren ayrı bir kanaldır; diğer AI işlemleri yereldir.

Türkçe ses EMA Lightning ONNX, İngilizce ses quantized Kokoro ONNX, konuşma tanıma Whisper.cpp ve metin ajanı Qwen3-1.7B Q4_K_M kullanır. CPU, streaming ve gecikme ölçümleri ile bilinen sınırlamalar [doğrulama raporunda](outputs/reports/VALIDATION.md) yer alır.

## Kurulum ve geliştirme

Uygulamayı kurup başlatın; demo işletme profiliyle SIP, SMS ve WhatsApp kanalları kapalıdır. İşletmeye göre profili ve `config.yaml` dosyasını düzenleyin, demo verilerini gerçek bilgilerle değiştirin ve gerekli kanal sırlarını Windows DPAPI kasasına kaydedin. Ayrıntılar için [uygulama kılavuzuna](outputs/README.md) ve [çevrimdışı kaynak paketinin derleme talimatlarına](outputs/source/BUILD.md) bakın.

Kaynak kod, değişiklik yamaları, sabitlenmiş bağımlılıklar ve bağımsız işletme profilleri depoda bulunur. Tüm modelleri içeren kurulum ve tam araç/arşiv paketi her GitHub Release’inde ayrı dosyalar olarak yayımlanır. SHA256 değerleri [manifestte](outputs/SHA256SUMS.txt), üçüncü taraf lisansları [envanterde](outputs/source/THIRD_PARTY.md) yer alır.

## Ölçülen durum

Yerel kabul kümesinde 14/14 ajan senaryosu, 38 işlem/güvenlik kontrolü geçti. Sıcak Türkçe TTS p95 yaklaşık 76 ms; sentetik yerel SIP/RTP ölçümü p95 yaklaşık 650 ms. Genel LLM’li ses görüşmesinde 1,5 saniye hedefi, toplam 4/8 GB RAM cihazları, İngilizce TTS’nin 250 ms hedefi ve canlı PBX/modem/WhatsApp kanalları henüz doğrulanmadı. İngilizce TTS p95 631 ms ölçüldü. Bu ölçümler için yöntem ve örneklem sınırları raporda açıklanır.

## Lisans

Uygulama GPL-3.0-or-later; SMS/Gammu worker GPL-2.0-or-later. Modeller ve diğer bileşenler kendi lisansları ve bildirimleriyle dağıtılır. Bileşen ve yeniden dağıtım ayrıntıları [THIRD_PARTY.md](outputs/source/THIRD_PARTY.md) içinde listelenmiştir.

---

# Local Business Agent

A Turkish-first, English-capable local voice and messaging customer-service agent for small businesses on Windows x64. AI inference and business data stay on the device. The AI core has no cloud API or telemetry. Download the self-contained offline installer and source bundle from the [latest release](https://github.com/Apollonny/isletme-ajani/releases/latest).

It handles grounded business questions, service comparisons, appointment booking/rescheduling/cancellation, message capture, and staff handoff. SIP and SIM-modem SMS are local integrations. WhatsApp uses a separate official Meta Cloud API gateway and requires internet access and Meta credentials.

See the [Turkish guide](outputs/README.md), [build instructions](outputs/source/BUILD.md), [validation report](outputs/reports/VALIDATION.md), and [third-party license inventory](outputs/source/THIRD_PARTY.md). The application is GPL-3.0-or-later; bundled models and dependencies carry their own licenses.
