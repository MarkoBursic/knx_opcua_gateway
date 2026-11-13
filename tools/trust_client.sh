#!/bin/bash
# Doda certifikat odjemalca (PEM ali DER) med zaupane: tools/trust_client.sh uaexpert.der
# Izpise subject in prstni odtis - preveri ju, preden potrdis zaupanje.
set -euo pipefail

SRC="${1:?uporaba: tools/trust_client.sh <certifikat.pem|.der>}"
OUT="${PKI_DIR:-pki}/trusted"
mkdir -p "$OUT"
NAME="$(basename "${SRC%.*}").der"

if head -c 64 "$SRC" | grep -q -- "-----BEGIN"; then
    openssl x509 -in "$SRC" -outform der -out "$OUT/$NAME"
else
    openssl x509 -inform der -in "$SRC" -outform der -out "$OUT/$NAME"
fi
chmod 644 "$OUT/$NAME"

echo "Zaupan: $OUT/$NAME"
openssl x509 -inform der -in "$OUT/$NAME" -noout -subject -fingerprint -sha256
echo "Streznik prebere mapo ob zagonu - ponovno ga zazeni."
