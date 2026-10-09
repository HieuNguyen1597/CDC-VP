#!/usr/bin/env bash
# Interactive console for a fx1_soc started with --uart0-socket PORT.
#
#   platforms/fx1_soc/tools/fx1_console.sh [PORT]      (default 5000)
#
# The firmware echoes what it receives, like a real UART console, so the
# terminal must not echo locally (plain `nc` or `telnet` in line mode shows
# every command twice and sends whole lines). This puts the terminal input in
# raw mode: keys go to the CLI one by one, backspace works, nothing is doubled.
# Output is passed through untouched: the firmware ends lines with CR LF, as a
# real serial console does (adding onlcr here would print CR CR LF).
#
# Client: socat when installed, else bash's own /dev/tcp, which needs nothing
# installed (a bare almalinux:9 image). FX1_CONSOLE_CLIENT=socat|bash|nc
# forces one.
#   socat  quit with Ctrl-]
#   bash   quit with `poweroff` in the CLI (the client ends with the VP)
#   nc     quit with `poweroff`, then Ctrl-C: ncat keeps waiting on its input
#          after the VP closes the connection, so it is never the default
# The VP listens on 127.0.0.1 only, so the client must run on the same host,
# or in the same container (docker exec), as the simulator.
set -u
port="${1:-5000}"
export PATH="/usr/bin:/bin:${PATH}"
client="${FX1_CONSOLE_CLIENT:-}"
if [[ -z "${client}" ]]; then
    if command -v socat > /dev/null 2>&1; then client=socat; else client=bash; fi
fi
case "${client}" in
    socat) echo "fx1 console on 127.0.0.1:${port} (Ctrl-] to quit)" ;;
    bash) echo "fx1 console on 127.0.0.1:${port} (end with 'poweroff' in the CLI)" ;;
    nc) echo "fx1 console on 127.0.0.1:${port} (end with 'poweroff', then Ctrl-C)" ;;
    *) echo "FX1_CONSOLE_CLIENT must be socat, nc or bash" >&2; exit 2 ;;
esac
if [[ -t 0 ]]; then
    saved="$(stty -g)"
    trap 'stty "${saved}"' EXIT
    stty raw -echo
fi
case "${client}" in
    socat) socat -,escape=0x1d "TCP:127.0.0.1:${port}" ;;
    nc) nc 127.0.0.1 "${port}" ;;
    bash)
        exec 3<> "/dev/tcp/127.0.0.1/${port}" || exit 1
        cat <&3 &          # socket -> terminal; ends when the VP closes the connection
        reader=$!
        cat <&0 >&3 &      # terminal -> socket (explicit <&0: an async command gets /dev/null otherwise)
        writer=$!
        wait "${reader}"
        kill "${writer}" 2> /dev/null
        exec 3>&-
        ;;
esac
