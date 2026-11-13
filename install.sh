#!/bin/bash
set -e

GATEWAY_DIR="$(pwd)"
BUILD_DEPS="/tmp/knx_gateway_deps"
mkdir -p "$BUILD_DEPS"

echo "=== 1. Namestitev sistemskih odvisnosti ==="
sudo apt-get update
sudo apt-get install -y build-essential cmake git pkg-config \
  libssl-dev libmbedtls-dev libjson-c-dev libcrypt-dev \
  automake autoconf libtool libsystemd-dev libev-dev

echo "=== 2. Prevajanje in namestitev open62541 (v1.4 z enkripcijo) ==="
cd "$BUILD_DEPS"
if [ ! -d "open62541" ]; then
  git clone --branch v1.4.6 --depth 1 https://github.com
fi
mkdir -p open62541/build && cd open62541/build
cmake -DUA_ENABLE_ENCRYPTION=MBEDTLS -DBUILD_SHARED_LIBS=ON ..
make -j$(nproc)
sudo make install
sudo ldconfig

echo "=== 3. Prevajanje knxd in priprava vendor/eibclient za ARM ==="
cd "$BUILD_DEPS"
if [ ! -d "knxd" ]; then
  git clone --depth 1 https://github.com
fi
cd knxd
./bootstrap.sh
./configure --enable-systemd --enable-usb --enable-eibnetip --enable-eibnetiptunnel --enable-eibnetipserver
make -j$(nproc)
sudo make install

# Kopiranje ARM glave in statične knjižnice v vaš projekt
mkdir -p "$GATEWAY_DIR/vendor/eibclient/include" "$GATEWAY_DIR/vendor/eibclient/lib"
cp src/include/eibclient.h "$GATEWAY_DIR/vendor/eibclient/include/"
cp src/include/eibtypes.h "$GATEWAY_DIR/vendor/eibclient/include/"
cp src/client/c/.libs/libeibclient.a "$GATEWAY_DIR/vendor/eibclient/lib/"
# eibloadresult.h je v starejših/novejših verzijah v src/include ali pod src/client
find . -name "eibloadresult.h" -exec cp {} "$GATEWAY_DIR/vendor/eibclient/include/" \;

echo "=== 4. Konfiguracija knxd.ini ==="
cd "$GATEWAY_DIR"
sudo cp config/knxd.ini /etc/knxd.ini
sudo systemctl daemon-reload
sudo systemctl enable --now knxd.socket knxd-net.socket

echo "=== 5. Prevajanje knx_opcua_gateway ==="
make clean && make

echo "=== KONČANO! ==="
echo "Zdaj generirajte certifikate (tools/make_pki.sh) in uredite config.json."
