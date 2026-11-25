#!/bin/sh
# Stand-in for rclone in test_uploader.cpp. FAKE_RCLONE_LOG receives one line of
# arguments per run and FAKE_RCLONE_PID the process ID. FAKE_RCLONE_MODE:
#   ok    print progress like "rclone --use-json-log" and exit 0
#   fail  print an error line and exit 7
#   critical  print a critical line and exit 1 (an unknown remote)
#   slow  run for 30 s (stops on SIGTERM)
#   hang  run for 30 s and ignore SIGTERM
# A program that cannot change its own environment reads the mode from a file.
[ -n "$FAKE_RCLONE_MODE_FILE" ] && FAKE_RCLONE_MODE=$(cat "$FAKE_RCLONE_MODE_FILE")
echo "$*" >> "$FAKE_RCLONE_LOG"
echo $$ > "$FAKE_RCLONE_PID"
# Lines copied from rclone v1.75.2 (--use-json-log --stats-log-level NOTICE), shortened.
stats='{"time":"2026-10-28T11:39:14.465773447-04:00","level":"notice","msg":"\nTransferred:   \t 2 KiB / 4 KiB, 50%, 0 B/s, ETA -\n","stats":{"bytes":2048,"checks":0,"deletedDirs":0,"deletes":0,"elapsedTime":1.0,"errors":0,"eta":null,"fatalError":false,"listed":2,"renames":0,"retryError":false,"speed":0,"totalBytes":4096,"totalChecks":0,"totalTransfers":2,"transferTime":1.0,"transferring":[{"bytes":1044480,"dstFs":"t/dst","eta":null,"group":"global_stats","name":"b.mp4","percentage":52,"size":2000000,"speed":1047541.77,"speedAvg":0,"srcFs":"t/src"}],"transfers":1},"source":"accounting/stats.go:549"}'
case "$FAKE_RCLONE_MODE" in
ok)
    echo "$stats" >&2
    exit 0
    ;;
fail)
    echo '{"time":"2026-10-28T11:39:15.542592399-04:00","level":"error","msg":"Failed to copy: quota \"exceeded\"","object":"a.jpg","objectType":"*local.Object","source":"operations/copy.go:1"}' >&2
    exit 7
    ;;
critical)
    echo '{"time":"2026-10-28T11:39:15.67925257-04:00","level":"critical","msg":"Failed to create file system for \"fake:cam/photos\": didn'"'"'t find section in config file (\"fake\")","source":"cmd/cmd.go:148"}' >&2
    exit 1
    ;;
slow)
    echo "$stats" >&2
    exec sleep 30
    ;;
hang)
    trap '' TERM
    echo "$stats" >&2
    n=0
    while [ $n -lt 300 ]; do sleep 0.1; n=$((n + 1)); done
    ;;
esac
exit 0
