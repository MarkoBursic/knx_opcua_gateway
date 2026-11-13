#!/bin/bash
set -e

# Mape in imena datotek
OUT_DIR="pki_client"
mkdir -p "$OUT_DIR"

TMP_CNF=$(mktemp)
trap 'rm -f "$TMP_CNF"' EXIT

# Zapis OpenSSL konfiguracije v začasno datoteko
cat << 'EOF' > "$TMP_CNF"
[ req ]
default_bits = 2048
default_md = sha256
distinguished_name = subject
req_extensions = req_ext
x509_extensions = req_ext
string_mask = utf8only
prompt = no

[ subject ]
commonName = open62541Client

[ req_ext ]
basicConstraints = CA:FALSE
keyUsage = digitalSignature, nonRepudiation, keyEncipherment, dataEncipherment
extendedKeyUsage = clientAuth
subjectAltName = @alt_names

[ alt_names ]
URI.1 = urn:open62541:client
EOF

# 1. Generiranje PEM certifikata in zasebnega ključa (veljavnost 365 dni)
openssl req -x509 -nodes -days 365 -newkey rsa:2048 \
  -keyout "$OUT_DIR/client_key.pem" \
  -out "$OUT_DIR/client_cert.pem" \
  -config "$TMP_CNF"

# 2. Pretvorba v DER format za OPC UA (open62541)
openssl x509 -outform der -in "$OUT_DIR/client_cert.pem" -out "$OUT_DIR/client_cert.der"
openssl rsa -outform der -in "$OUT_DIR/client_key.pem" -out "$OUT_DIR/client_key.der" 2>/dev/null

echo "Uspešno ustvarjeno v mapi $OUT_DIR/:"
echo "  - client_cert.pem / client_cert.der (javni certifikat odjemalca)"
echo "  - client_key.pem  / client_key.der  (zasebni ključ odjemalca)"
