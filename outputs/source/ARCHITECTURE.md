# Yerel ajan ve işlem güvenliği

```text
SIP/RTP -> 16 kHz PCM -> Silero VAD -> Whisper base Q5 -> named pipe
SMS SIM/AT -----------------------------------------> named pipe
WhatsApp HTTP.sys HTTPS/HMAC -> durable queue -------> named pipe
                                                     |
          Qwen3-1.7B Q4 tool planner + SQLite FTS5 + business tools
                                                     |
             verified fact/tool result renderer -> text / EMA / Kokoro
```

Core C++17’dir. Tek ürüne ait küçük worker süreçleri kanal ve lisans sınırları için ayrıdır. Core kendi bulut/ağ istemcisini kullanmaz. Yerel IPC remote istemcileri reddeder ve System/aynı Windows hesabıyla sınırlıdır. Gateway yalnız resmî Meta Graph HTTPS adresine gönderir. SIP UDP ve modem iletişim bağlantılarıdır. ONNX telemetrisi kapalıdır; opt-in ölçümler yerel dosyalara yazılır.

Qwen serbest işletme cevabı oluşturamaz: şemalı araç adı/argümanları seçer, renderer yalnız mevcut fact ID’lerini veya gerçek işlem sonuçlarını okur. Hizmet açıklama/süre/fiyatları da profile’dan fact olarak indekslenir. Yetersiz delil, araç limiti veya deadline çalışan kaydına dönüşür. Bilinen açık FAQ ve tek hizmetli rezervasyon akışlarında native durum makinesi gecikmeyi azaltır; karmaşık hizmet seçimi ve randevu değişikliği gerçek LLM ile yürütülür. Nitelikli şube/indirim/sigorta iddiaları ayrıca KB öznesi gerektirir. Bu kontroller ilgisiz bir doğru fact’in her soruya uygun olduğunu matematiksel olarak kanıtlamaz; sınırlı kabul kümesi raporlanır.

Yazma hazırlığı exact tarih/saat/kaynak/ad/grup bilgilerini onaya sunar. `evet/yes` sonrası `BEGIN IMMEDIATE` müsaitliği tekrar denetler. Half-open aralıklarda en yüksek eşzamanlı doluluk hesaplanır. Değişiklik geri alınabilir işlem içindedir; başarısız değişiklik eski rezervasyonu korur. Sahiplik kanal+sender ile doğrulanır. Replay events ve effects ayrı saklanır; müşteri mesajı da effect ile aynı transaction’da kaydedilir. Dış yazma eklentisi başlatılmadan sonuç belirsizliği kaydı tutulur; crash sonrası tekrarlamak yerine çalışan incelemesi gerekir. WhatsApp teslim retry’ları rezervasyonu tekrarlamaz; mesaj tesliminde haricî ağın at-least-once belirsizliği sürebilir.

Modeller CPU’ya sabittir. Qwen mmap, Q4_K_M, 2048 bağlam, Q8 K/F16 V, 128 batch, thinking kapalı; decode bütçesi 14 sn ve tur bütçesi 17 sn. Whisper ses bağlamı utterance uzunluğuna göre sınırlanır; 16 sn üstü utterance bölünür. Ses girişinde VAD/PCM streaming vardır; Whisper nihai metni konuşma segmenti sonunda çözer. Tekrar tekrar tüm Whisper bağlamını çözmek yerine bu CPU tasarımı tercih edildi. Bir çağrı ve metin işlemleri seri core içinde paylaşılan planlayıcı kullanır; yoğun metin işlemi ses talebini bekletebilir. Genel <1,5 sn SLA bu nedenle kanıtlanmış değildir.

EMA üç FP32 ONNX grafiği native işletilir; tam PyTorch runtime yoktur. Normalizer-tr Rust C ABI ve OnceLock kullanır. Tarih/saat ipuçları, telefon digit spelling ve açık etiketli rezervasyon kodu okuma vardır; belirsiz kritik değer reddedilir. İlk decoder penceresi 1 sn, devamı bounded pencereler; 8 frame context ve iptal kontrollü buffer kullanılır. Türkçe sabit fact cevapları sürüm bağlı en fazla 32 MB ses cache’ine girebilir; müşteri randevu verisi cache’e alınmaz. Kokoro int8/sherpa sadece İngilizce gerektiğinde yüklenir; dil değişiminde TR TTS grafikleri bırakılır. 24/48 kHz PCM, kesintiler arasında faz koruyan filtreli oran dönüşümüyle SIP 16 kHz’e çevrilir; PJSIP codec dönüşümünü yapar.

SIP ses motoru çağrı kabulünden önce ısınır; 256 ms pre-roll ve 256 ms sessizlik endpoint’i kullanır. Araya giriş epoch/cancellation değiştirir ve çıkış kuyruğunu temizler. NN medyanın callback’inde çalışmaz. Mikrofon ekranı WinMM kullanır; telefon hattından ayrı yerel operatör denemesidir. Tam çift yön mikrofon/AEC veya gürültülü klinik ortamı kabul testleri yapılmadı.

Profiller basit JSON/Markdown; SQLite WAL ve FTS5 bounded cache kullanır. Çalışma tarih/saatleri Windows yerel saatidir; hedef işletme için İstanbul seçilmelidir. Müşteri adları, mesaj ve rezervasyonlar yerel veritabanındadır. Retention mesaj temizliği başlangıçta yapılır; rezervasyon, events ve effects otomatik silinmez. Konuşma durumları süreç belleğindedir; yeniden başlatma bekleyen onayı düşürür, başarıyla yazılmış rezervasyonlar korunur. DPAPI sırları aynı Windows hesabına bağlıdır. Windows service ve çalışan ekranı aynı hesabı kullanır.
