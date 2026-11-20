#!/bin/sh
# Detached RAM-only ZT30 application test supervisor. See analysis/ZT30/RAM_SESSION.md.
# A separate checker must confirm persistent hashes and RTSP before app_restore resumes.
set -eu
[ "$#" -ge 5 ] || { echo 'usage: watchdog SECONDS SESSION VENDOR_PID RESTORE_PID COMMAND [ARGS...]' >&2; exit 2; }
verify_seconds=${APCG_ZT30_VERIFY_SECONDS:-60}
case "$verify_seconds" in ''|*[!0-9]*) exit 2;; esac
[ "$verify_seconds" -ge 10 ] && [ "$verify_seconds" -le 120 ] || exit 2
seconds=$1; session=$2; vendor_pid=$3; restore_pid=$4; shift 4
case "$seconds" in ''|*[!0-9]*) exit 2;; esac
[ "$seconds" -ge 1 ] && [ "$seconds" -le 60 ] || exit 2
case "$session" in /tmp/apcg-zt30.*) ;; *) exit 2;; esac
case "${session#/tmp/}" in */*) exit 2;; esac
[ -d "$session" ] && [ ! -L "$session" ] || exit 2
[ "$(cd "$session" && pwd -P)" = "$session" ] || exit 2
# This check is replaced only in the host fixture; production requires camera tmpfs.
awk '$2=="/tmp" && $3=="tmpfs" {found=1} END {exit !found}' /proc/mounts || exit 2
[ -f "$session/preflight.ok" ] && [ ! -e "$session/vendor-verified" ] || exit 2
case "$vendor_pid:$restore_pid" in *[!0-9:]*) exit 2;; esac
[ "$vendor_pid" -gt 1 ] && [ "$restore_pid" -gt 1 ] && [ "$vendor_pid" != "$restore_pid" ] || exit 2
vendor_args=$(tr '\000' ' ' < "/proc/$vendor_pid/cmdline")
restore_args=$(tr '\000' ' ' < "/proc/$restore_pid/cmdline")
[ "$vendor_args" = '/customer/siyi_app_359 /customer/359_default.ini ' ] || { echo 'unexpected vendor argv' >&2; exit 2; }
case "$restore_args" in 'ash /customer/app_restore.sh '|'sh /customer/app_restore.sh '|'/bin/sh /customer/app_restore.sh ') ;; *) echo 'unexpected app_restore argv' >&2; exit 2;; esac
command -v setsid >/dev/null
command -v xargs >/dev/null
[ -x "$session/vendor-exec" ] || { echo "vendor-exec missing" >&2; exit 2; }
# nohup's inherited ignored HUP cannot be reliably reset by a shell. Use setsid.
hup_mask=$(awk '/^SigIgn:/ {print $2}' /proc/$$/status)
case "$hup_mask" in *[13579bBdDfF]) echo 'do not launch through nohup' >&2; exit 2;; esac
case "$hup_mask" in *[4567cCdDeEfF]???) echo 'launcher leaves TERM ignored' >&2; exit 2;; esac
owners() {
    allowed=$1; output=$2
    # One ls avoids spawning a subprocess for every fd on large hosts.
    ls -l /proc/[0-9]*/fd/* > "$session/owners.scan" 2> "$session/owners.scan.stderr" || true
    grep -v 'No such file or directory' "$session/owners.scan.stderr" > "$session/owners.scan.errors" || true
    [ ! -s "$session/owners.scan.errors" ] || return 1
    awk -v allowed="$allowed" '
        { for(i=1;i<NF;i++) if($i=="->") {
            target=$(i+1)
            if(index(target,"/dev/mi_")==1 || index(target,"/dev/isp_")==1 ||
               index(target,"/dev/cam_os")==1 || target=="/dev/ttyS1" ||
               index(target,"/dev/bus/usb/")==1) {
                split($(i-1),parts,"/")
                if(parts[3]!=allowed) print parts[3],target
            }
        } }
    ' "$session/owners.scan" > "$output"
    [ ! -s "$output" ] || return 1
    # A process can close a device fd while retaining a mapping or loaded SDK.
    grep -H -E '/dev/|/config/lib/libmi_|/dev/mi_|/dev/isp_|/dev/cam_os|/dev/ttyS1|/dev/bus/usb/' /proc/[0-9]*/maps > "$session/owners.maps.scan" 2> "$session/owners.maps.stderr" || true
    grep -v -E 'No such file or directory|No such process' "$session/owners.maps.stderr" > "$session/owners.maps.errors" || true
    [ ! -s "$session/owners.maps.errors" ] || return 1
    awk -v allowed="$allowed" '
        {
            target=$NF
            if(target=="(deleted)") target=$(NF-1)
            split($1,parts,"/")
            if(parts[3]==allowed || target=="/dev/zero") next
            if(index(target,"/dev/")==1 || index(target,"/config/lib/libmi_")==1 ||
               index(target,"/dev/mi_")==1 || index(target,"/dev/isp_")==1 ||
               index(target,"/dev/cam_os")==1 || target=="/dev/ttyS1" ||
               index(target,"/dev/bus/usb/")==1) print parts[3],"mapped",target
        }
    ' "$session/owners.maps.scan" >> "$output"
    [ ! -s "$output" ]
}

owners "$vendor_pid" "$session/owners.preflight" || { echo 'unexpected SDK/peripheral owner' >&2; exit 2; }
cat "/proc/$vendor_pid/status" > "$session/vendor.status"
for fd in 0 1 2; do
    readlink "/proc/$vendor_pid/fd/$fd" > "$session/vendor.fd$fd"
    case "$(cat "$session/vendor.fd$fd")" in /dev/null|/dev/console) ;; *) echo 'unsupported vendor standard fd' >&2; exit 2;; esac
done
vendor_groups=$(awk '/^Groups:/ {sub(/^[^:]*:[[:space:]]*/, ""); print}' "$session/vendor.status")
"$session/vendor-exec" --check-groups "$vendor_groups" > "$session/launcher-preflight.log" 2>&1 || { echo "vendor launcher preflight failed" >&2; exit 2; }
vendor_umask=$(awk '/^Umask:/ {print $2}' "$session/vendor.status")
case "$vendor_umask" in 0[0-7][0-7][0-7]) ;; *) exit 2;; esac
ls -l "/proc/$vendor_pid/fd" > "$session/vendor.fds"
stat_fields() { sed 's/.*) //' "/proc/$1/stat" 2>/dev/null; }
start_time() { stat_fields "$1" | awk '{print $20}'; }
state() { stat_fields "$1" | awk '{print $1}'; }
identity() { [ "$(start_time "$1")" = "$2" ] && [ -n "$2" ]; }
live() { case "$(state "$1")" in ''|Z|X) return 1;; *) return 0;; esac; }
[ "$(state "$restore_pid")" != T ] || exit 2
# Refuse to guess resource limits. Restart inherits these exact matching limits.
cat "/proc/$vendor_pid/limits" > "$session/vendor.limits"
cat "/proc/$$/limits" > "$session/watchdog.limits"
cmp -s "$session/vendor.limits" "$session/watchdog.limits" || { echo 'vendor/watchdog limits differ; qualify launcher first' >&2; exit 2; }
( set -C; echo "$$" > "$session/watchdog.pid" )
cat "/proc/$vendor_pid/environ" > "$session/vendor.environ"
[ "$(wc -c < "$session/vendor.environ")" -lt 16384 ] || exit 2
cat "/proc/$vendor_pid/cmdline" > "$session/vendor.cmdline"
cat "/proc/$restore_pid/cmdline" > "$session/restore.cmdline"
readlink "/proc/$vendor_pid/cwd" > "$session/vendor.cwd"
cat "/proc/$vendor_pid/stat" > "$session/vendor.stat"
cat "/proc/$restore_pid/stat" > "$session/restore.stat"
vendor_start=$(start_time "$vendor_pid"); restore_start=$(start_time "$restore_pid")
frozen=0; changed=0; experiment_pid=''; experiment_start=''
group_members() {
    for f in /proc/[0-9]*/stat; do
        fields=$(sed 's/.*) //' "$f" 2>/dev/null) || continue
        set -- $fields
        [ "$#" -ge 20 ] || continue
        [ "$3" = "$experiment_pid" ] && [ "$4" = "$experiment_pid" ] || continue
        case "$1" in Z|X) continue;; esac
        p=${f#/proc/}; p=${p%/stat}
        printf '%s %s\n' "$p" "${20}"
    done
}
stop_group() {
    [ -n "$experiment_pid" ] || return 0
    # The wrapper stays the process-group leader until rollback, even after command exit.
    identity "$experiment_pid" "$experiment_start" || { echo 'experiment leader identity lost' >&2; return 1; }
    group_members > "$session/group.before-stop"
    kill -TERM "-$experiment_pid" 2>/dev/null || true
    sleep 5
    # While an original identity anchors the group, KILL also catches late forks.
    count=0
    while [ "$count" -lt 3 ]; do
        group_members > "$session/group.remaining"
        [ ! -s "$session/group.remaining" ] && { experiment_pid=''; return 0; }
        anchored=0
        identity "$experiment_pid" "$experiment_start" && anchored=1
        while read -r p born; do
            identity "$p" "$born" && live "$p" && anchored=1
        done < "$session/group.before-stop"
        [ "$anchored" = 1 ] || { echo 'experiment group identity lost' >&2; return 1; }
        cp "$session/group.remaining" "$session/group.before-stop"
        kill -KILL "-$experiment_pid" 2>/dev/null || true
        sleep 1; count=$((count+1))
    done
    group_members > "$session/group.remaining"
    [ ! -s "$session/group.remaining" ] && { experiment_pid=''; return 0; }
    echo 'experiment group did not exit; refusing another MI owner' >&2
    return 1
}
rollback() {
    rc=$?
    trap - EXIT
    trap ':' HUP INT TERM
    set +e
    if ! stop_group; then
        echo 'FAILED: experiment cleanup; app_restore suspended; operator power-cycle required' > "$session/result"
        exit 5
    fi
    if [ "$changed" = 1 ]; then
        # An interrupted vendor stop must never start a duplicate pipeline owner.
        if identity "$vendor_pid" "$vendor_start" && live "$vendor_pid"; then
            echo 'FAILED: original vendor still alive; app_restore suspended' > "$session/result"
            exit 5
        fi
        if ! owners '' "$session/owners.before-restart"; then
            echo 'FAILED: remaining SDK/peripheral owner; app_restore suspended' > "$session/result"
            exit 5
        fi
        cwd=$(cat "$session/vendor.cwd")
        # NUL environment values are data, never evaluated as shell code.
        xargs -0 sh -c 'cwd=$1; input=$2; output=$3; error=$4; mask=$5; groups=$6; launcher=$7; shift 7; cd "$cwd" || exit; umask "$mask"; exec env -i "$@" "$launcher" "$groups" /customer/siyi_app_359 /customer/359_default.ini < "$input" > "$output" 2> "$error"' sh "$cwd" "$(cat "$session/vendor.fd0")" "$(cat "$session/vendor.fd1")" "$(cat "$session/vendor.fd2")" "$vendor_umask" "$vendor_groups" "$session/vendor-exec" < "$session/vendor.environ" > "$session/vendor-restore.log" 2>&1 &
        count=0
        while [ "$count" -lt 15 ]; do
            pids=$(pidof siyi_app_359); candidate=''; multiple=0
            for p in $pids; do
                live "$p" || continue
                args=$(tr '\000' ' ' < "/proc/$p/cmdline")
                [ "$args" = "$vendor_args" ] || continue
                if [ -n "$candidate" ]; then multiple=1; else candidate=$p; fi
            done
            if [ -n "$candidate" ] && [ "$multiple" = 0 ]; then
                echo "$candidate" > "$session/vendor-restored.pid"; break
            fi
            sleep 1; count=$((count+1))
        done
        [ -s "$session/vendor-restored.pid" ] || { echo 'FAILED: vendor restart; app_restore suspended' > "$session/result"; exit 5; }
        count=0
        while [ "$count" -lt "$verify_seconds" ] && [ ! -e "$session/vendor-verified" ]; do sleep 1; count=$((count+1)); done
        [ -e "$session/vendor-verified" ] || { echo 'FAILED: verification missing; app_restore suspended' > "$session/result"; exit 5; }
    fi
    if [ "$frozen" = 1 ]; then
        identity "$restore_pid" "$restore_start" || { echo 'FAILED: app_restore identity lost' > "$session/result"; exit 5; }
        kill -CONT "$restore_pid" || exit 5
    fi
    if [ "$changed" = 1 ]; then echo 'vendor restored and externally verified' > "$session/result"
    else echo 'no service changes' > "$session/result"; fi
    exit "$rc"
}
trap rollback EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
echo ready > "$session/watchdog.ready"
count=0
while [ ! -e "$session/start" ] && [ "$count" -lt 30 ]; do
    [ -e "$session/stop" ] && exit 3
    sleep 1; count=$((count+1))
done
[ -e "$session/start" ] || exit 3
identity "$restore_pid" "$restore_start" && identity "$vendor_pid" "$vendor_start" || exit 4
# Mark suspension before the signal so an interrupted STOP is paired with CONT.
frozen=1; kill -STOP "$restore_pid"
count=0
while [ "$(state "$restore_pid")" != T ] && [ "$count" -lt 3 ]; do sleep 1; count=$((count+1)); done
[ "$(state "$restore_pid")" = T ] || { echo 'app_restore not suspended' >&2; exit 4; }
# Stop may be interrupted; changed requires rollback to verify before resuming.
changed=1; kill -TERM "$vendor_pid"
count=0
while live "$vendor_pid" && [ "$count" -lt 5 ]; do sleep 1; count=$((count+1)); done
if identity "$vendor_pid" "$vendor_start" && live "$vendor_pid"; then kill -KILL "$vendor_pid"; sleep 1; fi
identity "$vendor_pid" "$vendor_start" && live "$vendor_pid" && { echo 'vendor did not exit' >&2; exit 4; }
owners '' "$session/owners.before-experiment" || { echo 'remaining SDK/peripheral owner' >&2; exit 4; }
# Defer signals across asynchronous launch until its identity is captured.
pending_signal=''
trap 'pending_signal=129' HUP
trap 'pending_signal=130' INT
trap 'pending_signal=143' TERM
# Wrapper isolates descendants in one session and keeps a verifiable leader alive.
setsid sh -c 'cd "$1" || exit; shift; "$@"; echo "$?" > experiment.exit; while :; do sleep 1; done' sh "$session" "$@" > "$session/experiment.log" 2>&1 &
experiment_pid=$!; experiment_start=$(start_time "$experiment_pid")
echo "$experiment_pid" > "$session/experiment.pid"
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
[ -z "$pending_signal" ] || exit "$pending_signal"
count=0
while [ "$count" -lt "$seconds" ] && live "$experiment_pid"; do
    [ -e "$session/stop" ] || [ -e "$session/experiment.exit" ] && break
    sleep 1; count=$((count+1))
done
exit 0
