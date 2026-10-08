#!/bin/bash
# Skate.live Server: runs the bundled server on this Mac's Tailscale IP, so friends
# on the same tailnet join with Join + this IP. Closing the window stops it.
HERE="$(cd "$(dirname "$0")" && pwd)"
SUPPORT="$HOME/Library/Application Support/Skate3 LAN"
mkdir -p "$SUPPORT"
printf '\033]0;Skate.live Server\007'

# Tailscale's CLI if it is there, else any interface address in its 100.64.0.0/10 range.
IP=""
for cli in tailscale /Applications/Tailscale.app/Contents/MacOS/Tailscale; do
  IP="$("$cli" ip -4 2>/dev/null | head -1)" && [ -n "$IP" ] && break
done
if [ -z "$IP" ]; then
  IP="$(ifconfig | awk '$1 == "inet" { split($2, o, "."); if (o[1] == 100 && o[2] >= 64 && o[2] < 128) { print $2; exit } }')"
fi
if [ -z "$IP" ]; then
  echo "No Tailscale IP found. Start Tailscale, sign in and open this app again."
  read -r -p "Press Return to close."
  exit 1
fi

PYTHON=""
for p in /opt/homebrew/bin/python3 /usr/local/bin/python3 /usr/bin/python3; do
  [ -x "$p" ] && PYTHON="$p" && break
done

# Only one server can hold the ports (the launcher's Host mode starts one too).
pkill -f skate3_lan_server.py 2>/dev/null && sleep 1

echo "Skate.live Server on $IP"
echo "Friends: open Skate.live, pick Join, type $IP."
echo "You: the same, Join with $IP. Close this window to stop the server."
echo
exec "$PYTHON" -u "$HERE/skate3_lan_server.py" --ip "$IP" --name "$(scutil --get ComputerName)" \
  --log "$SUPPORT/server.log" --players "$SUPPORT/players.json"
