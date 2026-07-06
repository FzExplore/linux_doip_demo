/*******************************************************************************
****Author: Fang Zheng
****Date: 2026-07-03
****Description: 纯 C 实现的 AES-128-CMAC（零依赖，无 OpenSSL）
****    用于 UDS 0x27 安全访问的种子→密钥计算
****FilePath: \demo\aes_cmac.h
********************************************************************************/
#ifndef AES_CMAC_H
#define AES_CMAC_H

#include <stdint.h>
#include <stddef.h>

// ============================================================
//  计算 AES-128-CMAC
//  key: 16 字节密钥
//  data: 输入数据
//  data_len: 输入数据长度（字节）
//  mac: 输出 16 字节 MAC 值
//  注意：此实现仅支持单块消息（data_len <= 16），
//        这对 UDS 0x27 的 4 字节种子完全够用
// ============================================================
void aes_cmac(const uint8_t *key, const uint8_t *data, size_t data_len,
              uint8_t *mac);

#endif // AES_CMAC_H