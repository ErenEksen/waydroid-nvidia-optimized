# Waydroid NVIDIA — 19 Eylül 2026 doğrulama raporu

## Sonuç

**Kaynak düzeltmeleri, derlemeler ve izole GPU regresyon testleri hazır.
Canlı Android performans kabulü tamamlanmış değildir.** Kurulu sisteme bu aday
kurulmadı. Dolayısıyla %30 p99 / %50 takılma azalması sağlandığı iddia edilmiyor.

Çalışma dalı: `codex/perf-smoothness`. Orijinal indirilen kopya korunmuştur.
Çalışma klasöründeki değişiklikler incelemeye açık; otomatik push yapılmadı.

Gözlenen sistem: RTX 5080 Laptop GPU, NVIDIA 615.71.09, KWin Wayland;
ölçüm istemcisi eDP-1 üzerinde 1920×1200 ve yaklaşık 165 Hz bildirdi.
Kurulu `wd-venus.service` son kontrolde hâlâ **PID 1379**, `NRestarts=0` ile
çalışıyordu; Waydroid oturumu **STOPPED** idi. Servis/bileşen kurulumu, uygulama
verisi/cache silme, sürücü/kernel güncellemesi veya görüntü/animasyon ayarı değişikliği
bu çalışmada yapılmadı.

## Önce / sonra kabul durumu

| Senaryo / ölçüt | Önce | Sonra | Kabul durumu |
|---|---|---|---|
| 5 oturum-soğuk başlangıcı | Ölçülmedi | Ölçülmedi | Eksik |
| Ayarlar: 10 işlem-soğuk açılış | Ölçülmedi | Ölçülmedi | Eksik |
| Ayarlar/Launcher sıcak animasyonları | Ölçülmedi | Ölçülmedi | Eksik |
| İlk Android sunumu / p50-p95-p99 / >50 ms | Ölçülmedi | Ölçülmedi | Eksik |
| Eşzamanlı gerçek masaüstü sunumu | Eşleştirilmiş ölçüm yok | Eşleştirilmiş ölçüm yok | Eksik |
| GLES/Vulkan throughput değişimi | Eşleştirilmiş ölçüm yok | Eşleştirilmiş ölçüm yok | Eksik |
| 30 dakika kullanım / FD-syncobj-RSS kararlılığı | Yapılmadı | Yapılmadı | Eksik |

Neden: kimlik doğrulanmış `sudo` erişimi yoktu; Android oturumu çalışmıyordu.
Root gerektirmeyen, kurulu servisten bağımsız GPU testleri bunun yerine
çalıştırıldı. Bu testler doğruluk kanıtı sağlar; gerçek uygulama açılışının veya
masaüstü etkisinin yerine geçmez.

## Doğrulanmış düzeltmeler

- Semaphore socket import'unun ring üzerindeki oluşturma/önceki bekleme komutlarını
  geçmesini engelleyen, gönderim başına bir defalık gerçek ring tüketim bariyeri.
- SYNC_FD export sonrası resetlenen VkFence üzerinde tekrar beklememe: payload bir
  kez dışa aktarılır, saklanan fd beklenir ve dış tüketicilere kopyalanır.
- Zaten sinyallenmiş `-1` fd'nin `poll()` içine girerek sonsuz beklemesi düzeltildi.
  Devralınan socket testi 82–325. döngü civarlarında takılabiliyordu; düzeltmeden
  sonra uzun socket testleri tamamlandı. Bu **doğruluk karşılaştırmasıdır**,
  uygulama FPS/p99 iyileşmesi değildir.
- Export/import hataları başarıya çevrilmiyor; device/semaphore aidiyeti ve
  yaşam süresi, yanıt gönderme hatasında fd'nin iki kez kapatılması, socket EOF
  ve yanıt vermeyen socket beklemeleri ele alındı.
- Bağımsız queue'lar tek timeline üzerinde birbirlerinin işini bitmiş sayamaz:
  ek queue'lar mevcut socket yolunu kullanır.
- GPU format/modifier metadata sorgu cache'i ve değişmeyen HWC opacity/region
  durumunun tekrar kullanımı eklendi. Canlı buffer bellekleri havuzlanmadı.
- Renderer'a özel cache dizininde Vulkan iş yükünün dosya oluşturduğu ve yeni
  istemcinin bu dosyalara `IN_ACCESS` olayı ürettiği görüldü. Bu, Android
  HWUI/Skia/ANGLE cache'lerinin incelenmesinin tamamlandığı anlamına gelmez.

## Geçen testler

| Test | Sonuç | Kapsam / sınır |
|---|---|---|
| Mesa Android x86 + x86_64 release derleme | Geçti | ELF32/EM_386 ve ELF64/EM_X86_64 doğrulandı; Android üzerinde yürütülmedi |
| Host renderer + HWC + gralloc derleme | Geçti | HWC alpha/resize/fallback canlı doğrulaması bekliyor |
| Timeline, socket, CPU fallback | Her biri 10.000 döngü | Native Venus ICD → izole, paketlenmiş host renderer |
| İki eşzamanlı istemci | 2 × 10.000 döngü | Kurulu renderer kullanılmadı |
| İki queue | 10.000 döngü | Tek istemcide iki queue arasında dönüşümlü gönderim |
| İstemci FD sayıları | Artış yok | Timeline/CPU/concurrent/multiqueue 6→6, socket 5→5; host/guest uzun kullanım sızıntı kanıtı değildir |
| Vulkan compute | 65.536 eleman doğru | Ayrı istemcilerle tekrarlandı; throughput karşılaştırması değil |
| DMA-BUF → NVIDIA EGL texture | Geçti | `GL_TEXTURE_2D` bind ve mappable buffer write/read |
| Geciktirilmiş semaphore oluşturma | Geçti | Test-only Vulkan loader shim |
| Gerçek-fd import hatası | Güvenli fallback geçti | Hatalı import fd'yi tüketmedi; normal signaled import korunuyor |
| Fence export hatası / device-lost | Açık hata ile sonlandı | Başarı veya tamamlanmış GPU işi olarak raporlanmadı |
| Socket EOF / yanıt vermeyen peer | Sınırlı sürede hata | EOF hızlı sonlandı; yanıt vermeyen peer yaklaşık 30 s sınırında sonlandı |
| Python regresyonları | 26 test geçti | Metrikler, eksik veri, karşılaştırma, ABI/checksum, dosya yedeği/geri yükleme |
| Patch yeniden uygulama | Geçti | Üç upstream pin; dokunulan 21 Mesa, 20 renderer ve 10 HWC dosyası derleme kaynaklarıyla birebir aynı |
| Paket manifesti / RUNPATH / ABI | Geçti | Paketlenmiş binary seti üzerinde GPU matrisi yeniden çalıştırıldı |

İzole socket testlerinin ilk başarısız logları da `.work/` altında tutuldu;
sonuçlar yalnızca başarılı denemeler seçilerek sunulmuyor.

## Masaüstü ölçer doğrulaması

Küçük Wayland probe `wp_presentation` zaman damgalarını, discard sayılarını,
clock/output kimliğini ve yenileme periyodunu alabildi. Ayrı bir kısa smoke kaydı
872 sunum, 114 discard ve p99 yaklaşık 12,359 ms içeriyordu. Bu kayıt **kontrollü
önce baseline'ı değildir**; geliştirme/test yükü altında alındı. Android kareleri
ve Android'e bağlı masaüstü regresyonu olarak yorumlanamaz. KWin sequence değeri
sıfır olduğundan kaçırılan yenileme oranı açıkça `timestamp_estimate` etiketlidir.

Android `DisplayPresentTime` yoksa, kısmi ise, saatler uyuşmazsa, süreç sıcak
senaryoda değişirse veya veri kaybı ihtimali varsa benchmark başarısız/eksik döner.
`am start -W` veya `FrameCompleted`, fiziksel sunum diye yeniden adlandırılmaz.
Ayrı host probe, Android yüzeyinin ilk fiziksel görünür karesini tek başına
belirleyemez; gerekirse HWC/Perfetto korelasyonu ayrıca yapılmalıdır.

## Teslimatlar

- Aday bileşen seti: `.work/artifacts/perf-smoothness-final/`
- Arşiv: `.work/artifacts/perf-smoothness-final.tar.gz`
- SHA256: `.work/artifacts/perf-smoothness-final.tar.gz.sha256`
- Ham doğrulama özetleri: `.work/artifacts/validation/`
- Makinece okunur durum: `.work/artifacts/validation/report.json`
- Kurulum kontrolü: `dev/install-bundle --check <bundle>`
- Kurulum/geri dönüş: `docs/dev-workflow.md`
- Ölçüm koşulları ve kabul kapıları: `docs/performance-smoothness.md`

Arşiv SHA256:
`8e8052a0adee1c18d5bc769691dfb8f6e2a4be763e4ed3c5e4bc3395ec9aeb9b`

## Sonraki zorunlu adım

Kullanıcının kendi terminalinde `sudo -v` ile yetkilendirmesinden sonra **önce
kurulu setin baseline'ı** alınmalı. Ardından checksum'lı uyumlu set, yedekli
kurulum aracıyla kurulup aynı matris tekrar çalıştırılmalı. Sağlık hatasında
eski set geri alınmalı; yüzde hedefleri karşılanmazsa kalan darboğaz raporlanmalı.

Tam Android/LineageOS derlemesi, guest cache mekanizmalarında henüz kanıtlanmamış
izin değişiklikleri ve A/B kazancı gösterilmemiş scratch-syncobj havuzu bu adayda
yoktur. Kaynak düzeltmeleri “kesin seamless performans” garantisi olarak sunulmaz.

## Kurulum takip düzeltmesi — 19 Eylül 2026

Kullanıcının ilk kurulum denemesinde `ModuleNotFoundError: No module named
'dbus'` oluştu. Hata yeniden üretildi: sistem Python'ı `dbus.mainloop.glib`
modülünü yükleyebilirken ortak `dev/env.sh` derleme venv'ini `PATH` başına
alıyordu; `/usr/bin/waydroid` içindeki `env python3` yanlış Python'ı seçiyordu.
Hata ilk `session stop` komutunun import aşamasındaydı, dosya kurulumuna henüz
ulaşılmamıştı.

- Venv seçimi yalnızca `dev/build` içine taşındı; ortak çalışma ortamı `PATH`'i
  değiştirmiyor. Doğrudan provisioning örneğinde PATH komuta özel veriliyor.
- `--apply` ve `--rollback`, sudo/servis işlemlerinden önce gerçek Waydroid
  komutunu `--help` ile, 15 saniye sınırıyla doğruluyor. `--check` çevrimdışı
  bundle bütünlük kontrolü olarak kaldı.
- Yedi yeni regresyon testi eklendi: paket/checkout Python seçimi, ortak PATH,
  derleme araçlarının seçimi, başarısız/başarılı preflight sırası ve çevrimdışı
  kontrol. Toplam **33 Python testi geçti**. Testlerde sudo ve systemctl sahtedir;
  gerçek servisler değiştirilmedi.
- Gerçek sistemde `dbus` import'u ve `dev/wdu --help` tekrar başarılı oldu;
  aynı aday setinin checksum/ABI kontrolü geçti.

Native binary'ler, aday arşivi ve önceki GPU sonuçları değiştirilmedi. Arşiv
manifestinin `source_files` alanı paketleme anındaki araç kaynaklarını gösterir;
bu sonraki kurulum aracı düzeltmesini içermez. Önceki `report.json` tarihsel
rapor olarak korundu. Bu takip testi canlı root kurulumunu veya Android
performans kabulünü tamamlanmış saymaz.
