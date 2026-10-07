# Değişiklik Günlüğü

## 1.2.0

### Yeni
- **`ult-journald`** (merkezi log toplayıcı): servis stdout/stderr akışı, syslog (`/dev/log`) ve
  çekirdek (`/dev/kmsg`) tek bir zaman damgalı, öncelikli, dönen metin günlüğünde. PID ve
  kimlik çekirdekten doğrulanır; kontrol karakterleri temizlenir; `journal_max_file_kb` /
  `journal_max_files` ile sınırlı disk; `journal_max_files` düşürülünce artan dosyalar budanır.
- **`journalult`** (okuyucu, `journalctl` karşılığı): `-u` (ön ek `*`), `-p`, `-n`, `-k`, `-g`,
  `--since/--until`, `-f`, `-o short|cat|json`, `--list-units`, `--disk-usage`; `-u ultrainit`
  ile init'in kendi logu.
- Birimlerde **`log=journal`**: journald otomatik `want` + `after` olur; hazır değilse çıktı
  dosyaya düşer (yedek yol).
- Birimlerde **`ready_path=`**: `type=simple` servis, bu yol (ör. dinleyen soket) oluşana kadar
  `starting` sayılır; bağımlılar hazır olduktan sonra başlar. Bayat soketler başlatmadan önce
  temizlenir (yalnızca soketler).
- `ult-service <servis> status` artık son 5 log satırını da gösterir.
- `services/journald.svc` birimi.

### Test
- 73 test (journald/journalult bölümü dahil); ASan/UBSan ile temiz.

- Test paketi artık root olmadan çalıştırılırsa yanıltıcı FAIL satırları basmak yerine açıkça uyarır
  ve çıkar (`sudo make test`).


## 1.1.0

### Yeni
- **Yerel `.svc` birimleri** (systemd benzeri, düz `anahtar=değer`): `exec`, `type`
  (simple/forking/oneshot), `need/want/after/before/conflicts/provides`, `restart`,
  `timeout_*`, `user/group/workdir/umask/env`, `log`. Şablon birimler (`getty@.svc`, `%i`).
- **Olay tabanlı reconciler**: hedef durum / gerçek durum eşitleme, `max_parallel`
  sınırıyla paralel başlatma, `need` başarısızlığında bağımlıyı `failed` yapma,
  `conflicts`, bağımlılık döngüsü tespiti.
- **Kontrol soketi** (`/run/ultrainit/control`): `ult-service` artık PID 1 ile konuşur;
  start/stop/restart servis yerleşene kadar bekler; canlı `status`, `list`, `reload`.
- **cgroup v2**: servis başına cgroup, `cgroup.kill` ile kaçak çocuklar dahil temizlik.
- **Erken boot** (PID 1): kök rw, `/proc /sys /dev /dev/pts /dev/shm /run`, cgroup2,
  konsol, hostname, `lo` arayüzü, Ctrl-Alt-Del → SIGINT.
- **Gerçek kapanış**: ters bağımlılık sırası, TERM→KILL, `swapoff`, unmount, kökü ro,
  `reboot()/RB_HALT/RB_POWER_OFF`; sinyaller: TERM/INT=reboot, USR1=halt, USR2/PWR=poweroff.
- `depend()` artık betik çalıştırılmadan **statik** okunur (boot'ta servis başına shell yok).
- `reload` canlı çalışır ve elle durdurulmuş servislerin hedefini geri almaz.
- Test paketi (`make test`, 46 test) ve ASan/UBSan desteği.

### Düzeltilen hatalar (1.0.x)
1. Kapanışta gerçek PID 1 `return 0` ile çıkıyordu (kernel panic); artık `reboot()` çağrılır.
2. Erken mount'lar (`/proc /sys /dev /run`, kök rw) yoktu.
3. `pause()` yarışı: sinyal kaçabiliyordu → `signalfd` + `poll`.
4. `ult_conf_get` statik tampon döndürüyordu; `default_runlevel` bozulabiliyordu → çağıran tampon verir.
5. SIGHUP reload durumu sıfırlıyordu (`g_nservices=0` + `memset`), denetim kayboluyordu.
6. Boot dalgasındaki `wait()` herhangi bir çocuğu topluyordu; sayaç bozuluyor, ölen daemon'ların
   respawn kaydı kayboluyordu → yalnızca ilgili pid'ler izlenir.
7. Kapanış sırası `readdir` sırasının tersiydi → gerçek ters bağımlılık sırası.
8. `need` başarısız olsa da bağımlılar başlatılıyordu.
9. Respawn sırasında `waitpid` PID 1'i bloke ediyordu → bloklamayan zamanlayıcı.
10. `max_parallel` ve `respawn_window` config'te vardı ama okunmuyordu.
11. Güvensiz betikler "devre dışı" loglanıp grafikte kalıyordu → yüklenmez.
12. Kontrol–kullanım yarışı ve üst dizinlerin doğrulanmaması → fd tabanlı doğrulama + dizin zinciri.
13. Servis adı doğrulaması yoktu (`/`, `'`) → `ult_valid_name`.
14. `./ult-service` göreli yol yedeği kaldırıldı.
15. Servislere gereksiz `umask 027` miras kalıyordu → servislerde 022 (birim başına `umask=`).
16. "Shell katmanı yok" iddiası yanlıştı; belgelendi ve gereksiz ara `ult-service` süreci kaldırıldı.
17. README/`common.h` lisans satırları MFCL'ye çevrildi.

### Uyumluluk
- Eski `init.d` betikleri ve `runlevels/` symlink düzeni aynen çalışır.
- Mevcut `ultrainit.conf` korunur; yeni anahtarlar yoksa varsayılanlar kullanılır.
- `getty_ttys` için `services/getty@.svc` şablonu gerekir (`make install` kurar).
