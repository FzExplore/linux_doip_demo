#!/usr/bin/env python3
import socket, struct, os, time

HOST, PORT = '127.0.0.1', 13400
TESTER_ADDR, ECU_ADDR = 0x0E00, 0x0E80

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
    s = "✅" if ok(uds) else "❌"
    print(f"  {s} {name}: {uds.hex() if uds else '(空)'}")
    return ok(uds)

def connect_retry(host, port, n=10, d=1):
    for i in range(n):
        try:
            s = socket.socket(); s.connect((host, port))
            print(f"[连接成功] 第{i+1}次"); return s
        except: print(f"  等待 ({i+1}/{n})"); time.sleep(d)
    raise ConnectionRefusedError

# ============================================================
print("=" * 60)
print("  DoIP AB 分区 OTA 完整流程测试")
print("=" * 60)

FW = "./doip_server"
if not os.path.exists(FW): print(f"❌ 找不到 {FW}"); exit(1)
firmware = open(FW, "rb").read()
print(f"\n📦 固件: {FW} ({len(firmware)} 字节)")

sock = socket.socket(); sock.connect((HOST, PORT))
print(f"[1] 连接 {HOST}:{PORT}")

send_doip(sock, 0x0005, struct.pack('>H', TESTER_ADDR) + b'\x00\x00')
ptype, _ = recv_doip(sock)
print(f"[2] 路由激活: {'✅' if ptype == 0x0006 else '❌'}")

print("\n--- 0x22 ---")
send_diag(sock, b'\x22\xF1\x92'); step("软件版本", recv_diag(sock))

print("\n--- 编程会话+安全访问 ---")
send_diag(sock, b'\x10\x03'); step("10 03", recv_diag(sock))
send_diag(sock, b'\x27\x01'); uds = recv_diag(sock)
if not ok(uds): print("❌"); sock.close(); exit(1)
seed = uds[2:6]; key = bytes([~b & 0xFF for b in seed])
send_diag(sock, b'\x27\x02' + key); step("27 02", recv_diag(sock))

print("\n--- 刷写准备 ---")
send_diag(sock, b'\x31\x01\xFF\x01'); step("检查条件", recv_diag(sock))
send_diag(sock, b'\x31\x01\xFF\x00'); step("擦除", recv_diag(sock))

print(f"\n--- 下载 {len(firmware)} 字节 ---")
addr = 0x08000000
send_diag(sock, struct.pack('>BBBI', 0x34, 0x00, 0x44, addr) + struct.pack('>I', len(firmware)))
if not step("请求下载", recv_diag(sock)): sock.close(); exit(1)

total = 0; seq = 1
while total < len(firmware):
    blen = min(4096, len(firmware) - total)
    send_diag(sock, bytes([0x36, seq]) + firmware[total:total+blen])
    if not step(f"块#{seq}", recv_diag(sock)): break
    total += blen; seq += 1
    if seq % 10 == 0:
        print(f"  进度 {total}/{len(firmware)} ({total*100//len(firmware)}%)")

if total == len(firmware):
    print(f"\n--- 传输完成 ({total} 字节) ---")
    send_diag(sock, b'\x37'); step("37 退出", recv_diag(sock))
    send_diag(sock, b'\x31\x01\xFF\x02'); step("CRC", recv_diag(sock))
    send_diag(sock, b'\x31\x01\xFF\x03'); step("Commit", recv_diag(sock))

    print("\n--- ECU 复位 ---")
    send_diag(sock, b'\x11\x01')
    time.sleep(0.5); sock.close()

    print("\n" + "=" * 60)
    print("  等待 Boot Manager 重启...")
    print("=" * 60)
    time.sleep(3)

    sock = connect_retry(HOST, PORT)
    send_doip(sock, 0x0005, struct.pack('>H', TESTER_ADDR) + b'\x00\x00')
    recv_doip(sock); print("[重新路由激活] ✅")
    send_diag(sock, b'\x31\x01\xFF\x04'); step("Accept", recv_diag(sock))
    print("\n--- 验证 ---")
    send_diag(sock, b'\x22\xF1\x92'); step("软件版本", recv_diag(sock))
    print("\n" + "=" * 60)
    print("  🎉 OTA 刷写完成！")
    print("=" * 60)

sock.close()
print("\n测试结束")