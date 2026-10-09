#!/usr/bin/env bash
# Interactive console for a fx1_soc started with --uart0-socket PORT.
#
#   platforms/fx1_soc/tools/fx1_console.sh [PORT]      (default 5000)
#
# The firmware echoes what it receives, like a real UART console, so the
# terminal must not echo locally (plain `nc` or `telnet` in line mode shows
# every command twice and sends whole lines). This puts the terminal input in
# raw mode: keys go to the CLI one by one, backspace works, nothing is doubled.
# Output processing stays on (opost onlcr): the firmware ends lines with a
# bare '\n', which a fully raw terminal shows as a staircase.
#   socat (preferred): quit with Ctrl-]
#   fallback nc:       quit with `poweroff` in the CLI, or kill the simulator
set -u
port="${1:-5000}"
export PATH="/usr/bin:/bin:${PATH}"
if command -v socat >/dev/null 2>&1; then
    client=(socat -,escape=0x1d "TCP:127.0.0.1:${port}")
    echo "fx1 console on 127.0.0.1:${port} (Ctrl-] to quit)"
elif command -v nc >/dev/null 2>&1; then
    client=(nc 127.0.0.1 "${port}")
    echo "fx1 console on 127.0.0.1:${port} (end with 'poweroff' in the CLI)"
else
    echo "need socat or nc" >&2
    exit 1
fi
if [ -t 0 ]; then
    saved="$(stty -g)"
    trap 'stty "${saved}"' EXIT
    stty raw -echo opost onlcr
fi
"${client[@]}"
