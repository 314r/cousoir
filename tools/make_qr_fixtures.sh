#!/usr/bin/env bash
# Writes tests/data/qr/*.txt: QR codes made by qrencode (libqrencode), in
# byte mode at error correction level M, which tests/qr_code_test.cpp
# compares with Cousoir's own, module by module. The first line is the
# text; then a line per row, "#" for a dark module.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/data/qr"
mkdir -p "$out"
long=$(printf 'http://192.168.1.20:8090/opds/books?q=%s' "$(printf 'authors:%%22%%3DTerry%%20Pratchett%%22%%20or%%20%.0s' {1..6})")
texts=(
    "http://192.168.1.20:8090/"
    "http://192.168.178.123:8090/opds"
    "http://[fe80::1c2b:3d4e:5f60:7182]:8090/opds/books?sort=title&title=By%20title"
    "http://192.168.1.20:8090/opds/books?q=tags%3A%22%3DScience%20Fiction%22%20and%20languages%3Afra&sort=title&title=Science%20Fiction%20in%20French"
    "$long"
)
i=0
for text in "${texts[@]}"; do
    i=$((i + 1))
    {
        printf '%s\n' "$text"
        qrencode -8 -l M -m 0 -t ASCII "$text" | sed 's/##/#/g; s/  /./g'
    } > "$out/$i.txt"
done
