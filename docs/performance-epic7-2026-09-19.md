# Epic Seven — canlı oyun tanısı, 19 Eylül 2026

## Güncel durum — 22:05

Ring-pool adayı kullanıcı tarafından kurulmuş; host ve guest Mesa x86_64
checksum ile doğrulandı. 22:00 CPU kaydında eski liste taraması sıcak noktası
görülmüyor. Ancak ayrı 22:05 kare kaydında p99 122 ms kovasında ve 20 adet
50 ms üzeri aralık sürüyor. Oyun akıcılığı kabul hedefi **karşılanmadı**.
Aşağıdaki ilk tanı bölümleri tarihsel kayıttır; en yeni sonuç dosya sonundadır.

## İlk tanı sırasındaki durum

**Takılma ölçüldü; bu oturumda yeni performans yaması veya ayar değişikliği
uygulanmadı.** Masaüstü akıcılığına ilişkin kullanıcı gözlemi, oyun performansının
kabul edildiği anlamına gelmez. Bu kayıtlar öncesi/sonrası kıyaslaması değildir.

Hedef `com.stove.epic7.google`, sürüm 1.0.974, bildirilen ABI `arm64-v8a`.
Native bridge özelliği `libhoudini.so`. Oyunun süreç haritasını okumak normal
Android shell yetkisiyle reddedildi; hangi fonksiyonda zaman harcandığı henüz
native örnekleme ile belirlenmiş değil. Bu yüzden “Houdini kesin neden” denemez.

Kurulu adayın sekiz bileşen/yapılandırması manifestle eşleşti. Waydroid ve
container çalışıyordu. Renderer servisinin `NRestarts=0` değeri tek başına bütün
istemcilerde hata olmadığı anlamına gelmez; bu oturum için incelenen son renderer
loglarında yeni eşzamanlama/transport hata satırı görülmedi.

## 20 saniyelik Android kare kaydı

Yerel kayıt dizini:
`.work/game-diagnostics/20260919-201536-android/`

Pencere/Android fiziksel boyutu 1880×1170; SurfaceFlinger yenilemesi 165 Hz.
Ekran boyutu, çözünürlük, kalite ve yenileme ayarları değiştirilmedi.
TimeStats'ın bazı satırlarındaki `180 fps` bir **istatistik kovasıdır**, ekranın
180 Hz'e ayarlandığı anlamına gelmez.

| Ölçüm | Sonuç |
|---|---:|
| Oyun BLAST katmanı, kare aralığı örneği | 1.146 |
| TimeStats histogramından ortalama FPS tahmini | 56,146 |
| p50 / p95 histogram dilimi | 18 ms / 18 ms |
| p99 histogram dilimi | 122 ms |
| 50 ms üzerindeki dilimlerdeki aralık sayısı | 20 |
| Tüm host GPU kullanımı | %16–21, ortalama %18,52 |
| Oyunun GLThread ortalama CPU yükü | Bir çekirdeğin yaklaşık %66,2'si |

TimeStats milisaniyeyi kuantize eder ve yüksek sürelerde değişken genişlikte
kovalar kullanır. Bu nedenle p99 tam olarak 122,000 ms diye yorumlanmamalıdır.
`totalTimelineFrames=0` olduğundan jank alanlarındaki sıfırlar **jank yok** kanıtı
olarak kullanılmadı. Bunlar Android tarafındaki sunum kayıtlarıdır; KWin/ekran
üzerindeki gerçek sunum aralıkları değildir. GPU yüzdesi yalnızca oyuna değil,
tüm GPU istemcilerine aittir. Oyunun ön planda kaldığı başlangıç/bitiş dumpsys
kayıtlarıyla kontrol edildi; sahne içeriği görüntü olarak kaydedilmedi.

## Ayrı 10 saniyelik çizim ve zamanlayıcı izi

`atrace.txt`: 1.286.311 yazılan/kayıtta kalan olay; başlıkta taşma görünmüyor.
`gfx`, `view`, `dalvik`, `sched`, `freq` kategorileriyle geçici kayıt alındı.
İz başında `tracing_on=0`, sonunda tekrar `tracing_on=0` doğrulandı.
Bu ek izleme maliyeti olan **ayrı bir tanı kaydıdır**, kabul benchmark'ı değildir.

`analyze-epic7-trace.py` GLThread üzerindeki iç içe B/E işaretlerini ve
`sched_switch`/`sched_wakeup` olaylarını eşleştirdi. İz sınırında başlayan altı
E işareti ve kapanmamış bir dilim yüzdeliklere dahil edilmedi.

| Bölge | Örnek | p50 | p99 | En uzun |
|---|---:|---:|---:|---:|
| `onDrawFrame` | 545 | 7,574 ms | 121,248 ms | 172,642 ms |
| `eglSwapBuffers` | 544 | 8,673 ms | 10,765 ms | 12,657 ms |
| `QueueSubmit` | 544 | 1,304 ms | 2,052 ms | 2,814 ms |
| `AcquireNextImageKHR` | 545 | 0,046 ms | 0,074 ms | 0,158 ms |

- 50 ms'yi aşan **10 adet `onDrawFrame`** yakalandı.
- Başlangıçları yaklaşık **1,004–1,019 saniye** aralıklarla tekrarlanıyor.
- Uzun çağrılar 79,924–172,642 ms; bu çağrıların toplam süresinin **%96,75'i
  CPU'da çalışan durumda** geçiriliyor, çoğu zamanlayıcıda sıra bekleyerek değil.
- Bu çağrıların büyük kısmı ilk izlenmiş görüntü edinme işlemi öncesinde;
  kayıttaki büyük sıçramalar `eglSwapBuffers`/GPU fence beklemesi olarak görünmüyor.

**Çıkarım:** Bu sahnedeki büyük periyodik takılmaların öncelikli araştırma alanı
oyunun CPU çizim yolu. Bu kapsam oyun kodu, ARM çevirisi ve henüz ayrıntılı
izlenmeyen grafik sürücü çağrılarını içerir. Görünür işaretlerin dışında kalan
CPU zamanını oyun, Houdini veya ANGLE arasında kesin paylaştırmak için native
örnekleme gerekir. Bu kayıt, forkta başka darboğaz olmadığı iddiası değildir.

## Sonraki adım ve güvenlik sınırı

Normal Android shell ile `simpleperf stat` denemesi desteklenmeyen/erişilemeyen
olay hatası verdi; ölçüm üretmedi. Oyun paketi debuggable/profileable olarak
bildirilmiyor. Host tarafında `perf` kurulu değil; ajan oturumunda kimliği
doğrulanmış root erişimi yok.

**Stock Android simpleperf root olarak çalıştırılmadı.** Resmi Android 13 kaynak
incelemesi, `CheckPerfEventLimit()` fonksiyonunun root altında global
`perf_event_paranoid` değerine `-1` yazdığını ve `AdjustPerfEventLimit()` yolunun
başka global limitleri de değiştirebildiğini gösterdi. Bu nedenle hazırlanmaya
başlanan root-simpleperf yardımcı aracı dağıtılmadan geri çekildi. Global güvenlik
ayarlarını gevşetmek bu tanının gereği olarak kabul edilmedi.

Sonraki güvenli çalışma: mevcut güvenlik ayarlarını değiştirmeyen host profiler
ile **yalnızca oyunun iş parçacıklarından**, düşük sıklıkta native örnek almak;
game/translator/ANGLE modüllerini ayırmak; ancak ardından yedekli bir A/B adayını
aynı sahnede sınamak. Paket/kernel/sürücü, native bridge, APK veya oyun verileri
bu tanı sırasında değiştirilmedi. Anti-cheat veya oyun korumaları değiştirilmedi.

## Ham kanıtlar

- İlk host kaydı: `.work/game-diagnostics/20260919-201006/`
- Kare/CPU/GPU özeti: `.work/game-diagnostics/20260919-201536-android/summary.json`
- Kare histogramı: aynı dizinde `timestats-after.txt`
- Çizim/CPU zamanları: aynı dizinde `atrace-summary.json`
- Ham iz: aynı dizinde `atrace.txt`
- Yeniden analiz: `.work/game-diagnostics/analyze-epic7-trace.py`

ADB bağlantısı kullanıcı tarafından yetkilendirildikten sonra kullanıldı.
TimeStats başlangıçta boş/devre dışıydı; yalnızca kısa ölçüm için etkinleştirildi
ve sonunda devre dışı bırakıldı. Eski uygulama/shader cache'i veya loglar silinmedi.

## Takip: host perf hazır, yetkili kayıt bekleniyor

Kullanıcı host `perf` paketini kurdu; `/usr/bin/perf` 7.2.6-1 doğrulandı.
Oyun yeniden açıldığından eski PID/TID değerleri kullanılmadı. Güncel kullanıcı
ile `perf stat -e cpu-clock:u` denemesi yetki nedeniyle başarısız oldu. Global
`perf_event_paranoid=2` değeri korunuyor; kullanıcı terminalindeki sudo doğrulaması
ajanın ayrı terminal oturumuna aktarılmıyor.

`dev/profile-game-cpu` bu nedenle **host Linux perf** kullanan, kullanıcı
terminalinden çalıştırılacak bir yardımcı olarak eklendi. Önceki geri çekilen
Android simpleperf aracı değildir. Özellikleri:

- Paket kimliğini ve canlı `GLThread` TID/start-time değerlerini yeniden bulur;
  belirsiz/yeniden başlayan hedefleri reddeder.
- Yalnızca kullanıcı uzayındaki çizim iş parçacıklarından 99 Hz, varsayılan
  15 saniye örnek alır. Sistem geneli kayıt, kernel örnekleri, stack-memory
  yakalama, sysctl/setcap/sudoers değişikliği veya servis restart'ı yoktur.
- Root yetkisini yalnızca kullanıcının kendi terminalindeki sudo doğrulamasıyla
  kullanır. Root profiler ve alt süreçlerinin süresi ayrıca sınırlandırılır.
- Veriyi stdout üzerinden kullanıcının açtığı dosyaya yazar; `.debug` build-id
  önbelleğini güncellemez, harici debuginfod isteği yapmaz. Süreçten yalnızca
  bellek eşleme metadata'sı okunur; oyun verileri veya bellek içeriği kopyalanmaz.
- Örneklerden ham veri, kütüphane/fonksiyon raporları, zaman damgalı örnek listesi
  ve önce/sonra kernel perf limitleri kaydedilir. Boş örnekler başarı sayılmaz.
- Birim testleriyle hedef seçimi, komut sınırları, yetkisiz başlangıç, çıktı
  koruma ve boş örnek reddi kontrol edildi. **Canlı root kayıt henüz alınmadı.**

Kullanıcının kendi terminalinde, oyun sorunlu sahnedeyken:

```sh
sudo -v && ./dev/profile-game-cpu --package com.stove.epic7.google --seconds 15
```

Bu **tanı komutudur**, kurulum veya performans patch'i değildir. İşlem sonunda
çıktı dizini yazdırılır; inceleme aynı proje klasöründen devam edebilir.

## Yeni bulgu ve patch: Venus gönderim kaydı havuzu

Kullanıcının aldığı host kaydı:
`.work/game-diagnostics/20260919-210727-perf/`

- `perf.data`: 1.214 CPU örneği, raporlanan kayıp örnek sıfır. Kayıt başlamadan
  ve bittikten sonra kernel perf limitleri aynı; kurulum/ayar değişikliği yok.
- Birden fazla aynı-adlı GLThread seçilmiş olsa da bu kayıttaki **bütün örnekler
  TID 520003** üzerinde. İsim eşitliği tek başına iş parçacığı rolünü kanıtlamaz.
- Örneklerin 680'i (%56,01) `vulkan.virtio.so`; 670'i (%55,19)
  `vn_ring_submit_locked`. Yaklaşık %35,09 anonim çalıştırılabilir eşlemede;
  anonim örneklerin oyun/Houdini payı sembolsüz kesin ayrıştırılmadı.
- Kaydedilmiş süreç eşlemesi ve aynı checksum'lı önceki binary'nin ELF segmentleri
  kullanılarak adresler eşleştirildi. 670 örnek şu offsetlerde:
  `0xfdc40` (495), `0xfdc4d` (132), `0xfdc51` (37), `0xfdc46` (4), `0xfdc44` (2).
  Bunlar `vn_ring_get_submit` fonksiyonunun derleyicinin içeri aldığı **serbest
  liste arama döngüsüdür**; GPU fence bekleme döngüsü değildir.

Bu sonuç, önceki CPU-onDrawFrame gözlemini tamamlar ve araştırmanın önceliğini
somut bir Mesa/Venus sıcak noktasına çevirir. Önceki atrace ile bu perf aynı anda
alınmadığından, her periyodik spike'ın bütün maliyetini bu fonksiyona atfetmek
henüz mümkün değildir. %55 örnek payı, %55 FPS artışı anlamına gelmez.

### Kök neden

Eski kod kaydın yeniden kullanımına `shmem_count >= istenen` ile karar veriyordu.
Ama `shmem_count` ayrılmış kapasite değil **son gönderimdeki referans sayısı**.
En az iki referanslık alanı olan kayıt sıfır referansla kullanıldıktan sonra
bir-referans isteyen işlem için uygun sayılmıyor. Sıfır/bir referans örüntüsü
kayıt biriktiriyor; sonraki aramalar bütün listeyi tarıyor.

Resmi Mesa aynasındaki `2cf1f6cb5088dcbe0523616169630e2e1f1de574` değişikliği
karşılaştırıldı: bu commit incelenen lineer aramayı getiren eski değişikliktir,
bu görevdeki düzeltme olarak kopyalanmadı.

### Uygulanan kaynak düzeltmesi

`.work/mesa/src/virtio/vulkan/vn_ring.c` değiştirildi ve patch sabit upstream
ağacından `patches/mesa/0003-wip-ahb-memory-steering.patch` içine yeniden üretildi.

- `shmem_capacity` ayrılmış kapasiteyi, `shmem_count` aktif referansları tutuyor.
- 2/4/8/16/32/64/128 kapasiteli yedi ayrı havuz; her sınıfta en fazla 16 kayıt.
- Kayıt alımı ilgili sınıfın başından; oyun süresiyle büyüyen liste araması yok.
- 128'den büyük tek seferlik kayıtlar tamamlanınca serbest bırakılıyor.
- Aktif gönderim listesi, sıra numarası bekleme, fence/protokol ve shmem unref
  sırası korunuyor. Yalnızca **emekli CPU muhasebe kayıtları** havuzlanıyor;
  GPU belleği veya aktif görüntü buffer'ları havuzlanmıyor.
- Mevcut ring mutex'i korunuyor, teardown münhasır erişimle çalışıyor.

### Doğrulama

- ASan/UBSan ve LeakSanitizer: gerçek pool/retirement fonksiyonlarını derleyen
  `tests/test-ring-submit-pool.py` geçti (kapasiteyi yeniden kullanma, büyük
  burst'te üst sınır, in-flight kaydı erken kullanmama, sıra sayacı taşması,
  allocation failure, 1 milyon karma tekrar, dört mutex-korumalı thread).
  İlk sandbox koşusunda LeakSanitizer kısıtı görüldü; test sandbox dışında
  başarıyla tekrarlandı, kısıt nedeniyle başarısız koşu başarılı sayılmadı.
- Android Mesa **x86 ve x86_64** release derlemeleri ve native test ICD'si geçti.
- Yeni native ICD ile **60.000 GPU senkronizasyon döngüsü**, iki istemci, iki queue,
  üç compute tekrarı ve DMA-BUF/mappable buffer testleri geçti; istemci FD'leri
  socket 5→5, diğer senkronizasyon senaryolarında 6→6.
- Geciktirilmiş oluşturma, import/export hatası, device-lost, EOF, socket timeout
  izole testleri geçti; hata gizlenmedi.
- 41 Python testi geçti. Patch serileri pristine pinlere uygulandı; Mesa'daki
  22 değiştirilmiş dosya derleme kaynaklarıyla eşleşti.

Gerçek eski/yeni fonksiyonları derleyen mikrotestte 30.000 sıfır/bir-referans
çiftinin süresi eski kodda 1.029–1.043 ms, yeni kodda 0,47–1,15 ms.
Eski kod 30.001 tahsis/serbest-kayıt biriktirirken yeni kod 1 kaydı tekrar kullandı.
**Bu sadece CPU muhasebe mikrobenchmark'ıdır; oyun FPS/p99 kazancı değildir.**

### Kurulabilir yeni aday

Yeni set: `.work/artifacts/perf-game-ring-pool/`

Önceki set korunuyor. Sekiz payload'dan yalnızca guest Mesa'nın x86 ve x86_64
binary checksum'ları değişti. Uyumlu-set yükleyicisi ve rollback mekanizması
aynı şekilde kullanılıyor. **Yeni patch kurulu sisteme uygulanmadı.**

```sh
sudo -v && ./dev/install-bundle --apply ./.work/artifacts/perf-game-ring-pool
```

Bu komut Waydroid'i yeniden başlatır; oyun ilerlemesi kaydedilmeli. Kurulum
başarısından sonra aynı sahne/ayarlar/benzer oturum yaşıyla yeni TimeStats ve CPU
kaydı alınmalı. Kalıcı kabul, gerçek oyun önce/sonra ölçümü ve kararlılık
kontrolü tamamlanana kadar **bekliyor**.

Ham testler: `.work/game-diagnostics/ring-pool/`; izole GPU çalışması:
`.work/sync-regression-20260919-214132/`.

## Ring-pool kurulum sonrası — 22:00 CPU, 22:05 kare ölçümü

Kullanıcının CPU kaydı: `.work/game-diagnostics/20260919-220044-perf/`.
Her iki host guest-Mesa dosyası yeni adayla byte checksum eşleşiyor; ADB
üzerinden `/vendor/lib64/hw/vulkan.virtio.so` SHA256 da yeni x86_64 adayla
eşleşti: `12048c3e06b57e7979a89d33111bf3e9e008091fe91e762fc3b8ff7b7477f593`.
Bu kontrol canlı dosyayı doğrular; profiler kayıt anında binary hash
kaydetmediğinden geçmişteki mmap içeriğinin bağımsız hash kanıtı değildir.

| Kullanıcı alanı CPU örnekleme | Önce (21:07) | Sonra (22:00) |
|---|---:|---:|
| İstenen süre | 15 s | 15 s |
| Toplam CPU örneği | 1214 | 616 |
| Örnek period toplamından CPU süresi tahmini | 12,263 s | 6,222 s |
| Mesa/Venus örneği | 680 (%56,01) | 2 (%0,32) |
| `vn_ring_submit_locked` örneği | 670 | 0 |
| Anonim çalıştırılabilir bölge örneği | 426 | 442 |
| ANGLE örneği | 37 | 53 |

Yeni kayıtta bütün örnekler TID 544491 üzerinde; kayıp örnek sıfır. Mesa'daki
iki örnek `vn_ring_wait_all` içinde. Sıfır örnek sıfır maliyet kanıtı değildir.
Sahne ve oturum yaşları eşleştirilmiş değil; yaklaşık yarıya düşen CPU süresi
kontrollü FPS/p99 kazancı olarak sunulamaz. Anonim bölgenin yüzdesinin %71,75'e
çıkması tek başına regresyon değildir: mutlak örneği 426'dan 442'ye değişmiştir,
Mesa maliyeti ise örneklerden çıkmıştır. Anonim kodu oyun veya ARM çevirisi
olarak kesin ayrıştıracak sembol/callchain yok; Houdini kesin suçlanamaz.

### Ayrı canlı kare kontrolü

`.work/game-diagnostics/20260919-220515-ring-pool-frames/` altında 20,037 s
TimeStats kaydı alındı. Önceden dolu TimeStats kaydı olmadığı kontrol edildi;
geçici sayaç açıldı ve finally yolunda kapatıldı. Oyun yeniden başlatılmadı,
görüntü ayarları değişmedi. Oyun BLAST katmanı 1106 kare aralığı üretti.
Activity listesinde oyun iki uçta da var; bu tek başına ön plan/aynı sahne
kanıtı değildir. Fiziksel masaüstü sunumu ölçülmedi.

| Android TimeStats | Önce (20:15) | Sonra (22:05) |
|---|---:|---:|
| Kare aralığı sayısı | 1146 | 1106 |
| Raporlanan/türetilen ortalama FPS | 56,146 | 55,637 |
| p50 / p95 kovası | 18 / 18 ms | 18 / 18 ms |
| p99 kovası | 122 ms | 122 ms |
| 50 ms üzerindeki kovalarda aralık sayısı | 20 | 20 |

Histogram kovaları yüksek sürelerde değişken genişliktedir. Farklı anlarda,
eşleştirilmemiş sahnelerde alınan bu kayıtlar kontrollü A/B veya regresyon
kararı değildir; ancak büyük takılmaların hâlâ mevcut olduğunu gösterir.
**Liste taraması israfı giderilmiş görünüyor, oyun takılma hedefi karşılanmadı.**

Sonraki tanı, aynı zaman tabanında çizim dilimleri ve düşük sıklıklı CPU
örneklerini eşleştirip yalnızca uzun karelerdeki kodu ayırmalıdır. Tüm kayıt
ortalamasına bakarak yeni bir senkronizasyon patch'i, cache temizliği, native
bridge değişikliği veya oyun korumasına müdahale gerekçelendirilmez. Bu
turda ek performans patch'i veya kurulum yapılmadı.

Makine tarafından okunabilir CPU karşılaştırması:
`.work/game-diagnostics/20260919-220044-perf/comparison.json`; kare sonuçları:
`.work/game-diagnostics/20260919-220515-ring-pool-frames/summary.json`.
