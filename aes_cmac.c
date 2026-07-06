/*******************************************************************************
****Author: Fang Zheng
****Date: 2026-07-03
****Description: 纯 C 实现的 AES-128-CMAC（零依赖，无 OpenSSL）
****    参考: NIST SP 800-38B, RFC 4493
****    用于 UDS 0x27 安全访问的种子→密钥计算
****FilePath: \demo\aes_cmac.c
********************************************************************************/
#include "aes_cmac.h"
#include <string.h>

// ============================================================
//  AES-128 S-Box（256 字节）
// ============================================================
static const uint8_t sbox[256] = {
    0x63,0x7C,0x77,0x7B,0xF2,0x6B,0x6F,0xC5,0x30,0x01,0x67,0x2B,0xFE,0xD7,0xAB,0x76,
    0xCA,0x82,0xC9,0x7D,0xFA,0x59,0x47,0xF0,0xAD,0xD4,0xA2,0xAF,0x9C,0xA4,0x72,0xC0,
    0xB7,0xFD,0x93,0x26,0x36,0x3F,0xF7,0xCC,0x34,0xA5,0xE5,0xF1,0x71,0xD8,0x31,0x15,
    0x04,0xC7,0x23,0xC3,0x18,0x96,0x05,0x9A,0x07,0x12,0x80,0xE2,0xEB,0x27,0xB2,0x75,
    0x09,0x83,0x2C,0x1A,0x1B,0x6E,0x5A,0xA0,0x52,0x3B,0xD6,0xB3,0x29,0xE3,0x2F,0x84,
    0x53,0xD1,0x00,0xED,0x20,0xFC,0xB1,0x5B,0x6A,0xCB,0xBE,0x39,0x4A,0x4C,0x58,0xCF,
    0xD0,0xEF,0xAA,0xFB,0x43,0x4D,0x33,0x85,0x45,0xF9,0x02,0x7F,0x50,0x3C,0x9F,0xA8,
    0x51,0xA3,0x40,0x8F,0x92,0x9D,0x38,0xF5,0xBC,0xB6,0xDA,0x21,0x10,0xFF,0xF3,0xD2,
    0xCD,0x0C,0x13,0xEC,0x5F,0x97,0x44,0x17,0xC4,0xA7,0x7E,0x3D,0x64,0x5D,0x19,0x73,
    0x60,0x81,0x4F,0xDC,0x22,0x2A,0x90,0x88,0x46,0xEE,0xB8,0x14,0xDE,0x5E,0x0B,0xDB,
    0xE0,0x32,0x3A,0x0A,0x49,0x06,0x24,0x5C,0xC2,0xD3,0xAC,0x62,0x91,0x95,0xE4,0x79,
    0xE7,0xC8,0x37,0x6D,0x8D,0xD5,0x4E,0xA9,0x6C,0x56,0xF4,0xEA,0x65,0x7A,0xAE,0x08,
    0xBA,0x78,0x25,0x2E,0x1C,0xA6,0xB4,0xC6,0xE8,0xDD,0x74,0x1F,0x4B,0xBD,0x8B,0x8A,
    0x70,0x3E,0xB5,0x66,0x48,0x03,0xF6,0x0E,0x61,0x35,0x57,0xB9,0x86,0xC1,0x1D,0x9E,
    0xE1,0xF8,0x98,0x11,0x69,0xD9,0x8E,0x94,0x9B,0x1E,0x87,0xE9,0xCE,0x55,0x28,0xDF,
    0x8C,0xA1,0x89,0x0D,0xBF,0xE6,0x42,0x68,0x41,0x99,0x2D,0x0F,0xB0,0x54,0xBB,0x16,
};

// ============================================================
//  Rcon 轮常量（AES-128 密钥扩展用）
// ============================================================
static const uint8_t rcon[10] = {
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36
};

// ============================================================
//  AES-128 密钥扩展：16 字节密钥 → 176 字节（11 轮密钥）
// ============================================================
static void aes_key_expand(const uint8_t *key, uint8_t *round_keys) {
    memcpy(round_keys, key, 16);

    for (int i = 4; i < 44; i++) {
        uint8_t temp[4];
        memcpy(temp, round_keys + (i - 1) * 4, 4);

        if (i % 4 == 0) {
            // RotWord
            uint8_t t = temp[0];
            temp[0] = temp[1]; temp[1] = temp[2]; temp[2] = temp[3]; temp[3] = t;
            // SubWord
            for (int j = 0; j < 4; j++) temp[j] = sbox[temp[j]];
            // XOR Rcon
            temp[0] ^= rcon[i / 4 - 1];
        }

        for (int j = 0; j < 4; j++)
            round_keys[i * 4 + j] = round_keys[(i - 4) * 4 + j] ^ temp[j];
    }
}

// ============================================================
//  伽罗瓦域乘法（GF(2^8)），用于 MixColumns
// ============================================================
static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        uint8_t hi = a & 0x80;
        a <<= 1;
        if (hi) a ^= 0x1B;
        b >>= 1;
    }
    return p;
}

// ============================================================
//  AES-128 加密单个块（16 字节）
//  block: 输入输出（原地修改）
// ============================================================
static void aes_encrypt_block(const uint8_t *round_keys, uint8_t *block) {
    // AddRoundKey（轮 0）
    for (int i = 0; i < 16; i++) block[i] ^= round_keys[i];

    // 第 1~9 轮
    for (int r = 1; r < 10; r++) {
        // SubBytes
        for (int i = 0; i < 16; i++) block[i] = sbox[block[i]];
        // ShiftRows
        uint8_t tmp[16];
        tmp[0]  = block[0];  tmp[1]  = block[5];  tmp[2]  = block[10]; tmp[3]  = block[15];
        tmp[4]  = block[4];  tmp[5]  = block[9];  tmp[6]  = block[14]; tmp[7]  = block[3];
        tmp[8]  = block[8];  tmp[9]  = block[13]; tmp[10] = block[2];  tmp[11] = block[7];
        tmp[12] = block[12]; tmp[13] = block[1];  tmp[14] = block[6];  tmp[15] = block[11];
        memcpy(block, tmp, 16);
        // MixColumns
        for (int c = 0; c < 4; c++) {
            uint8_t a[4];
            for (int i = 0; i < 4; i++) a[i] = block[c * 4 + i];
            uint8_t b0 = gmul(2, a[0]) ^ gmul(3, a[1]) ^ a[2] ^ a[3];
            uint8_t b1 = a[0] ^ gmul(2, a[1]) ^ gmul(3, a[2]) ^ a[3];
            uint8_t b2 = a[0] ^ a[1] ^ gmul(2, a[2]) ^ gmul(3, a[3]);
            uint8_t b3 = gmul(3, a[0]) ^ a[1] ^ a[2] ^ gmul(2, a[3]);
            block[c * 4 + 0] = b0;
            block[c * 4 + 1] = b1;
            block[c * 4 + 2] = b2;
            block[c * 4 + 3] = b3;
        }
        // AddRoundKey
        for (int i = 0; i < 16; i++) block[i] ^= round_keys[r * 16 + i];
    }

    // 第 10 轮（无 MixColumns）
    for (int i = 0; i < 16; i++) block[i] = sbox[block[i]];
    uint8_t tmp[16];
    tmp[0]  = block[0];  tmp[1]  = block[5];  tmp[2]  = block[10]; tmp[3]  = block[15];
    tmp[4]  = block[4];  tmp[5]  = block[9];  tmp[6]  = block[14]; tmp[7]  = block[3];
    tmp[8]  = block[8];  tmp[9]  = block[13]; tmp[10] = block[2];  tmp[11] = block[7];
    tmp[12] = block[12]; tmp[13] = block[1];  tmp[14] = block[6];  tmp[15] = block[11];
    memcpy(block, tmp, 16);
    for (int i = 0; i < 16; i++) block[i] ^= round_keys[160 + i];
}

// ============================================================
//  左移 1 位（128 位视为大整数），用于 CMAC 子密钥生成
// ============================================================
static void left_shift_1(const uint8_t *in, uint8_t *out) {
    uint8_t carry = 0;
    for (int i = 15; i >= 0; i--) {
        out[i] = (in[i] << 1) | carry;
        carry = (in[i] >> 7) & 1;
    }
}

// ============================================================
//  计算 AES-128-CMAC
//  key: 16 字节密钥
//  data: 输入数据
//  data_len: 输入数据长度（字节）
//  mac: 输出 16 字节 MAC 值
// ============================================================
void aes_cmac(const uint8_t *key, const uint8_t *data, size_t data_len,
              uint8_t *mac) {
    uint8_t round_keys[176];
    aes_key_expand(key, round_keys);

    // 生成子密钥 K1, K2
    // L = AES(key, 0x00...00)
    uint8_t L[16] = {0};
    aes_encrypt_block(round_keys, L);

    // K1 = (L << 1) ^ (MSB(L) == 1 ? Rb : 0), Rb = 0x87
    uint8_t K1[16], K2[16];
    left_shift_1(L, K1);
    if (L[0] & 0x80) K1[15] ^= 0x87;

    // K2 = (K1 << 1) ^ (MSB(K1) == 1 ? Rb : 0)
    left_shift_1(K1, K2);
    if (K1[0] & 0x80) K2[15] ^= 0x87;

    // 准备消息块
    uint8_t M[16];
    memset(M, 0, 16);

    if (data_len == 16) {
        // 整块：XOR K1
        memcpy(M, data, 16);
        for (int i = 0; i < 16; i++) M[i] ^= K1[i];
    } else {
        // 不足一块：10* 填充 + XOR K2
        memcpy(M, data, data_len);
        M[data_len] = 0x80;  // 10* 填充：1 后面全是 0
        for (int i = 0; i < 16; i++) M[i] ^= K2[i];
    }

    // 最终加密
    aes_encrypt_block(round_keys, M);
    memcpy(mac, M, 16);
}