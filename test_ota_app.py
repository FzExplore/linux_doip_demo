#!/usr/bin/env python3
"""
DoIP OTA 刷写测试 — 发送 test_appB 到板卡
真实流程：连接 → 路由激活 → 编程会话 → 安全访问 → 刷写 → Commit
"""
import socket, struct, os, time, sys
from cryptography.hazmat.primitives.cmac import CMAC
from cryptography.hazmat.primitives.ciphers import algorithms

# ============================================================
# 配置：改成你的板卡 IP
# ============================================================
HOST = '127.0.0.1'       # ← 改成板卡 IP，比如 '192.168.1.xxx'
PORT = 13400
TESTER_ADDR = 0x0E00
ECU_ADDR    = 0x0E80

# 0x27 安全访问 — AES-128 密钥（与 ECU 端一致）
SECRET_KEY = bytes.fromhex("00112233445566778899AABBCCDDEEFF")

# 要发送的固件文件
FW_FILE = './test_appB'

# ============================================================
# DoIP 工具函数
# ============================================================
def recv_doip(sock):
    hdr = sock.recv(8, socket.MSG_WAITALL)
    if len(hdr) < 8: return None, None
    ptype = struct.unpack('>H', hdr[2:4])[0]
    plen  = struct.unpack('>I', hdr[4:8])[0]
    payload = sock.recv(plen, socket.MSG_WAITALL) if plen > 0 else b''
    return ptype, payload

def send_doip(sock, ptype, payload):
    sock.sendall(b'\x02\xfd' + struct.pack('>H', ptype) + struct.pack('>I', len(payload)) + payload)

def send_diag(sock, uds):
    send_doip(sock, 0x8001, struct.pack('>H', TESTER_ADDR) + struct.pack('>H', ECU_ADDR) + uds)

def recv_diag(sock):
    ptype, payload = recv_doip(sock)
    return payload[4:] if ptype == 0x8001 else None

def ok(uds): return uds is not None and len(uds) >= 1 and uds[0] != 0x7F

def step(name, uds):
    s = "OK" if ok(uds) else "FAIL"
    print(f"  [{s}] {name}: {uds.hex() if uds else '(空)'}")
    return ok(uds)

# ============================================================
# 主流程
# ============================================================
print("=" * 60)
print("  DoIP OTA 刷写测试 — 发送 test_appB")
print(f"  目标: {HOST}:{PORT}")
print("=" * 60)

# 1. 检查固件文件
if not os.path.exists(FW_FILE):
    print(f"FAIL: 找不到固件文件 {FW_FILE}")
    print(f"提示: 请先在 Linux 虚拟机上运行 ./build_aarch64.sh")
    sys.exit(1)

firmware = open(FW_FILE, "rb").read()
print(f"\n固件: {FW_FILE} ({len(firmware)} 字节)")

# 2. 连接
sock = socket.socket()
sock.connect((HOST, PORT))
print(f"[1] 连接 {HOST}:{PORT} OK")

# 3. 路由激活
send_doip(sock, 0x0005, struct.pack('>H', TESTER_ADDR) + b'\x00\x00')
ptype, _ = recv_doip(sock)
print(f"[2] 路由激活: {'OK' if ptype == 0x0006 else 'FAIL'}")

# 4. 读取当前软件版本 (DID 0xF192)
print("\n--- 读取当前状态 ---")
send_diag(sock, b'\x22\xF1\x92')
step("软件版本(DID 0xF192)", recv_diag(sock))

# 5. 进入编程会话 + 安全访问
print("\n--- 编程会话 + 安全访问 ---")
send_diag(sock, b'\x10\x03')
if not step("10 03 编程会话", recv_diag(sock)):
    print("  提示: 需要先进入扩展会话(10 02)再进编程会话(10 03)")
    sock.close(); sys.exit(1)

send_diag(sock, b'\x27\x01')
uds = recv_diag(sock)
if not ok(uds):
    print("FAIL: 安全访问请求种子失败")
    sock.close(); sys.exit(1)
seed = uds[2:6]
c = CMAC(algorithms.AES(SECRET_KEY))
c.update(seed)
key = c.finalize()[:4]  # AES-CMAC 取前4字节
send_diag(sock, b'\x27\x02' + key)
step("27 安全访问解锁", recv_diag(sock))

# 6. 刷写准备
print("\n--- 刷写准备 ---")
send_diag(sock, b'\x31\x01\xFF\x01')
step("检查刷写条件", recv_diag(sock))
send_diag(sock, b'\x31\x01\xFF\x00')
step("擦除暂存区(2MB)", recv_diag(sock))

# 7. 请求下载
print(f"\n--- 下载固件 {len(firmware)} 字节 ---")
addr = 0x08000000
send_diag(sock, struct.pack('>BBBI', 0x34, 0x00, 0x44, addr) + struct.pack('>I', len(firmware)))
if not step("34 请求下载", recv_diag(sock)):
    sock.close(); sys.exit(1)

# 8. 逐块传输
total = 0
seq = 1
failed = False
while total < len(firmware):
    blen = min(4096, len(firmware) - total)
    send_diag(sock, bytes([0x36, seq]) + firmware[total:total + blen])
    if not step(f"36 块#{seq}", recv_diag(sock)):
        failed = True
        break
    total += blen
    seq += 1
    if seq % 10 == 0:
        print(f"  进度: {total}/{len(firmware)} ({total * 100 // len(firmware)}%)")

if failed:
    print(f"\nFAIL: 传输中断 ({total}/{len(firmware)} 字节)")
    sock.close(); sys.exit(1)

# 9. 传输完成
print(f"\n--- 传输完成 ({total} 字节) ---")
send_diag(sock, b'\x37')
step("37 传输退出", recv_diag(sock))

# 10. CRC 校验
send_diag(sock, b'\x31\x01\xFF\x02')
step("CRC校验", recv_diag(sock))

# 11. Commit — 暂存区 → 非活跃槽，切换 active_slot
print("\n--- Commit 固件到非活跃槽 ---")
send_diag(sock, b'\x31\x01\xFF\x03')
step("Commit 写入槽位", recv_diag(sock))

# 12. 不发送 0x11 01 复位（保持 doip_server 在线）
#    手动验证 OTA 结果

print("\n" + "=" * 60)
print("  OTA 刷写完成！")
print("=" * 60)
print()
print("验证方式（在板卡上执行）：")
print("  1. 查看 OTA 状态: ls -la data/ota/")
print("  2. 查看活跃槽位: cat data/ota/ota_status")
print("  3. 运行新固件验证:")
print("     chmod +x data/ota/slot_b/firmware.bin")
print("     ./data/ota/slot_b/firmware.bin")
print("     → 应输出 [appB] 我叫 AppB")
print("     → 说明 OTA 成功！")
print()

sock.close()
print("测试结束")