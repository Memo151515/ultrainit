<p align="center">
  <img src="assets/logo.svg" alt="UltraInit logo" width="700">
</p>

# UltraInit 1.2

Hafif, C ile yazılmış, **deklaratif servis tanımlı** bir init sistemi.
systemd'nin faydalı kısımlarını (bağımlılık grafiği, denetim, cgroup, birim
dosyaları, kontrol soketi) alır; D-Bus, journald, socket activation gibi
ağır parçaları almaz. İkili dosyalar toplam ~100 KB, harici kütüphane yok.

```
ult-service list                  # servisler, durumları, etkin mi
ult-service sshd start|stop|restart|status
ult-service sshd enable|disable   # runlevel'e ekle/çıkar
ult-service reload                # runlevel + yapılandırmayı canlı yeniden oku
ult-service poweroff|reboot|halt
journalult -u sshd -n 50          # merkezi log (journalctl karşılığı)
```

## Mimari

PID 1 olan `ultrainit` bir **reconciler**'dır: her servis için *hedef durum*
(çalışmalı / durmalı) tutar ve gerçek durumu bu hedefe sürekli eşitler.
Dalga yok, olay tabanlı çalışır:

- `start X` → X ve `need`/`want` bağımlılıkları otomatik çekilir, bağımlılık
  sırasına göre, `max_parallel` sınırı içinde paralel başlar.
- `stop X` → X'e `need` ile bağlı olan her şey **önce** durur, sonra X.
- Kapanış → tüm hedefler "durmalı" olur; sonuç otomatik olarak başlatmanın
  tam tersi sıradır.
- Bağımlılık döngüsü varsa loglanır ve o birimler arasındaki sıralama
  yoksayılır (sistem kilitlenmez).

### Servis durumları

`inactive → starting → running | done | failed → stopping → inactive`
(+ `restarting`: yeniden başlatma bekliyor).

## Yerel birimler (`/etc/ultrainit/services/<ad>.svc`)

systemd benzeri, ama tek bölümlü, düz `anahtar=değer` dosyası. Hazır örnek:
`services/example.svc.sample`.

```ini
description=Ornek servis
exec=/usr/bin/ornek-daemon --foreground
type=simple               # simple | forking | oneshot
need=localmount           # zorunlu bağımlılık (otomatik başlatılır)
want=syslog               # isteğe bağlı bağımlılık
after=udev                # sadece sıralama
before=sddm
restart=on-failure        # no | on-failure | always
log=file                  # file | console | null
```

| Anahtar | Anlamı |
|---|---|
| `exec` | Başlatma komutu. **Mutlak yol** olmalı; `"..."` ve `'...'` desteklenir. Shell yoktur, doğrudan `execve` edilir. |
| `stop_exec` | İsteğe bağlı özel durdurma komutu (sonra yine TERM → KILL). |
| `type` | `simple`: `exec` servisin kendisi · `forking`: daemonlaşıp çıkar, `pidfile=` ile PID okunur · `oneshot`: bir kez çalışıp biter (`done`). |
| `need` / `want` / `after` / `before` / `conflicts` / `provides` | Bağımlılıklar. `need` başarısızsa bu birim de `failed` olur. `provides` takma ad tanımlar. |
| `restart`, `restart_sec` | Çökünce yeniden başlatma politikası. Hız sınırı: `respawn_max` / `respawn_window`. |
| `timeout_start`, `timeout_stop` | Saniye. Durdurmada süre dolunca `SIGKILL`. |
| `user`, `group`, `workdir`, `umask`, `env` | Çalışma ortamı (`env=` tekrarlanabilir). Varsayılan `umask` 022. |
| `pidfile` | `type=forking` için. |
| `ready_path` | Yalnızca `type=simple`. Bu yol (ör. soket) oluşana kadar servis `starting` sayılır; bağımlılar ancak hazır olunca başlar. |
| `log` | `file` (varsayılan: `/var/log/ultrainit/<ad>.log`) · `journal` (merkezi log, aşağıya bakın) · `console` · `null`. |

**Şablonlar:** `getty@.svc` gibi `ad@.svc` dosyaları, `ad@ornek` adıyla
örneklenir; içinde `%i` (örnek) ve `%n` (tam ad) kullanılabilir.
`getty_ttys=tty1,tty2` ayarından `getty@tty1`, `getty@tty2` otomatik oluşur.

Birim dosyaları **root'a ait** olmalı, grup/diğer yazamamalı, symlink
olmamalı (bkz. Güvenlik).

## Eski tip `init.d` betikleri (geriye uyumlu)

OpenRC tarzı `start()/stop()/status()/depend()` betikleri çalışmaya devam eder.
Farkı: `depend()` bloğu artık betik **çalıştırılmadan** okunur
(`need`, `want`, `use`, `after`, `before`, `provide`, `conflict`), yani boot
sırasında servis başına ek shell çalışmaz. Betik `ULT_PIDFILE` ortam
değişkenine (ya da `/run/ultrainit/<ad>.pid`) bir PID yazarsa o süreç
denetime alınır ve çökerse yeniden başlatılır.

## cgroup v2

`/sys/fs/cgroup` üzerinde cgroup2 varsa her servis kendi cgroup'una
(`/sys/fs/cgroup/ultrainit/<ad>`) alınır. Durdurmada `cgroup.kill` ile,
process grubundan **kaçmış çocuklar dahil**, servisin tüm süreçleri
temizlenir. cgroup yoksa process grubu sinyali ile devam eder.

## Merkezi log: `ult-journald` + `journalult`

systemd'nin journald'inin hafif karşılığı. Üç kaynağı tek, zaman damgalı,
öncelikli bir kayıt akışında toplar:

| Kaynak | Nasıl |
|---|---|
| Servis çıktısı | Birimde `log=journal`. UltraInit servisin stdout/stderr'ini doğrudan journald'e bağlar (stdout = `info`, stderr = `err`). PID, çekirdekten (`SO_PEERCRED`) alınır. |
| syslog | `/dev/log` (`logger`, `syslog(3)`); öncelik, etiket ve PID ayrıştırılır. |
| Çekirdek | `/dev/kmsg`; journald yeniden başlayınca aynı mesajlar tekrar kaydedilmez. |

```sh
ult-service journald enable && ult-service reload   # bir kez
```

`log=journal` kullanan her birim, journald'i otomatik `want` + `after` yapar
ve journald **hazır olmadan** (`ready_path`) başlamaz, yani ilk satırdan
itibaren yakalanır. journald bir sebeple yoksa servis çıktısı sessizce
`/var/log/ultrainit/<ad>.log` dosyasına düşer.

Kayıtlar düz metindir (`/var/log/ultrainit/journal/journal.log`, dönen
dosyalar; `epoch_ms ⇥ öncelik ⇥ birim ⇥ pid ⇥ mesaj`). Kontrol karakterleri
`?` ile değiştirilir (terminal kaçış dizisi enjeksiyonu olmaz).

```sh
journalult                       # son 100 kayıt
journalult -u sshd -n 50         # bir birim (-u 'getty@*' ön ek)
journalult -u ultrainit          # init'in kendi logu
journalult -p err                # err ve daha ciddi (emerg..debug ya da 0-7)
journalult -k                    # yalnızca çekirdek
journalult --since 10m -g hata   # son 10 dk, mesajda arama
journalult -f                    # canlı takip
journalult -o json               # json | cat | short
journalult --list-units | --disk-usage
```

`ult-service <servis> status`, servisin son 5 log satırını da gösterir.

**Sınırlamalar (dürüst liste):** log dosyaları yalnızca root'a açıktır
(`0640`; grup tabanlı okuma yok) · syslog'daki **birim adı istemcinin
iddiasıdır** (PID çekirdekten doğrulanır, etiket doğrulanamaz) · hız
sınırlaması yok: gürültülü bir servis rotasyonla eski kayıtları
sıkıştırabilir · journald çökerse `log=journal` servislerinin bağlantısı
kopar ve yazdıklarında `SIGPIPE` alabilirler (journald `restart=always` ile
hemen kalkar, ama o servislerin yeniden başlatılması gerekir).

## Boot, kapanış ve sinyaller

Gerçek PID 1 olarak `ultrainit` şunları yapar: kök dizini rw yapar, `/proc`,
`/sys`, `/dev`, `/dev/pts`, `/dev/shm`, `/run` ve cgroup2'yi (zaten bağlı
değilse) bağlar, konsolu hazırlar, hostname'i ve `lo` arayüzünü ayarlar,
sonra runlevel'i yükler. (initramfs bunları zaten yaptıysa dokunmaz.)

| Sinyal | Etki |
|---|---|
| `SIGTERM`, `SIGINT` (Ctrl-Alt-Del) | reboot |
| `SIGUSR1` | halt |
| `SIGUSR2`, `SIGPWR` | poweroff |
| `SIGHUP` | `reload` |

Kapanışta servisler ters bağımlılık sırasıyla durur; sonra kalan süreçlere
`SIGTERM` → `SIGKILL`, `swapoff`, dosya sistemlerini unmount, kökü salt-okunur
yapma ve `reboot()` çağrısı yapılır. `shutdown_timeout` aşılırsa kalanlar
zorla sonlandırılır.

## Runlevel ve `reload`

`ult-service X enable` yalnızca `runlevels/default/X` bağlantısını oluşturur.
`ult-service reload` (ya da `SIGHUP`) değişikliği canlı uygular:
**yeni etkinleştirilenleri başlatır, çıkarılanları durdurur**; çalışan
servislerin durumuna ve **elle durdurulmuş servislerin hedefine dokunmaz**.

## Kontrol soketi

`/run/ultrainit/control` (UNIX soket, mod 0600, `SO_PEERCRED` ile yalnızca
root). Satır tabanlı: `start|stop|restart|status <ad>`, `dump`, `reload`,
`poweroff|reboot|halt`, `ping`. `start/stop/restart` servis yerleşene kadar
cevap vermez (`OK <durum>` / `ERR <durum> <neden>`), yani `ult-service`
senkron çalışır. Init çalışmıyorsa (chroot/kurulum) `ult-service`, eski tip
betikleri bağımlılık çözümü olmadan doğrudan çalıştırabilir.

## Yapılandırma (`/etc/ultrainit/ultrainit.conf`)

| Anahtar | Varsayılan | |
|---|---|---|
| `default_runlevel` | `default` | |
| `max_parallel` | 16 | aynı anda "starting" olabilecek en fazla servis (1-64) |
| `respawn_window` / `respawn_max` | 30 / 5 | pencere içinde bu kadar çökmeden sonra `failed` |
| `shutdown_timeout` | 30 | kapanış için toplam bekleme (sn) |
| `getty_ttys` | `tty1` | `getty@.svc` şablonundan örneklenecek TTY'ler |
| `journal_max_file_kb` / `journal_max_files` | 4096 / 8 | log dosyası boyutu (64-1048576 KiB) ve tutulacak dosya sayısı (2-64) |

## Kurulum

```sh
make                       # ultrainit + ult-service
sudo make install          # /usr/sbin, /usr/bin, /etc/ultrainit
sudo make install PREFIX=/usr/local
```

`make install` mevcut `ultrainit.conf`'un üzerine yazmaz
(`ultrainit.conf.orig-1.1.0` bırakır). PID 1 olarak denemek için kernel
komut satırına `init=/usr/sbin/ultrainit` ekleyin — **önce sanal makinede
(QEMU) deneyin.**

Dizinler: `/etc/ultrainit/{ultrainit.conf, services/, init.d/, runlevels/}`,
log: `/var/log/ultrainit.log`, servis logları: `/var/log/ultrainit/<ad>.log`.

## Güvenlik modeli

1. Birim/betik/ana config: **root'a ait**, grup/diğer **yazamaz**, **normal
   dosya**. Doğrulama dosya açıldıktan sonra aynı fd üzerinden (`O_NOFOLLOW` +
   `fstat`) yapılır; ayrıca `/`'ya kadar **tüm üst dizinler** root'a ait ve
   grup/diğer-yazılamaz olmalıdır (root'a ait sticky dizinler hariç). Böylece
   kontrol–kullanım arası yarış (TOCTOU) pratikte kapanır.
2. Servis adları `[A-Za-z0-9_.@:-]` ile sınırlıdır; `/`, tırnak, `..` reddedilir.
3. Ortam her exec öncesi temizlenir (minimal `PATH`, `HOME`, `SHELL`).
4. Kontrol soketi yalnızca root'a açıktır.
5. Çökme döngüsü hız sınırıyla durdurulur.
6. Log dosyası `O_NOFOLLOW` ile açılır.

## Test

```sh
make test                      # 73 test: gerçek süreçler, yalıtılmış kök (root gerekir: sudo make test)
ULT_SKIP_PID1=1 make test      # unshare gerektiren testleri atla
CFLAGS_EXTRA="-fsanitize=address,undefined" ULT_SKIP_PID1=1 sh tests/run-tests.sh
```

Kapsam: bağımlılık sırası, paralellik, respawn ve hız sınırı, bağımlılık
başarısızlığı, ters sırayla durdurma, TERM'i yok sayan servis (KILL), şablonlar,
eski betik uyumu, reload semantiği, `log=file`, `user=`, güvenlik reddetmeleri,
temiz kapanış, journald/journalult (servis akışı, syslog, `/dev/kmsg`, rotasyon, canlı takip, yedek yol), cgroup v2 (yalıtılmış mount namespace) ve PID 1 davranışı
(yalıtılmış pid namespace içinde; `reboot()` dahil).

**Kapsam dışı (gerçek donanım/VM gerektirir):** gerçek bir çekirdekten boot,
gerçek `reboot()/poweroff` ve kapanıştaki unmount / `swapoff` / kökü
salt-okunur yapma adımları (testlerde `ULT_TEST_MODE` ile atlanır), gerçek
udev/dbus/sddm zinciri. Bunları QEMU'da denemeniz önerilir.

## Lisans

**MEMO Free Code License (MFCL)** — Telif (c) 2026 Memo151515.
Bkz. `LICENSE`. Kısaca: serbestçe kullan/değiştir/dağıt (kapalı kaynak
türevler dahil), ancak yazara (Memo151515) görünür bir atıf yap.

## Sürüm

**1.2.0** — Merkezi log (`ult-journald` + `journalult`), `log=journal`,
`ready_path`. (1.1.0: yerel `.svc` birimleri, reconciler, kontrol soketi,
cgroup v2, erken boot, doğru kapanış.) Ayrıntı: `CHANGELOG.md`.
