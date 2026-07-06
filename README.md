# linux_doip_demo

基于 Linux 的 DoIP（Diagnostics over IP）诊断服务端，实现 ISO 13400-2 / ISO 14229-1 协议栈，支持 **AB 双分区 OTA 固件升级**。

---

## 功能特性

### UDS 诊断服务（ISO 14229-1）

| 服务 ID | 名称 | 说明 |
|:---:|------|------|
| 0x10 | 诊断会话控制 | 默认会话 / 编程会话 / 扩展会话，含 S3 超时 |
| 0x11 | ECU 复位 | 硬复位 / 软复位 |
| 0x14 | 清除故障码 | 支持按组清除 DTC |
| 0x19 | 读取 DTC | 按状态掩码读取 / 快照 / 数量统计 |
| 0x22 | 按 ID 读数据 | DID 读取，含持久化支持 |
| 0x27 | 安全访问 | 种子+密钥，AES-CMAC 算法 |
| 0x2E | 按 ID 写数据 | DID 写入，持久化到非易失存储 |
| 0x31 | 例程控制 | 擦除 / 条件检查 / CRC / Commit / Accept |
| 0x34 | 请求下载 | 建立下载会话 |
| 0x36 | 传输数据 | 逐块传输固件 |
| 0x37 | 传输退出 | 结束下载，校验 |
| 0x3E | Tester Present | 心跳保活 |

### OTA 固件升级

- **AB 双分区**：slot_a / slot_b 交替运行，升级不覆盖当前固件
- **暂存区隔离**：下载过程写入 staging，不污染活跃槽
- **原子操作**：元数据先写 `.tmp` 再 rename，防止断电损坏
- **状态机**：INACTIVE → TESTING → OK / FAILED，含启动次数限制
- **CRC8 校验**：元数据完整性保护

### 安全访问

- **AES-CMAC** 算法（纯 C 实现，零外部依赖）
- 密钥长度 128 位，种子 4 字节
- 支持硬件加速（ARM Crypto Extensions 可用）

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
