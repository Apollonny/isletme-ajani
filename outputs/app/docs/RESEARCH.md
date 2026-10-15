# Depo seçimi — 14 Ekim 2026

Sıralama bu projenin Türkçe/CPU/bellek ihtiyaçlarına göredir. Kaynak sütunundaki model boyutu çalışma RAM’i değildir. Bakım için GitHub API `pushed_at` ve `archived` gözlemi kullanıldı; bu alan bakım kalitesini tek başına kanıtlamaz. Tam commit kayıtları `research-snapshot.json` ve seçilen sürümler `dependencies.lock.json` içindedir.

| Sıra | Depo / rol | Lisans | Son push / bakım | Kaynak tüketimi | Türkçe / karar |
|---|---|---|---|---|---|
| 1 | [canberk7/ema-lightning](https://github.com/canberk7/ema-lightning) / Türkçe TTS | Apache-2.0 | 2026-10-10; açık depo | 8,6M; ONNX 35 MB; TR TTS süreç ölçümü ayrı raporda | Türkçe odaklı; yeni ve küçük bakım ekibi |
| 2 | [ggml-org/whisper.cpp](https://github.com/ggml-org/whisper.cpp) / STT | MIT | 2026-10-12; açık depo | base Q5 ağırlık 60 MB; sınırlı ses bağlamı | Çok dilli base Türkçe içerir |
| 3 | [ggml-org/llama.cpp](https://github.com/ggml-org/llama.cpp) / Yerel LLM | MIT | 2026-10-14; açık depo | Qwen3-1.7B Q4 dosya 1,28 GB; core ölçümü yaklaşık 1,37 GB | Türkçe araç planlama; doğrulanmış yanıt oluşturucu |
| 4 | [snakers4/silero-vad](https://github.com/snakers4/silero-vad) / VAD | MIT | 2026-09-29; açık depo | ONNX 2,3 MB; 512 örnek/32 ms çerçeve | Dil bağımsız |
| 5 | [pjsip/pjproject](https://github.com/pjsip/pjproject) / Telefon/SIP | GPL-2.0-or-later / ticari | 2026-10-14; açık depo | Native C/C++; bir aktif çağrı; video/AEC ağır yollar kapalı | Ses metninin diliyle ilgilenmez |
| 6 | [sqlite/sqlite](https://github.com/sqlite/sqlite) / Hafif RAG | NOASSERTION | 2026-10-13; açık depo | FTS5 + WAL; 4 MB SQLite sayfa ön belleği | Türkçe küçük harf dönüşümü eklenmiştir |
| 7 | [k2-fsa/sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) / İngilizce TTS | Apache-2.0 | 2026-10-14; açık depo | Kokoro int8 ONNX 114 MB + sesler 28 MB | af_heart; TR için EMA tercih edildi |
| 8 | [gammu/gammu](https://github.com/gammu/gammu) / SMS geçidi | GPL-2.0-or-later | 2026-10-13; açık depo | Ayrı native SIM/AT modem worker; Gammu 1.45.0 | UCS-2 Türkçe ve çok parçalı mesaj |
| 9 | [erdemtuna/normalizer-tr](https://github.com/erdemtuna/normalizer-tr) / TR normalizasyon | Apache-2.0 | 2026-10-14; açık depo | Rust native C ABI; bir defa yüklenen sözlük | Tarih/saat/para ipuçları; belirsizlik reddi |
| 10 | [pipecat-ai/pipecat](https://github.com/pipecat-ai/pipecat) / Ses ajanı framework | BSD-2-Clause | 2026-10-14; açık depo | Python ve işlem hattı ek yükü; bu dağıtıma alınmadı | Dil desteği bağlı modellerden gelir |

EMA’nın yayımladığı GPU ilk ses sayısı CPU kabul sonucu olarak kullanılmadı. Yerel ölçüm FP32 ONNX üzerindedir. Kokoro için sherpa ile uyumlu `kokoro-int8-multi-lang-v1_0` export’u kullanıldı; onnx-community grafiğini C API’ye yanlış biçimde bağlamaktan kaçınıldı. Meta Cloud API’ye ait küçük HTTPS geçidi bu uygulamanın kaynağındadır; AI işlem hattına bulut SDK’sı eklenmedi. Kaynak/sürüm seçimi ve dosya SHA256 değerleri kilitlidir.
