#!/usr/bin/env python3
"""
DoIP 一键诊断测试脚本
流程：连接 → 路由激活 → 扩展会话 → 安全访问(27解锁)
"""
#!/usr/bin/env python3
import socket
import struct
import random

HOST, PORT = '127.0.0.1', 13400
DOIP_DIAG = 0x8001
SA = TA = 0x0E80

def recv_doip(sock):
    hdr = sock.recv(8, socket.MSG_WAITALL)
    if len(hdr) < 8: return None, None
    ptype = struct.unpack('>H', hdr[2:4])[0]
    plen  = struct.unpack('>I', hdr[4:8])[0]
    payload = sock.recv(plen, socket.MSG_WAITALL) if plen > 0 else b''
    return ptype, payload

def send_doip(sock, ptype, payload):
    hdr = b'\x02\xfd' + struct.pack('>H', ptype) + struct.pack('>I', len(payload))
    sock.sendall(hdr + payload)

def send_diag(sock, uds_data):
    payload = struct.pack('>H', SA) + struct.pack('>H', TA) + uds_data
    send_doip(sock, DOIP_DIAG, payload)

def recv_diag(sock):
    ptype, payload = recv_doip(sock)
    return payload[4:] if ptype == DOIP_DIAG else None

def parse_uds(uds_data):
    if len(uds_data) < 1: return None, False, b''
    sid = uds_data[0]
    if sid == 0x7F: return uds_data[1], False, uds_data[2:]
    return sid & 0x3F, True, uds_data[1:]

# =========================================================
sock = socket.socket()
sock.connect((HOST, PORT))
print(f'[1] 连接 {HOST}:{PORT}')

# Step 1: 路由激活
send_doip(sock, 0x0005, struct.pack('>H', SA) + struct.pack('>H', TA) + b'\x00\x00')
recv_doip(sock); print('[2] 路由激活')

# Step 2: 编程会话 (10 02)
send_diag(sock, b'\x10\x02'); uds = recv_diag(sock)
print(f'[3] 编程会话: {"✅" if parse_uds(uds)[1] else "❌"}  {uds.hex()}')

# Step 3: 安全访问解锁
send_diag(sock, b'\x27\x01'); uds = recv_diag(sock)
_, ok, data = parse_uds(uds)
seed = data[1:5]; key = bytes([~b & 0xFF for b in seed])
send_diag(sock, b'\x27\x02' + key); uds = recv_diag(sock)
print(f'[4] 安全访问: {"✅ 解锁" if parse_uds(uds)[1] else "❌"}')

# Step 5: 例程控制
print('\n--- 0x31 例程控制 ---')
send_diag(sock, b'\x31\x01\xFF\x01'); uds = recv_diag(sock)
print(f'[5] 检查条件: {"✅" if parse_uds(uds)[1] else "❌"}  {uds.hex()}')
send_diag(sock, b'\x31\x01\xFF\x00'); uds = recv_diag(sock)
print(f'[6] 擦除Flash: {"✅" if parse_uds(uds)[1] else "❌"}  {uds.hex()}')

# Step 6: 开始下载
print('\n--- 下载流程 ---')
firmware_size = 8192  # 模拟 8KB 固件
addr = 0x08000000
# 34 00 44 addr(4) size(4)
send_diag(sock, b'\x34\x00\x44' + struct.pack('>II', addr, firmware_size))
uds = recv_diag(sock)
print(f'[7] 请求下载: {"✅" if parse_uds(uds)[1] else "❌"}  {uds.hex()}')

# Step 7: 逐块传输 (4KB per block)
max_block = 4096
total_sent = 0
block_seq = 1
while total_sent < firmware_size:
    remaining = firmware_size - total_sent
    block_len = min(remaining, max_block)
    # 生成随机数据（模拟固件）
    data = bytes([random.randint(0, 255) for _ in range(block_len)])
    # 发块: 36 seq + data
    send_diag(sock, b'\x36' + bytes([block_seq]) + data)
    uds = recv_diag(sock)
    sid, ok, d = parse_uds(uds)
    if not ok:
        print(f'[8] 块#{block_seq}: ❌  {uds.hex()}')
        break
    ack_seq = d[0]
    if ack_seq != block_seq:
        print(f'[8] 块#{block_seq}: ❌ 序号不匹配，回复序号={ack_seq}')
        break
    total_sent += block_len
    print(f'[8] 块#{block_seq}: {block_len}字节  进度={total_sent}/{firmware_size}')
    block_seq += 1

# Step 9: 传输退出
if total_sent == firmware_size:
    send_diag(sock, b'\x37'); uds = recv_diag(sock)
    print(f'\n[9] 传输退出: {"✅" if parse_uds(uds)[1] else "❌"}  {uds.hex()}')
    # Step 10: CRC校验
    send_diag(sock, b'\x31\x01\xFF\x02'); uds = recv_diag(sock)
    print(f'[10] CRC校验: {"✅" if parse_uds(uds)[1] else "❌"}  {uds.hex()}')

    # Step 11: Commit（暂存区 → 非活跃槽 + 切换 active_slot）
    send_diag(sock, b'\x31\x01\xFF\x03'); uds = recv_diag(sock)
    print(f'[11] Commit: {"✅" if parse_uds(uds)[1] else "❌"}  {uds.hex()}')

    # Step 12: ECU 复位（重启后跑新固件）
    send_diag(sock, b'\x11\x01'); uds = recv_diag(sock)
    print(f'[12] ECU复位: {"✅" if parse_uds(uds)[1] else "❌"}')

    # Step 13: Accept（确认新固件正常，标记 OK，部署到供应商目录）
    sock.close()
    sock = socket.socket(); sock.connect((HOST, PORT))
    send_doip(sock, 0x0005, struct.pack('>H', SA) + struct.pack('>H', TA) + b'\x00\x00')
    recv_doip(sock)
    send_diag(sock, b'\x10\x02'); recv_diag(sock)
    send_diag(sock, b'\x27\x01'); uds = recv_diag(sock)
    _, _, data = parse_uds(uds)
    seed = data[1:5]; key = bytes([~b & 0xFF for b in seed])
    send_diag(sock, b'\x27\x02' + key); recv_diag(sock)
    send_diag(sock, b'\x31\x01\xFF\x04'); uds = recv_diag(sock)
    print(f'[13] Accept: {"✅" if parse_uds(uds)[1] else "❌"}  {uds.hex()}')

    print('\n🎉 刷写完成！')
    print('  暂存区: /ota/staging/firmware.bin')
    print('  A 槽:   /ota/slot_a/firmware.bin')
    print('  B 槽:   /ota/slot_b/firmware.bin')
    print('  供应商: /vendor/sdk/firmware.bin')
else:
    print(f'\n❌ 传输未完成，只发了 {total_sent}/{firmware_size} 字节')

sock.close()
print('\n测试完成!')