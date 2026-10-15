# Ölçüm ve kabul durumu

14 Ekim 2026; AMD Ryzen 5 7500F, 6 fiziksel / 12 mantıksal çekirdek, 31,61 GiB RAM, Windows x64. CPU inference; GPU kullanılmadı. Bu makine bir 4/8 GB toplam RAM cihazı değildir.

| Kontrol | Sonuç | Kapsam |
|---|---|---|
| İşlem/kanıt/eklenti/PCM güvenliği | 38/38 geçti | Gerçek SQLite, native eklenti süreci, bağımsız DSP oran testleri |
| Yerel müşteri senaryoları | 14/14 geçti | Gerçek Qwen + native akış; yapay müşteri verisi; 3 işletme |
| Türkçe TTS ilk PCM, hazır | p50 63.9; p95 76.0; p99/maks 84.2 ms | 8 ayrı metin, 6 tekrar; her süreçte ilk tekrar hariç |
| Türkçe TTS süreç tepe RAM | 125 MB | Working set; tek TTS süreci |
| İngilizce TTS ilk PCM, hazır | p50 363.8; p95/maks 631.2 ms | 3 metin, 6 tekrar; 250 ms hedefi geçmedi |
| İngilizce TTS süreç tepe RAM | 345 MB | TR grafikleri bırakılmış, Kokoro/sherpa lazy load |
| Gerçek model metin turu | p50 3.0; p95 2803.8; maks 11495.2 ms | LLM ve native senaryolar birlikte; başlangıç ilk tura dahildir |
| Core tepe RAM | 1368.1 MB | Gerçek model testinde 20 ms örnekleme |
| Yerel SIP/RTP ilk anlamlı ses | p50 609.8; p95/maks 649.8 ms | 3 çağrı; çalışma saatleri FAQ; cevap “Çalışma saatlerimiz…” |
| LLM bellekte + SIP ses motoru | 1575.4 MB toplam | Gerçek LLM metin turundan sonra SIP, core ve probe aynı anda bellekte; tek örnek |
| Aynı bellek senaryosunda ilk ses | 669.8 ms | Yoğun eşzamanlı LLM decode yapılmadı |
| Neural TTS iptal | geçti | İlk chunk’ta cancellation; ikinci chunk ve önceden iptal edilmiş iş üretilmedi |
| Gateway HMAC / SMS Unicode | geçti | RFC HMAC örneği, değiştirilmiş imza reddi, Türkçe UCS-2 dönüşümü |

TTS ham raporları `benchmark.json` ve `tts-metrics.csv` içinde normalizasyon, text graph, acoustic graph, decoder, RTF, CPU süreleri, warm/cold ilk ses ve RAM içerir. Türkçe cold başlangıç model yükleme + ilk PCM yaklaşık 1,21–1,25 sn; bu süre 76 ms warm değeriyle karıştırılmamalıdır. SIP motoru çağrı kabulünden önce ısıtılır. Örnekleme WorkingSet ölçer; ek TTS bellek ve toplam sistem RAM’i aynı kavram değildir.

SIP ölçümü sentetik giriş dosyasının son ±500 amplitude üstü örneğinden kayıtlı cevabın ilk ±500 üstü örneğine hesaplanır. RTP hizalaması yaklaşık ±40 ms belirsizlik ekler. Kayıt gerçek PJSIP UDP/RTP taşımacılığıdır, dış PBX/PSTN değildir. EMA’nın 4 ms GPU yayını kabul ölçümü olarak kullanılmadı.

## Önce / sonra

Whisper tam ses bağlamı / sınırlı bağlam ve native ses hattı ölçümü: baseline p95 1287.5 ms, son optimize hat p95 649.8 ms. Fark yaklaşık %49.5. Baseline 2, optimize 3 örnektir; küçük örneklem nedensel kalite veya bütün donanımlar için sonuç kanıtı değildir. Kaynakta bounded context, pre-roll, çağrı öncesi warmup, sürüm bağlı cache ve faz koruyan resampler birlikte bulunmaktadır. `sip-before-fixes.json` daha önce yakalanan başarısız regresyonu korur; son kabul sonucu değildir.

Kokoro thread kalibrasyonu `english-tuning.json` içindedir: aynı appointment metninde 1 thread warm yaklaşık 508–511 ms; 4 thread yaklaşık 195–262 ms. Ayrılmış son 3 metinlik kümede p95 631.2 ms bulundu. Küçük kalibrasyon sonucunu daha geniş test başarısı olarak sunmuyoruz.

Native geliştirmeler yalnız upstream çağrı sarmalama değildir: üç EMA graph’ının native duration/latent/decoder penceresi yönetimi, Türkçe C ABI normalizasyon ve telefon/kod tipleri, gerçek rezervasyon işlemleri, max-concurrent restoran kapasitesi, deadline/KV-prefix yeniden kullanımı, IPC ve JSON-RPC worker sınırları, PCM resampler ve cancellation akışları yeni kaynak kodundadır. PJSIP/Gammu MinGW yamaları ayrı diff olarak sağlanır. Whisper exposed audio-context seçeneği adapter’da kullanılır; upstream Whisper çekirdeğini değiştirdik iddiası yoktur.

## Tamamlanmamış doğrulamalar

- Genel LLM sesli görevlerinde <1,5 sn ve yoğun eşzamanlı çağrı+metin SLA’sı doğrulanmadı; Qwen metin turu ses talebini bekletebilir.
- İngilizce TTS 250 ms/250 MB hedefini geçmedi. Lazy loading ve 4 thread seçimi kaynak maliyetini azaltır; bu başarısız hedef raporda saklanır.
- 4 GB ve 8 GB toplam RAM Windows cihaz testleri yapılmadı. 1,58 GB süreç toplamı bu cihazlarda aynı gecikmeyi kanıtlamaz.
- Gerçek modem/SIM, Meta hesap/numara/HTTPS ve dış PBX kullanıcıda hazır değil; canlı kanal testleri yapılmadı. SMS local transport simülasyonu fiziksel modem değildir.
- Ayrılmış Türkçe ad/tarih/saat/para/telefon/kod kümesi ses üretti. İnsan MOS, bağımsız kelime atlama/tekrarlama ve sayı doğruluğu etiketlemesi tamamlanmadı; dinleme WAV’ları sağlanır. STT base Q5 gerçek gürültülü konuşmalarda ek veriyle değerlendirilmelidir.
- 14/14 sonucu küçük yerel kabul kümesidir; üretim taleplerinin tamamında %95 başarı iddiası değildir.

Installer kurulum/kaldırma testi ve kaynak hazırlama sonucu ayrıca `packaging.json` içinde kaydedilir. Installer geliştirme dağıtımı imzasızdır; Microsoft runtime önkoşulu resmî imzalı pakettir. Dosya bütünlüğü `SHA256SUMS.txt` ile doğrulanabilir.

## Paketleme doğrulaması — 15.10.2026

- NSIS 3.13 derlemesi çıkış kodu 0 ile tamamlandı; paket 1.504.978.472 bayt ve 1.663.253.361 bayt uygulama verisi içeriyor. Installer iç CRC değeri `6471086A`.
- Sessiz `/TEST` kurulumu 439 dosyayı çıkardı. Kurulu uygulamanın `--doctor` denetimi bütün model hash’lerini doğruladı; `ajan-tests.exe` 38 güvenlik/işlem kontrolünü geçti.
- Kaldırıcı çıkış kodu 0. Uygulama dosyalarını kaldırdı; config, altı örnek profil dosyası ve `data` dizini kaldı.
- Kaynak ZIP’i 481 girdi ve 445.295.031 açılmış bayt içeriyor; her girdi okunarak CRC denetiminden geçirildi. Çevrimdışı kaynak hazırlama ve vendored Rust release build kontrolleri geçti.
- Installer ve kaynak arşivinin SHA256 değerleri `../SHA256SUMS.txt` ve `packaging.json` içinde bulunur.

