#!/bin/bash
# Ustvari samopodpisan strezniski certifikat za OPC UA gateway (DER) in mapo za zaupane certifikate odjemalcev.
# Uporaba: tools/make_pki.sh [ime_gostitelja] [IP] [application_uri]
#   ime_gostitelja  privzeto: $(hostname)
#   IP              neobvezno (npr. 10.80.32.65), se doda v certifikat
#   application_uri privzeto: urn:knx-opcua-gateway  (mora biti enak "pki"."application_uri" v config.json)
set -euo pipefail

HOST="${1:-$(hostname)}"
IP="${2:-}"
APPURI="${3:-urn:knx-opcua-gateway}"
OUT="${PKI_DIR:-pki}"

mkdir -p "$OUT/trusted"
if [[ -e "$OUT/server_cert.der" || -e "$OUT/server_key.der" ]]; then
    echo "V $OUT ze obstaja strezniski certifikat ali kljuc - ne prepisujem. Izbrisi ju, ce zelis nova." >&2
    exit 1
fi

SAN="URI:${APPURI},DNS:${HOST}"
[[ -n "$IP" ]] && SAN="${SAN},IP:${IP}"

umask 077
openssl req -x509 -newkey rsa:2048 -sha256 -days 36500 -nodes \
    -keyout "$OUT/server_key.pem" -out "$OUT/server_cert.pem" \
    -subj "/CN=KNX OPC UA Gateway/O=KNX-OPCUA" \
    -addext "subjectAltName=${SAN}" \
    -addext "basicConstraints=CA:FALSE" \
    -addext "keyUsage=digitalSignature,nonRepudiation,keyEncipherment,dataEncipherment,keyCertSign" \
    -addext "extendedKeyUsage=serverAuth,clientAuth"

openssl x509 -in "$OUT/server_cert.pem" -outform der -out "$OUT/server_cert.der"
openssl rsa  -in "$OUT/server_key.pem"  -outform der -out "$OUT/server_key.der" 2>/dev/null
rm -f "$OUT/server_key.pem"          # ostane samo DER kljuc (chmod 600)
chmod 644 "$OUT/server_cert.der" "$OUT/server_cert.pem"
chmod 600 "$OUT/server_key.der"

echo "Ustvarjeno:"
echo "  $OUT/server_cert.der   (certifikat, veljavnost 10 let)"
echo "  $OUT/server_key.der    (zasebni kljuc - ne kopiraj nikamor)"
echo "  $OUT/trusted/          (sem dodaj certifikate odjemalcev: tools/trust_client.sh)"
echo "SAN: ${SAN}"
