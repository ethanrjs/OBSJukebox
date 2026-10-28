#!/usr/bin/env bash
# Generates the updater test fixtures in the given folder. The key is a throwaway test key that only
# QA builds and a test release trust; it is never committed and never signs a release. The folder is
# git-ignored, so each machine and each CI run makes its own.
set -euo pipefail
out="${1:?usage: make-update-fixtures.sh folder}"
mkdir -p "$out"
openssl ecparam -name prime256v1 -genkey -noout -out "$out/test-key.pem"
openssl ec -in "$out/test-key.pem" -pubout -outform DER 2>/dev/null | tail -c 65 | xxd -p -c 65 > "$out/test-public.hex"
a=$(printf 'a%.0s' {1..64}); b=$(printf 'b%.0s' {1..64})
printf '{"files":{"macos-arm64":{"sha256":"%s","size":2},"windows":{"sha256":"%s","size":1}},"schema":1,"version":"9.8.7"}\n' "$a" "$b" > "$out/update.json"
need_plain=1 need_r=1 need_s=1
while (( need_plain || need_r || need_s )); do
    openssl dgst -sha256 -sign "$out/test-key.pem" -out "$out/candidate.sig" "$out/update.json"
    hex=$(xxd -p -c 200 "$out/candidate.sig")
    r_length=$((16#${hex:6:2}))
    r_pad=$([[ ${hex:8:2} == 00 ]] && echo 1 || echo 0)
    s_pad=$([[ ${hex:$(( (6 + r_length) * 2 )):2} == 00 ]] && echo 1 || echo 0)
    if (( !r_pad && !s_pad && need_plain )); then mv "$out/candidate.sig" "$out/plain.sig"; need_plain=0
    elif (( r_pad && !s_pad && need_r )); then mv "$out/candidate.sig" "$out/padded-r.sig"; need_r=0
    elif (( !r_pad && s_pad && need_s )); then mv "$out/candidate.sig" "$out/padded-s.sig"; need_s=0
    fi
done
rm -f "$out/candidate.sig"
