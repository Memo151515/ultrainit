#!/bin/sh
# UltraInit test paketi: gercek surecler, yalitilmis bir kok dizini ve ayri
# derlenmis ikili dosyalarla calisir. Gercek sisteme dokunmaz.
#   sh tests/run-tests.sh            -> tum testler
#   ULT_SKIP_PID1=1 sh tests/run-tests.sh   -> PID 1 (unshare) testini atla

set -u
if [ "$(id -u)" != 0 ]; then
    echo "Bu testler root gerektirir (guvenlik denetimi root'a ait dosya ister)."
    echo "Calistirin: sudo make test"
    exit 77
fi
umask 022
SRC=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d /tmp/ult-test.XXXXXX)
chmod 755 "$T"
PASS=0; FAIL=0
INIT_PID=""

cleanup() {
    [ -n "$INIT_PID" ] && kill "$INIT_PID" 2>/dev/null
    pkill -f "$T/" 2>/dev/null
    rm -rf "$T"
}
trap cleanup EXIT

ok()   { PASS=$((PASS+1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

# ---------- derleme ----------
mkdir -p "$T/bin"
build() { # cikti kaynaklar...
    out=$1; shift
    cc -O1 -g -Wall -Wextra -std=c11 -D_GNU_SOURCE ${CFLAGS_EXTRA:-} \
        "-DULT_CONF_DIR=\"$T/etc/ultrainit\"" "-DULT_RUN_DIR=\"$T/run\"" \
        "-DULT_LOG_FILE=\"$T/ultrainit.log\"" "-DULT_SVC_LOG_DIR=\"$T/svclog\"" \
        "-DULT_CGROUP_ROOT=\"${CGROOT:-$T/nocgroup}\"" \
        "-DULT_JOURNALULT_PATH=\"$T/bin/journalult\"" \
        "-DULT_JOURNAL_DIR=\"$T/journal\"" "-DULT_JOURNAL_SOCK=\"$T/run/journal.stream\"" \
        "-DULT_SYSLOG_SOCK=\"$T/run/syslog.sock\"" -DULT_TEST_MODE -o "$out" "$@"
}
build "$T/bin/ultrainit" "$SRC/src/init.c" "$SRC/src/unit.c" "$SRC/src/common.c" \
    || { echo "derleme basarisiz"; exit 1; }
build "$T/bin/ult-service" "$SRC/src/ult-service.c" "$SRC/src/common.c" \
    || { echo "derleme basarisiz"; exit 1; }
build "$T/bin/ult-journald" "$SRC/src/journald.c" "$SRC/src/common.c" \
    || { echo "derleme basarisiz"; exit 1; }
build "$T/bin/journalult" "$SRC/src/journalult.c" "$SRC/src/common.c" \
    || { echo "derleme basarisiz"; exit 1; }
INIT="$T/bin/ultrainit"; CTL="$T/bin/ult-service"; JC="$T/bin/journalult"

# ---------- yardimcilar ----------
# 'pgrep -f' kendi komut satirini da eslestirebildigi icin comm+arguman bakilir
leftover() { ps -eo comm=,args= | awk '$1=="sleep" && $3=="300" {f=1} END{exit !f}'; }
E="$T/etc/ultrainit"; SV="$E/services"; ID="$E/init.d"; RL="$E/runlevels/default"
ORDER="$T/order"
mkdir -p "$SV" "$ID" "$RL" "$T/run" "$T/svclog"
chmod 755 "$T/etc" "$E" "$SV" "$ID" "$E/runlevels" "$RL"

svc()  { printf '%s\n' "$2" > "$SV/$1.svc"; chmod 644 "$SV/$1.svc"; }
enable_link() { ln -sf "$SV/$1.svc" "$RL/$1"; }
state() { "$CTL" "$1" status 2>/dev/null | head -1 | awk '{print $2}'; }
pidof_svc() { "$CTL" "$1" status 2>/dev/null | head -1 | sed -n 's/.*(pid \([0-9]*\)).*/\1/p'; }
wait_state() { # servis durum saniye
    i=0; max=$(( $3 * 10 ))
    while [ $i -lt $max ]; do
        [ "$(state "$1")" = "$2" ] && return 0
        sleep 0.1; i=$((i+1))
    done
    return 1
}
wait_pid_change() { # servis eskipid saniye
    i=0; max=$(( $3 * 10 ))
    while [ $i -lt $max ]; do
        np=$(pidof_svc "$1")
        [ -n "$np" ] && [ "$np" != "$2" ] && return 0
        sleep 0.1; i=$((i+1))
    done
    return 1
}

cat > "$E/ultrainit.conf" <<CONF
default_runlevel=default
max_parallel=8
respawn_window=30
respawn_max=3
shutdown_timeout=15
getty_ttys=tty1
CONF
chmod 644 "$E/ultrainit.conf"

# ---------- birimler ----------
# siralama: c -> (want) b -> (need) a   (hepsi oneshot, ORDER dosyasina yazar)
svc a "exec=/bin/sh -c \"sleep 0.3; echo a >> $ORDER\"
type=oneshot
log=null"
svc b "exec=/bin/sh -c \"echo b >> $ORDER\"
type=oneshot
need=a
log=null"
svc c "exec=/bin/sh -c \"echo c >> $ORDER\"
type=oneshot
want=b
after=b
log=null"

# denetim
svc sleeper "exec=/bin/sleep 300
restart=always
restart_sec=0
log=null"
svc crasher "exec=/bin/false
restart=always
restart_sec=0
log=null"
# bagimlilik basarisizligi
svc bad "exec=/nonexistent/binary
log=null"
svc dep "exec=/bin/sleep 300
need=bad
log=null"
# durdurma sirasi: app, base'e bagli
svc base "exec=/bin/sleep 300
stop_exec=/bin/sh -c \"echo stop-base >> $ORDER\"
log=null"
svc app "exec=/bin/sleep 300
need=base
stop_exec=/bin/sh -c \"echo stop-app >> $ORDER\"
log=null"
# TERM'i yok sayan servis
svc stubborn "exec=/bin/sh -c \"trap '' TERM; while :; do sleep 1; done\"
timeout_stop=1
log=null"
# sablon (getty yerine sleep)
svc 'getty@' "exec=/bin/sleep 300
description=tty %i
restart=always
log=null"
# yerel birim ile cakisan guvensiz birimler
svc evil "exec=/bin/true"
chmod 666 "$SV/evil.svc"
ln -sf /bin/true "$SV/link.svc"
# log=file ve kullanici degistirme
svc uidtest "exec=/bin/sh -c \"echo uid=\$(id -u) hello\"
type=oneshot
user=nobody
log=file"
# journald YOKKEN log=journal: dosyaya dusmeli
svc jfallback "exec=/bin/sh -c \"echo fallback-works\"
type=oneshot
log=journal"
# gec etkinlestirilecek
svc late "exec=/bin/sleep 300
log=null"

# eski tip betik: depend() statik okunur, pidfile ile denetlenir
cat > "$ID/legacy" <<'SH'
depend() {
    need base
}
start() {
    sleep 300 &
    echo $! > "$ULT_PIDFILE"
}
stop() {
    [ -f "$ULT_PIDFILE" ] && kill "$(cat "$ULT_PIDFILE")" 2>/dev/null
    rm -f "$ULT_PIDFILE"
}
status() { :; }
SH
chmod 755 "$ID/legacy"
ln -sf "$ID/legacy" "$RL/legacy"

for n in c sleeper crasher dep app stubborn uidtest; do enable_link $n; done

echo "== UltraInit testleri (gecici kok: $T) =="

# ---------- init'i baslat ----------
"$INIT" > "$T/init.out" 2>&1 &
INIT_PID=$!
i=0; while [ ! -S "$T/run/control" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
check "kontrol soketi olustu" '[ -S "$T/run/control" ]'
sleep 1.5

echo "-- bagimlilik sirasi"
check "a -> b -> c sirasiyla calisti" '[ "$(head -3 "$ORDER" | tr "\n" " ")" = "a b c " ]'
check "oneshot zinciri DONE" '[ "$(state a)" = done ] && [ "$(state b)" = done ] && [ "$(state c)" = done ]'

echo "-- denetim / respawn"
check "sleeper calisiyor" '[ "$(state sleeper)" = running ]'
OLD=$(pidof_svc sleeper)
kill -9 "$OLD" 2>/dev/null
check "oldurulen servis yeniden basladi (yeni pid)" 'wait_pid_change sleeper "$OLD" 5'
check "surekli cokeren servis FAILED oldu (respawn siniri)" 'wait_state crasher failed 8'

echo "-- bagimlilik basarisizligi"
check "bad FAILED" '[ "$(state bad)" = failed ]'
check "dep, bad yuzunden FAILED" '[ "$(state dep)" = failed ] && "$CTL" dep status | grep -q "bagimlilik basarisiz"'

check "bad: exec hatasi nedeni raporlandi" '"$CTL" bad status | grep -q "No such file"'
check "log=file: cikti servis logunda" 'grep -q "hello" "$T/svclog/uidtest.log"'
if id nobody >/dev/null 2>&1; then
    check "user=nobody: surec kullanici degistirdi" 'grep -q "uid=$(id -u nobody)" "$T/svclog/uidtest.log"'
fi

echo "-- sablon + eski tip betik"
check "getty@tty1 sablonundan orneklendi" '[ "$(state getty@tty1)" = running ]'
check "eski tip betik calisiyor (depend statik: base cekildi)" '[ "$(state legacy)" = running ] && [ "$(state base)" = running ]'
check "betik surecleri pidfile ile denetleniyor" '[ -n "$(pidof_svc legacy)" ]'
LOLD=$(pidof_svc legacy); kill -9 "$LOLD" 2>/dev/null
check "betik servisi olunce yeniden baslatildi" 'wait_pid_change legacy "$LOLD" 6'

echo "-- ult-service komutlari"
check "list calisiyor" '"$CTL" list | grep -q sleeper'
O1=$(pidof_svc sleeper)
"$CTL" sleeper restart >/dev/null 2>&1
check "restart yeni pid verdi" '[ "$(pidof_svc sleeper)" != "$O1" ] && [ "$(state sleeper)" = running ]'
"$CTL" late start >/dev/null 2>&1
check "start (runlevel disi) calisti" '[ "$(state late)" = running ]'
"$CTL" late stop >/dev/null 2>&1
check "stop durdurdu" '[ "$(state late)" = inactive ]'

"$CTL" jfallback start >/dev/null 2>&1
check "log=journal, journald yokken servis loguna dustu (yedek yol)" 'grep -q fallback-works "$T/svclog/jfallback.log"'

echo "-- durdurma sirasi (app, base'ten once)"
: > "$ORDER"
"$CTL" base stop >/dev/null 2>&1
check "base durunca bagimli app once durdu" '[ "$(tr "\n" " " < "$ORDER")" = "stop-app stop-base " ]'
check "ikisi de inactive" '[ "$(state app)" = inactive ] && [ "$(state base)" = inactive ]'
"$CTL" app start >/dev/null 2>&1
check "app start base_i otomatik cekti" '[ "$(state app)" = running ] && [ "$(state base)" = running ]'

echo "-- TERM'i yok sayan servis"
T0=$(date +%s)
"$CTL" stubborn stop >/dev/null 2>&1
T1=$(date +%s)
check "stubborn KILL ile durduruldu" '[ "$(state stubborn)" = inactive ] && [ $((T1-T0)) -le 5 ]'

echo "-- reload (durum korunur)"
SP=$(pidof_svc sleeper)
kill -HUP "$INIT_PID"; sleep 1
check "SIGHUP sonrasi servisler kesintisiz (pid ayni)" '[ "$(pidof_svc sleeper)" = "$SP" ] && [ "$(state getty@tty1)" = running ]'
"$CTL" late enable >/dev/null 2>&1; "$CTL" reload >/dev/null 2>&1; sleep 0.5
check "enable+reload yeni servisi baslatti" '[ "$(state late)" = running ]'
"$CTL" late disable >/dev/null 2>&1; "$CTL" reload >/dev/null 2>&1; sleep 1
check "disable+reload servisi durdurdu" '[ "$(state late)" = inactive ]'
check "reload sleeper'i etkilemedi" '[ "$(pidof_svc sleeper)" = "$SP" ]'
check "reload, elle durdurulan servisi (stubborn) tekrar baslatmadi" '[ "$(state stubborn)" = inactive ]'

echo "-- guvenlik"
"$CTL" evil start >/dev/null 2>&1
check "grup/diger yazilabilir birim reddedildi" 'grep -q "evil.svc.*yazilabilir" "$T/ultrainit.log"'
"$CTL" link start >/dev/null 2>&1
check "symlink birim reddedildi" 'grep -q "link.svc.*sembolik" "$T/ultrainit.log"'
check "gecersiz servis adi reddedildi" '! "$CTL" "../etc" start >/dev/null 2>&1'
chmod 666 "$E/ultrainit.conf"; kill -HUP "$INIT_PID"; sleep 0.5
check "guvensiz ana config reddedildi" 'grep -q "ana config dogrulanamadi" "$T/ultrainit.log"'
chmod 644 "$E/ultrainit.conf"

echo "-- temiz kapanis"
: > "$ORDER"
"$CTL" poweroff >/dev/null 2>&1
i=0; while kill -0 "$INIT_PID" 2>/dev/null && [ $i -lt 150 ]; do sleep 0.1; i=$((i+1)); done
if kill -0 "$INIT_PID" 2>/dev/null; then bad "init kapanista cikmadi"; else
    wait "$INIT_PID"; RC=$?; INIT_PID=""
    check "init temiz cikti (rc=0)" '[ "$RC" = 0 ]'
fi
check "kapanista app, base'ten once durduruldu" 'grep -n "stop-" "$ORDER" | sed "s/^[0-9]*://" | tr "\n" " " | grep -q "^stop-app stop-base "'
check "kontrol soketi temizlendi" '[ ! -S "$T/run/control" ]'
check "arta kalan surec yok" '! leftover'

# ---------- cgroup v2 testi (yalitilmis mount namespace) ----------
echo "-- cgroup v2 (unshare icinde)"
NSARGS="--mount --propagation private"
mkdir -p "$T/cg"
if [ "${ULT_SKIP_PID1:-0}" = 1 ] || ! command -v unshare >/dev/null 2>&1 ||
   ! unshare $NSARGS sh -c "mount -t cgroup2 none '$T/cg'" 2>/dev/null; then
    echo "  (atlandi: cgroup2 bu ortamda baglanamiyor)"
else
    CGROOT="$T/cg" build "$T/bin/ultrainit-cg" "$SRC/src/init.c" "$SRC/src/unit.c" "$SRC/src/common.c" \
        || { echo "derleme basarisiz"; exit 1; }
    rm -f "$RL"/*; rm -f "$T/run/control"
    # ana surec + process grubundan kacan (setsid) artik surec
    svc leaky "exec=/bin/sh -c \"setsid sleep 402 & exec sleep 300\"
log=null"
    enable_link leaky
    unshare $NSARGS sh -c "mount -t cgroup2 none '$T/cg' && exec '$T/bin/ultrainit-cg'" > "$T/cg.out" 2>&1 &
    CGP=$!
    i=0; while [ ! -S "$T/run/control" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
    sleep 1
    check "cgroup v2 destegi etkinlesti" 'grep -q "cgroup v2 destegi etkin" "$T/cg.out"'
    LP=$(pidof_svc leaky)
    check "servis kendi cgroup'una tasindi" '[ -n "$LP" ] && grep -q "ultrainit/leaky" "/proc/$LP/cgroup"'
    leftover402() { ps -eo comm=,args= | awk '$1=="sleep" && $3=="402" {f=1} END{exit !f}'; }
    check "kacak artik surec (setsid) calisiyor" 'leftover402'
    "$CTL" leaky stop >/dev/null 2>&1
    sleep 0.5
    check "stop, cgroup uzerinden kacak sureci de oldurdu" '[ "$(state leaky)" = inactive ] && ! leftover402'
    "$CTL" poweroff >/dev/null 2>&1
    i=0; while kill -0 "$CGP" 2>/dev/null && [ $i -lt 100 ]; do sleep 0.1; i=$((i+1)); done
    unshare $NSARGS sh -c "mount -t cgroup2 none '$T/cg' && rmdir '$T/cg'/ultrainit/* '$T/cg/ultrainit' 2>/dev/null; true"
    rm -f "$RL"/*
fi

# ---------- journald / journalult ----------
echo "-- journald + journalult"
rm -f "$RL"/*; rm -f "$T/run/control"
JREADY="$T/run/journal.stream"
KMSG_OK=0
if ( echo "<6>ult-probe" > /dev/kmsg ) 2>/dev/null && [ -r /dev/kmsg ]; then KMSG_OK=1; fi
JARGS=""; [ "$KMSG_OK" = 1 ] || JARGS=" --no-kmsg"
svc journald "exec=/bin/sh -c \"exec $T/bin/ult-journald$JARGS 2>>$T/journald.err\"
ready_path=$JREADY
restart=always
timeout_start=10
log=null"
svc chatty "exec=/bin/sh -c \"echo out-line-1; echo err-line-1 >&2; printf 'esc\\033[31mred\\n'; sleep 300\"
log=journal"
svc flood "exec=/bin/sh -c \"i=0; while [ \$i -lt 4000 ]; do echo flood-line-\$i-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx; i=\$((i+1)); done\"
type=oneshot
log=journal"
enable_link chatty     # journald, log=journal nedeniyle otomatik cekilir
"$INIT" > "$T/init-j.out" 2>&1 &
INIT_PID=$!
i=0; while [ ! -S "$T/run/control" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
sleep 2

check "journald otomatik cekildi ve calisiyor (log=journal -> want)" '[ "$(state journald)" = running ] && [ "$(state chatty)" = running ]'
check "ready_path: journald hazir olduktan sonra baslatildi (dosyaya dusmedi)" '[ ! -s "$T/svclog/chatty.log" ]'
check "stdout satiri gunluge dustu" '"$JC" -u chatty --no-color | grep -q "out-line-1"'
check "stderr satiri gunluge dustu" '"$JC" -u chatty --no-color | grep -q "err-line-1"'
check "-p err yalnizca stderr (err) kaydini getirir" '"$JC" -u chatty -p err | grep -q err-line-1 && ! "$JC" -u chatty -p err | grep -q out-line-1'
CP=$(pidof_svc chatty)
check "PID cekirdekten (SO_PEERCRED) dogru kaydedildi" '"$JC" -u chatty --no-color | grep -q "chatty\[$CP\]"'
check "terminal kacis dizisi (ESC) etkisizlestirildi" '"$JC" -u chatty -o cat | grep -q "esc?\[31mred" && ! "$JC" -u chatty -o cat | grep -q "$(printf "\033")"'
logger -u "$T/run/syslog.sock" -t mytag -p user.warning "hello-syslog" 2>/dev/null; sleep 0.5
check "syslog (/dev/log benzeri soket) yakalandi" '"$JC" -u mytag | grep -q "hello-syslog"'
check "syslog onceligi: -p warning gorur, -p err gormez" '"$JC" -u mytag -p warning | grep -q hello-syslog && ! "$JC" -u mytag -p err | grep -q hello-syslog'
check "json cikti" '"$JC" -u chatty -o json -n 1 | grep -Eq "^\{\"timestamp_ms\":[0-9]+,\"priority\":[0-9],\"unit\":\"chatty\""'
check "--since 1h kayitlari getirir, --since 0s getirmez" '"$JC" --since 1h -u chatty | grep -q out-line-1 && ! "$JC" --since 0s -u chatty | grep -q out-line-1'
check "ult-service status son log satirlarini gosterir" '"$CTL" chatty status | grep -q "Son loglar" && "$CTL" chatty status | grep -q "out-line-1"'
check "-g buyuk/kucuk harf duyarsiz arama" '"$JC" -g OUT-LINE-1 | grep -q out-line-1'
check "--list-units birimleri sayar" '"$JC" --list-units | grep -qx chatty && "$JC" --list-units | grep -qx mytag'
check "-u ultrainit: init'in kendi logu" '"$JC" -u ultrainit -g "journald: calisiyor" | grep -q journald'
check "gunluk dizini 0750, dosyasi 0640" '[ "$(stat -c %a "$T/journal")" = 750 ] && [ "$(stat -c %a "$T/journal/journal.log")" = 640 ]'

if [ "$KMSG_OK" = 1 ]; then
    MARK="ult-kmsg-marker-$$"
    echo "<6>$MARK" > /dev/kmsg; sleep 0.7
    check "cekirdek mesaji (/dev/kmsg) yakalandi" '"$JC" -k -g "$MARK" | grep -q "kernel: $MARK"'
    "$CTL" journald restart >/dev/null 2>&1; sleep 1.5
    check "journald yeniden baslayinca kmsg tekrar kaydedilmedi" '[ "$("$JC" -k -g "$MARK" -n all | wc -l)" = 1 ]'
else
    echo "  (atlandi: /dev/kmsg bu ortamda yazilamiyor)"
fi

# canli takip
"$JC" -f -g follow-marker -u followtag --no-color > "$T/follow.out" 2>&1 &
FP=$!
sleep 0.7
logger -u "$T/run/syslog.sock" -t followtag "follow-marker-1"; sleep 1
kill "$FP" 2>/dev/null
check "-f canli takip yeni kaydi gosterdi" 'grep -q follow-marker-1 "$T/follow.out"'

# rotasyon: kucuk sinirlarla yeniden baslat
printf 'journal_max_file_kb=64\njournal_max_files=3\n' >> "$E/ultrainit.conf"
"$CTL" journald restart >/dev/null 2>&1; sleep 1.5
"$CTL" flood start >/dev/null 2>&1
i=0; while [ $i -lt 60 ] && ! "$JC" -u flood -n 1 2>/dev/null | grep -q "flood-line-3999-"; do sleep 0.2; i=$((i+1)); done
NF=$(ls "$T"/journal/journal*.log | wc -l)
check "rotasyon: dosya sayisi sinirli (<=3) ve >1" '[ "$NF" -le 3 ] && [ "$NF" -ge 2 ]'
check "rotasyon: en yeni kayit (3999) okunabiliyor" '"$JC" -u flood -n 1 | grep -q "flood-line-3999-"'
check "rotasyon: en eski kayit (0) silindi" '[ "$("$JC" -u flood -n all | grep -c "flood-line-0-")" = 0 ]'
check "--disk-usage calisiyor" '"$JC" --disk-usage | grep -q KiB'

check "journald calisma boyunca hic cokmedi (yalnizca istenen restart'lar)" '! grep -qE "journald: (yeniden baslatilacak|basarisiz)" "$T/init-j.out"'
"$CTL" poweroff >/dev/null 2>&1
i=0; while kill -0 "$INIT_PID" 2>/dev/null && [ $i -lt 150 ]; do sleep 0.1; i=$((i+1)); done
if kill -0 "$INIT_PID" 2>/dev/null; then bad "journald'li init kapanista cikmadi"; else wait "$INIT_PID"; INIT_PID=""; ok "journald'li init temiz kapandi"; fi
check "kapanista journald akis soketi temizlendi" '[ ! -S "$JREADY" ]'
rm -f "$RL"/*

# ---------- PID 1 testi (ayri pid + mount namespace) ----------
echo "-- PID 1 modu (unshare)"
if [ "${ULT_SKIP_PID1:-0}" = 1 ] || ! command -v unshare >/dev/null 2>&1; then
    echo "  (atlandi)"
elif ! unshare --pid --fork --mount --propagation private --mount-proc true 2>/dev/null; then
    echo "  (atlandi: bu ortamda unshare izni yok)"
else
    rm -f "$ORDER"
    for n in sleeper app; do enable_link $n; done
    unshare --pid --fork --mount --propagation private --mount-proc "$INIT" > "$T/pid1.out" 2>&1 &
    UP=$!
    i=0; while [ ! -S "$T/run/control" ] && [ $i -lt 60 ]; do sleep 0.1; i=$((i+1)); done
    check "PID 1 olarak basladi (pid 1 logu)" 'grep -q "(pid 1)" "$T/pid1.out"'
    sleep 1.5
    check "PID 1 altinda servisler calisiyor" '[ "$(state sleeper)" = running ]'
    "$CTL" ping >/dev/null 2>&1
    "$CTL" reboot >/dev/null 2>&1
    i=0; while kill -0 "$UP" 2>/dev/null && [ $i -lt 150 ]; do sleep 0.1; i=$((i+1)); done
    if kill -0 "$UP" 2>/dev/null; then bad "PID 1 kapanista cikmadi (panik/asili kalma)"; kill "$UP" 2>/dev/null
    else ok "PID 1 reboot() ile namespace'i sonlandirdi (cokme yok)"; fi
    check "kapanis loglandi" 'grep -q "yeniden baslatma" "$T/pid1.out"'
    check "PID 1 kapanisinda sahipsiz surec kalmadi" '! leftover'
fi

check "init ciktilarinda sanitizer/runtime hatasi yok" '! cat "$T"/init.out "$T"/init-j.out "$T"/journald.err "$T"/pid1.out "$T"/cg.out 2>/dev/null | grep -qE "Sanitizer|runtime error"'

echo
printf 'Sonuc: %d basarili, %d basarisiz\n' "$PASS" "$FAIL"
[ "$FAIL" = 0 ]
