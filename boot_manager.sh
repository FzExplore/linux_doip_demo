#! /bin/bash
# ============================================================
#  Boot Manager — AB 分区启动管理器
# ============================================================

BASE_DIR="$(cd "$(dirname "$0")" && pwd)"
STATUS_FILE="$BASE_DIR/data/ota/ota_status"
SLOT_A="$BASE_DIR/data/ota/slot_a/firmware.bin"
SLOT_B="$BASE_DIR/data/ota/slot_b/firmware.bin"
PORT=13400

echo "[BOOT] AB分区启动管理器，PID=$$"

parse_status() {
    if [ ! -f "$STATUS_FILE" ]; then
        echo "[BOOT] ota_status 不存在，默认从 A 启动"
        echo "a"
        return
    fi
    SLOT=$(dd if="$STATUS_FILE" bs=1 count=1 2>/dev/null)
    echo "${SLOT:-a}"
}

while true; do
    if ss -tlnp 2>/dev/null | grep -q ":$PORT "; then
        echo "[BOOT] 端口 $PORT 已占用，杀掉旧进程"
        fuser -k ${PORT}/tcp 2>/dev/null
        sleep 1
    fi

    ACTIVE=$(parse_status)

    if [ "$ACTIVE" = "b" ] || [ "$ACTIVE" = "B" ]; then
        TARGET="$SLOT_B"
        echo "[BOOT] 选择 slot_b: $TARGET"
    else
        TARGET="$SLOT_A"
        echo "[BOOT] 选择 slot_a: $TARGET"
    fi

    if [ ! -f "$TARGET" ]; then
        echo "[BOOT] 槽位固件不存在，回退到当前目录"
        TARGET="$BASE_DIR/doip_server"
    fi
    if [ ! -x "$TARGET" ]; then
        chmod +x "$TARGET"
    fi

    echo "[BOOT] ===================================================="
    echo "[BOOT] 启动 doip_server: $TARGET"
    echo "[BOOT] ===================================================="

    "$TARGET"
    echo "[BOOT] doip_server 退出，2秒后重启..."
    sleep 2
done