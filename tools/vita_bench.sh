#!/bin/sh
# Runs the bench (tests/bench) on a PS Vita over Wi-Fi and prints its report.
# Needs vitacompanion on the Vita (FTP on port 1337, commands on 1338) and the
# app installed once from its VPK (platform/vita/README.md).
#
#   tools/vita_bench.sh VITA_IP [VITA_BUILD [MAC_BUILD]]   (build-vita, build)
set -eu
ip=${1:?usage: $0 VITA_IP [VITA_BUILD [MAC_BUILD]]}
build=${2:-build-vita}
host=${3:-build}
title=SCIM00001
ftp="ftp://$ip:1337"
dir=ux0:/data/scimitar-bench

echo "quit all" | nc -w 2 "$ip" 1338 >/dev/null || true
sleep 2
curl -sS --ftp-method nocwd -T "$build/eboot.bin" "$ftp/ux0:/app/$title/eboot.bin"
curl -sS --ftp-method nocwd -T "$host/bench/kernels.exe" "$ftp/ux0:/app/$title/kernels.exe"
curl -sS --ftp-method nocwd -Q "DELE $dir/results.txt" "$ftp/" >/dev/null 2>&1 || true
echo "launch $title" | nc -w 2 "$ip" 1338 >/dev/null
echo "launched $title on $ip; waiting for the report"
seen=0
for _ in $(seq 1 120); do
    sleep 5
    progress=$(curl -s --ftp-method nocwd "$ftp/$dir/progress.txt" || true)
    lines=$(printf '%s' "$progress" | grep -c '' || true)
    if [ "$lines" -gt "$seen" ]; then
        printf '%s\n' "$progress" | tail -n +"$((seen + 1))"
        seen=$lines
    fi
    if curl -s --ftp-method nocwd "$ftp/$dir/results.txt" 2>/dev/null | grep -q '^RESULT'; then
        exit 0
    fi
done
echo "no report after 10 minutes" >&2
exit 1
