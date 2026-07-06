#!/bin/bash

# 交叉编译脚本 for aarch64 (ARM 64位)
# 使用 aarch64-linux-gnu-gcc 静态编译，直接在板卡上运行

# 编译器
CC="aarch64-linux-gnu-gcc"

# 编译选项 - 静态链接，避免板卡缺库
CFLAGS="-Wall -Wextra -O2 -g -static"

echo "=========================================="
echo "交叉编译 doip_server + test_appA/B 到 aarch64"
echo "编译器: $CC"
echo "CFLAGS: $CFLAGS"
echo "=========================================="

# 清理旧编译
echo "[1/3] 清理旧编译产物..."
make clean
if [ $? -ne 0 ]; then
    echo "clean 失败！"
    exit 1
fi

# 开始编译
echo "[2/3] 开始编译..."
make CC="$CC" CFLAGS="$CFLAGS"
if [ $? -ne 0 ]; then
    echo "编译失败！"
    exit 1
fi

# 检查结果
echo "[3/3] 检查编译结果..."
ALL_OK=1
for f in doip_server test_appA test_appB; do
    if [ -f "$f" ]; then
        echo "  [OK] $f"
        file "$f"
    else
        echo "  [FAIL] $f 未生成！"
        ALL_OK=0
    fi
done

if [ "$ALL_OK" -eq 1 ]; then
    echo ""
    echo "=========================================="
    echo "编译成功！"
    echo "------------------------------------------"
    echo "测试 OTA 流程："
    echo "  1. 先把 test_appA 部署到 slot_a"
    echo "  2. 板卡上运行，看到 [appA] 我叫 AppA"
    echo "  3. 通过 DoIP 下发 test_appB 做 OTA 升级"
    echo "  4. 升级后运行，看到 [appB] 我叫 AppB"
    echo "     → 说明 OTA 成功！"
    echo "=========================================="
else
    echo "编译失败！"
    exit 1
fi