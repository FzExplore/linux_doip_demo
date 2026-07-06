<!--
 * @Author: Fang Zheng
 * @Date: 2026-07-06 15:52:05
 * @LastEditors: Do not edit
 * @LastEditTime: 2026-07-06 16:45:40
 * @Description: 
 * @FilePath: \demo\README.md
-->
# linux_doip_demo

基于 Linux 的 DoIP（Diagnostics over IP）诊断服务端，完整实现 **ISO 13400-2**（DoIP）和 **ISO 14229-1**（UDS）协议栈，支持标准 **AB 双分区 FOTA 固件升级**。

该项目用于学习 UDS 诊断和 OTA 升级流程，也可移植到 MCU 上用于实际产品。

---

## 协议标准

| 标准 | 内容 | 实现程度 |
|:---:|------|:---:|
| ISO 13400-2 | DoIP 协议 | ✅ 全实现：车辆识别、路由激活、诊断消息、Alive Check |
| ISO 14229-1 | UDS 诊断服务 | ✅ 所有刷写所需服务：会话、安全访问、下载、例程控制 |
| NIST SP 800-38B | AES-CMAC | ✅ 纯 C 实现，符合标准 |

---

## 功能特性

### 完整 DoIP 服务（ISO 13400-2）

| DoIP 消息 | 实现 | 说明 |
|:---:|:---:|------|
| 车辆识别请求 | ✅ | 广播发现，支持按 VIN/EID 查询 |
| 车辆公告响应 | ✅ | 返回 VIN/逻辑地址/EID/GID |
| 路由激活请求 | ✅ | 0x0005 请求，0x0006 响应 |
| 诊断消息传输 | ✅ | 0x8001 用户数据，支持肯定/否定应答 |
| Alive Check | ✅ | 周期性存活检测，维持连接 |

### UDS 诊断服务（ISO 14229-1）

| 服务 ID | 子功能 | 名称 | 实现 |
|:---:|:---:|------|:---:|
| 0x10 | 0x01/0x02/0x03 | 诊断会话控制 | ✅ 默认/编程/扩展，带 S3 超时 |
| 0x11 | 0x01/0x02 | ECU 复位 | ✅ 硬复位/软复位 |
| 0x14 | - | 清除故障码 | ✅ 按组清除 DTC |
| 0x19 | 0x01/0x02/0x04/0x0A | 读取 DTC 信息 | ✅ 数量/列表/快照 |
| 0x22 | - | 按 ID 读数据 | ✅ DID 读取，持久化存储 |
| 0x27 | 0x01/0x02 | 安全访问 | ✅ 种子+密钥，AES-CMAC 算法 |
| 0x2E | - | 按 ID 写数据 | ✅ DID 写入，持久化存储 |
| 0x31 | 0x01/0x02/0x03 | 例程控制 | ✅ 5 个标准例程 |
| 0x34 | - | 请求下载 | ✅ 建立下载会话 |
| 0x36 | - | 传输数据 | ✅ 逐块写入固件 |
| 0x37 | - | 传输退出 | ✅ 结束下载 |
| 0x3E | 0x00 | Tester Present | ✅ 心跳保活，维持会话 |

### OTA 固件升级（AB 双分区）

| 特性 | 说明 |
|------|------|
| ✅ **AB 双分区** | slot_a / slot_b 交替运行，升级过程不覆盖当前运行固件，断电不会砖 |
| ✅ **暂存区隔离** | 下载过程写入 `staging`，只有全量下载成功才会 Commit 到槽位 |
| ✅ **原子操作** | 元数据和固件文件都采用"先写临时文件再 rename"，防止写入中断电损坏 |
| ✅ **状态机** | `INACTIVE → TESTING → OK / FAILED`，带启动次数限制，启动失败自动回滚 |
| ✅ **CRC8 校验** | 元数据完整性质检，防止元数据损坏导致启动错误 |
| ✅ **中断恢复** | 下载中断会自动清理暂存区，不影响下次升级 |
| ✅ **供应商交付目录** | 最终确认后自动复制到供应商指定路径 |

### 安全访问

| 特性 | 说明 |
|------|------|
| ✅ **AES-128-CMAC** | NIST SP 800-38B 标准，纯 C 实现，**零外部依赖** |
| ✅ **128 位对称密钥** | 密钥 `00112233445566778899AABBCCDDEEFF` 可改 |
| ✅ **种子长度** | 4 字节随机种子，每次请求种子随机生成 |
| ✅ **ARM 硬件加速** | 可切换到 OpenSSL 自动使用 CPU Crypto Extensions |
| ✅ **上位机算法** | Python 实现参考在 `test_ota_app.py`

---

## 目录结构

```
.
├── doip.h              # DoIP 协议栈（ISO 13400-2）
├── doip_server.h/c     # DoIP 服务端（TCP/UDP 监听）
├── doip_main.c         # 程序入口
├── uds.h               # UDS 协议定义（ISO 14229-1）
├── uds.c               # UDS 服务实现（会话/安全/下载/例程/DTC）
├── ota_manager.h/c     # AB 双分区 OTA 管理器
├── dtc_store.h/c       # DTC 故障码存储
├── aes_cmac.h/c        # AES-128-CMAC 纯 C 实现
├── dtc_simulator.c     # DTC 模拟器（生成测试故障码）
├── test_appA.c         # 测试 AppA（打印 "I am AppA"）
├── test_appB.c         # 测试 AppB（打印 "I am AppB"）
├── test_ota_app.py     # Python OTA 测试脚本（含 AES-CMAC）
├── test_doip.py        # Python DoIP 客户端测试
├── test_ota.py         # Python OTA 测试脚本
├── makefile            # 编译配置
├── build_aarch64.sh    # aarch64 交叉编译脚本
├── boot_manager.sh     # 看门狗启动脚本
└── data/ota/           # OTA 数据目录
    ├── slot_a/         # A 槽固件
    ├── slot_b/         # B 槽固件
    ├── staging/        # 暂存区
    ├── vendor/         # 供应商固件目录
    └── ota_status      # 槽位元数据
```

---

## 编译与运行

### 本机编译（Linux / WSL）

```bash
make
```

### 交叉编译（aarch64）

```bash
./build_aarch64.sh
```

### 启动服务

```bash
./doip_server
```

### 看门狗启动（退出后自动重启）

```bash
./boot_manager.sh
```

### 首次部署

```bash
make install    # 将 doip_server 安装到 slot_a
```

---

## OTA 升级流程

```
1. 进入扩展会话          10 03
2. 安全访问解锁          27 01 / 27 02  (AES-CMAC)
3. 检查刷写条件          31 01 FF 01
4. 擦除暂存区            31 01 FF 00
5. 请求下载              34 00 44 ...
6. 传输数据              36 ...  (×N 块)
7. 传输退出              37
8. CRC 校验              31 01 FF 02
9. Commit                31 01 FF 03  (暂存区 → 非活跃槽)
10. Accept               31 01 FF 04  (TESTING → OK)
11. 硬复位               11 01        (重启运行新固件)
```

---

## 测试

### Python 测试

```bash
# 安装依赖
pip install cryptography

# OTA 升级测试
python3 test_ota_app.py

# DoIP 协议测试
python3 test_doip.py
```

### DTC 模拟器

```bash
./dtc_simulator
```

---

## 移植到 MCU

UDS 协议层与 OTA 状态机完全解耦，移植只需修改存储层（约 65 行）：

| 当前实现 | MCU 替换 |
|------|------|
| `fopen / fread / fwrite` | `FLASH_Read / FLASH_Program` |
| `rename` | 写 Flash 标志位 |
| `stat` | `FLASH_GetSectorSize` |
| `remove` | `FLASH_EraseSector` |
| `time(NULL)` | `HAL_GetTick` |
| `rand()` | `HAL_RNG_GetRandomNumber` |
| DoIP 传输层 | CAN / CAN-FD 驱动 |

---

## 依赖

- **编译**：gcc / aarch64-linux-gnu-gcc
- **运行**：Linux 内核 2.6+，无外部库依赖
- **测试**：Python 3.6+，cryptography 库
