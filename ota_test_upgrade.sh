#!/bin/bash

# OTA 升级流程模拟脚本
# 模拟完整的 OTA 流程：校验 → 转移 → 切换槽位

OTA_DIR="data/ota"
STAGING="$OTA_DIR/staging/firmware.bin"
SLOT_A="$OTA_DIR/slot_a/firmware.bin"
SLOT_B="$OTA_DIR/slot_b/firmware.bin"
STATUS="$OTA_DIR/ota_status"

echo "=========================================="
echo "OTA 升级流程模拟"
echo "=========================================="

# 1. 检查 staging 是否有新固件
if [ ! -f "$STAGING" ]; then
    echo "[FAIL] staging 中没有新固件！请先执行 ota_test_init.sh"
    exit 1
fi
echo "[1/4] 发现新固件: $(ls -lh "$STAGING" | awk '{print $5}')"

# 2. 校验固件（模拟：检查 SHA256 或签名）
echo "[2/4] 校验固件..."
STAGING_HASH=$(sha256sum "$STAGING" | awk '{print $1}')
echo "       SHA256: $STAGING_HASH"
echo "       [OK] 校验通过"

# 3. 确定目标槽位：当前活跃是 A，则写入 B
CURRENT=$(grep active_slot "$STATUS" 2>/dev/null | awk -F= '{print $2}')
if [ "$CURRENT" = "A" ]; then
    TARGET_SLOT="B"
    TARGET_PATH="$SLOT_B"
elif [ "$CURRENT" = "B" ]; then
    TARGET_SLOT="A"
    TARGET_PATH="$SLOT_A"
else
    echo "[FAIL] 无法确定当前活跃槽位！"
    exit 1
fi
echo "       当前活跃槽: $CURRENT → 目标槽: $TARGET_SLOT"

# 4. 转移固件到目标槽
echo "[3/4] 转移固件到 slot_$TARGET_SLOT..."
cp "$STAGING" "$TARGET_PATH"
chmod +x "$TARGET_PATH"
rm -f "$STAGING"
echo "       [OK] 固件已写入 slot_$TARGET_SLOT"

# 5. 更新 ota_status，切换活跃槽
echo "[4/4] 更新 ota_status..."
echo "active_slot=$TARGET_SLOT" > "$STATUS"
echo "ota_state=done"        >> "$STATUS"
echo "       [OK] 活跃槽切换: $CURRENT → $TARGET_SLOT"

echo ""
echo "=========================================="
echo "OTA 升级完成！"
echo "------------------------------------------"
echo "旧固件 (slot_$CURRENT): test_appA → [appA] 我叫 AppA"
echo "新固件 (slot_$TARGET_SLOT): test_appB → [appB] 我叫 AppB"
echo "------------------------------------------"
echo "验证方式："
echo "  ./$TARGET_PATH"
echo "  → 应输出 [appB] 我叫 AppB"
echo "=========================================="