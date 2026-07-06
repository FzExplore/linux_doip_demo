#!/bin/bash
###
 # @Author: Fang Zheng
 # @Date: 2026-07-03 13:00:24
 # @LastEditors: Do not edit
 # @LastEditTime: 2026-07-03 13:00:37
 # @Description: 
 # @FilePath: \demo\ota_test_init.sh
### 

# OTA 测试初始化脚本
# 将 test_appA 部署到 slot_a 作为初始固件
# 将 test_appB 放到 staging 备用，模拟"已经下载完毕待校验"

echo "=========================================="
echo "OTA 测试环境初始化"
echo "=========================================="

# 清空旧数据
rm -rf data/ota
mkdir -p data/ota/slot_a data/ota/slot_b data/ota/staging data/ota/vendor

# 1. 初始固件 = test_appA，放入 slot_a
cp test_appA data/ota/slot_a/firmware.bin
chmod +x data/ota/slot_a/firmware.bin
echo "[OK] slot_a 部署 test_appA（旧固件）"

# 2. 出厂备份也用 test_appA
cp test_appA data/ota/vendor/firmware.bin
chmod +x data/ota/vendor/firmware.bin
echo "[OK] vendor 出厂备份 = test_appA"

# 3. 模拟 OTA 下载：test_appB 已下载到 staging，等待校验
cp test_appB data/ota/staging/firmware.bin
chmod +x data/ota/staging/firmware.bin
echo "[OK] staging 暂存 test_appB（新固件，待校验）"

# 4. 写入 ota_status：当前活跃槽 = A
echo "active_slot=A"  > data/ota/ota_status
echo "ota_state=idle" >> data/ota/ota_status
echo "[OK] ota_status 写入: active_slot=A"

echo ""
echo "=========================================="
echo "初始化完成！"
echo "------------------------------------------"
echo "当前状态："
echo "  slot_a  → test_appA（运行中）"
echo "  staging → test_appB（已下载，待校验）"
echo "  slot_b  → 空"
echo "  vendor  → test_appA（出厂备份）"
echo "------------------------------------------"
echo "测试步骤："
echo "  1. 运行当前固件:  ./data/ota/slot_a/firmware.bin"
echo "     → 应输出 [appA] 我叫 AppA"
echo "  2. 执行 OTA 升级:  ./ota_test_upgrade.sh"
echo "     → 校验 staging → 写入 slot_b → 更新 ota_status"
echo "  3. 运行新固件:    ./data/ota/slot_b/firmware.bin"
echo "     → 应输出 [appB] 我叫 AppB（OTA 成功！）"
echo "=========================================="