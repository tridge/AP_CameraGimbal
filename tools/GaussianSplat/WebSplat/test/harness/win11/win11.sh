#!/bin/bash
# Start/stop the harness Chrome on the win11 machine and the ssh tunnels to it:
#   -L CDP_PORT: this machine reaches the remote Chrome's DevTools endpoint
#   -R APP_PORT: the remote Chrome reaches this machine's harness web server as its localhost
# usage: win11.sh start|stop|status   (env: CDP_PORT=9345 APP_PORT=8795 HOST=win11
#   PROFILE=websplat-harness-profile: Chrome profile name; stop only stops that profile's Chrome)
set -e
HOST=${HOST:-win11}
CDP_PORT=${CDP_PORT:-9345}
APP_PORT=${APP_PORT:-8795}
PROFILE=${PROFILE:-websplat-harness-profile}
DIR=$(cd "$(dirname "$0")" && pwd)
SOCK="$HOME/.ssh/websplat-win11-tunnel-$PROFILE.sock"
ssh_q() { ssh -o LogLevel=ERROR "$HOST" "$@"; }
case "$1" in
start)
  ssh_q 'mkdir -p /tmp/websplat-harness'
  for f in start.ps1 stop.ps1; do ssh_q "cat > /tmp/websplat-harness/$f" < "$DIR/$f"; done
  ssh_q "powershell -NoProfile -ExecutionPolicy Bypass -File \$(cygpath -w /tmp/websplat-harness/start.ps1) -port $CDP_PORT -name $PROFILE $EXTRA"
  ssh -o LogLevel=ERROR -o ExitOnForwardFailure=yes -f -N -M -S "$SOCK" \
    -L "$CDP_PORT:127.0.0.1:$CDP_PORT" -R "$APP_PORT:127.0.0.1:$APP_PORT" "$HOST"
  for i in $(seq 20); do
    curl -s "http://127.0.0.1:$CDP_PORT/json/version" >/dev/null && { echo "CDP up on 127.0.0.1:$CDP_PORT"; exit 0; }
    sleep 1
  done
  echo "CDP did not come up" >&2; exit 1
  ;;
stop)
  ssh -S "$SOCK" -O exit "$HOST" 2>/dev/null || true
  ssh_q 'powershell -NoProfile -ExecutionPolicy Bypass -File $(cygpath -w /tmp/websplat-harness/stop.ps1) -name '"$PROFILE"
  ;;
status)
  curl -s "http://127.0.0.1:$CDP_PORT/json/version" || echo "no CDP on $CDP_PORT"
  ;;
*) echo "usage: $0 start|stop|status" >&2; exit 2 ;;
esac
