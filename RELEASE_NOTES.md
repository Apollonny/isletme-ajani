# İşletme Ajanı 0.1.0

İlk Windows x64 çevrimdışı yayın.

- Yerel Qwen ajanı, SQLite FTS5 tabanlı işletme bilgisi, randevu işlemleri ve çalışan devri.
- Türkçe EMA Lightning neural TTS, İngilizce quantized Kokoro TTS, Whisper.cpp STT, Silero VAD ve SIP/PJSIP hattı.
- Dükkan, klinik ve restoran örnek profilleri; güvenli onay/tekrarlı olay kontrolleri ve JSON-RPC eklentileri.
- `YerelAjan-Setup.exe`: modeller dâhil tek dosyalı çevrimdışı Windows installer.
- `YerelAjan-Source.zip`: kaynaklar, upstream yamaları, lisans bildirimleri, Rust vendor ve sabitlenmiş derleme arşivleri.

Doğrulama: 38 yerel güvenlik/işlem kontrolü geçti; 14 ajan kabul senaryosu geçti. Installer sessiz test kurulumu ve kaldırması geçti, tüm kurulu model hash’leri doğrulandı. Canlı PBX, SIM modem ve WhatsApp testi için gerekli dış altyapı hazır olmadığından bu kanallar test edilmedi. Diğer doğrulanmamış performans hedefleri `outputs/reports/VALIDATION.md` içindedir.

Lisans: uygulama GPL-3.0-or-later. SMS/Gammu worker GPL-2.0-or-later. Modeller ve üçüncü taraf bileşenleri kendi lisansları ve bildirimleriyle gelir.
