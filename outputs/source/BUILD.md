# Kaynaktan derleme

Kaynak ZIP’ini yeni bir dizine açın; `outputs/source`, `outputs/app`, `work/downloads` düzenini koruyun. Python 3.11+, Windows x64 ve derleme için yaklaşık 6 GB boş disk gerekir.

```powershell
python outputs/source/build.py --runtime "C:\Users\YOUR_NAME\AppData\Local\YerelAjan"
```

Kaynak paketinde native bağımlılık arşivleri, GPL bileşenlerin kaynakları, Rust crate vendor dizini, CMake dosyaları, tam değiştirilmiş dosyalar ve inceleme için unified diff bulunur. Eksik derleme araçları LLVM-MinGW 20261006, CMake 4.4.4, Ninja 1.13.2 ve Rust 1.99.0 olarak hazırlanır. İndirmeler `dependencies.lock.json` SHA256 değerleriyle doğrulanır. Bu geliştirme hazırlığı ağ kullanabilir; uygulama ve installer ağdan model getirmez.

İnternetsiz derleme için araçları önceden `work/tools`, `work/cargo`, `work/rustup` altında hazırlayın; Rust `x86_64-pc-windows-gnullvm` hedefi kurulu olmalıdır. `--offline` eksik bağımlılıkları indirmez. `--prepare-only --offline` kaynak açma ve patch hazırlığını kontrol eder. Rust bağımlılıkları vendored ve Cargo.lock ile sabittir. `--runtime` modelleri ve örnek profilleri kurulu offline uygulamadan kopyalar. Microsoft runtime’ın kendi kurulum önkoşulu devam eder.

CMake çalıştırma çıktısı `outputs/app` içine yazılır. `ajan-tests.exe` modellerden bağımsız güvenlik testlerini; `--doctor` model bütünlüğünü kontrol eder. Gerçek Qwen testi:

```powershell
python outputs/source/integration_tests.py --app outputs/app --reports outputs/reports --work work/tests
python outputs/source/sip_integration.py --app outputs/app --reports outputs/reports --work work/sip --runs 3
```

`benchmark.ps1` uygulama dizininde çalıştırılır. Installer için NSIS 3.13 ile `outputs/source/installer.nsi` derlenir; payload `outputs/app` ve tüm modeller hazır olmalıdır. Kurulum test modu: `YerelAjan-Setup.exe /S /TEST /D=C:\workspace\install-test`; `/D` son parametre olmalıdır. Test modu kısayol ve kaldırma kaydı yazmaz. Kaldırıcı yalnız listelenen dağıtım dosyalarını siler; veri/config/profiller korunur.

`patches/pjproject-2.17.patch` Schannel/MinGW derlemesini; `patches/Gammu-1.45.0.patch` native serial/dirent derlemesini düzeltir. Bu derlemenin SIP taşımacılığı UDP’dir; Schannel TLS canlı doğrulaması yapılmadı. `patches/files` aynı değişikliklerin tam dosyalarını içerir ve build script tarafından sürümü kilitli kaynak üzerine uygulanır.
