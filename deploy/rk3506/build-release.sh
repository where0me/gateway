#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
TOOLCHAIN_DIR=${RK3506_TOOLCHAIN_DIR:-/home/alientek/opt/rk3506-toolchain}
ENVIRONMENT_SETUP="$TOOLCHAIN_DIR/environment-setup"
OUTPUT_ROOT="$REPO_ROOT/out/rk3506"
PACKAGE_NAME=zigbee-gateway
PACKAGE_ROOT="$OUTPUT_ROOT/$PACKAGE_NAME"
ARCHIVE_PATH="$OUTPUT_ROOT/zigbee-gateway-rk3506-armv7.tar.gz"

if [ ! -f "$ENVIRONMENT_SETUP" ]; then
    echo "找不到RK3506工具链环境脚本: $ENVIRONMENT_SETUP" >&2
    exit 1
fi

# 官方环境脚本提供ARM ABI、sysroot、编译器和SQLite库搜索路径。
. "$ENVIRONMENT_SETUP" >/dev/null

rm -rf "$PACKAGE_ROOT"
mkdir -p "$PACKAGE_ROOT/data" "$PACKAGE_ROOT/web" "$PACKAGE_ROOT/install"

make -B -C "$REPO_ROOT/gateway-core"

MACHINE=$($READELF -h "$REPO_ROOT/gateway-core/gateway-server" | awk -F: '/Machine:/{gsub(/^[[:space:]]+/, "", $2); print $2}')
if [ "$MACHINE" != "ARM" ]; then
    echo "后端架构校验失败，实际Machine为: $MACHINE" >&2
    exit 1
fi

cd "$REPO_ROOT/gateway-ui"
npm run lint
npm run build

cp "$REPO_ROOT/gateway-core/gateway-server" "$PACKAGE_ROOT/gateway-server"
cp "$SCRIPT_DIR/gateway_config.json" "$PACKAGE_ROOT/gateway_config.json"
cp "$SCRIPT_DIR/S89zigbee-network" "$PACKAGE_ROOT/install/S89zigbee-network"
cp "$SCRIPT_DIR/S90zigbee-gateway" "$PACKAGE_ROOT/install/S90zigbee-gateway"
cp "$SCRIPT_DIR/S99appinit" "$PACKAGE_ROOT/install/S99appinit"
cp "$SCRIPT_DIR/README.md" "$PACKAGE_ROOT/install/README.md"
cp -R "$REPO_ROOT/gateway-ui/dist/." "$PACKAGE_ROOT/web/"
chmod 755 "$PACKAGE_ROOT/gateway-server" \
    "$PACKAGE_ROOT/install/S89zigbee-network" \
    "$PACKAGE_ROOT/install/S90zigbee-gateway" \
    "$PACKAGE_ROOT/install/S99appinit"
$STRIP --strip-unneeded "$PACKAGE_ROOT/gateway-server"

tar -C "$OUTPUT_ROOT" -czf "$ARCHIVE_PATH" "$PACKAGE_NAME"
(
    cd "$OUTPUT_ROOT"
    sha256sum "$(basename "$ARCHIVE_PATH")" > "$(basename "$ARCHIVE_PATH").sha256"
)

echo "RK3506部署包已生成:"
echo "  $ARCHIVE_PATH"
echo "  $ARCHIVE_PATH.sha256"
