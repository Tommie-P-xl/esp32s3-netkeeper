/*
 * SRun Portal 认证算法（与登录页 Portal.js 的 encode() 逐字节等价）。
 *
 * 三个字段的来历：
 *   hmd5   = HMAC-MD5(key = token, msg = 明文密码)       十六进制小写
 *   info   = "{SRBX1}" + SRunBase64(xencode(json, token))
 *   chksum = SHA1(token+u + token+hmd5 + token+acid + token+ip +
 *                 token+n + token+type + token+info)
 *
 * 两个坑（2026-10 在电脑端 Python 实现里踩过，这里同样遵守）：
 *   1. base64 不是标准字母表，SRun 换成
 *      "LVoJPiCN2R8G90yg+hmFHuacZ1OWMnrsSTXkYpUq/3dlbfKwv6xztjI7DeBE45QA"
 *   2. xencode 是 XXTEA 变体，JS 的 for 循环结束后 p == n，收尾那一轮必须
 *      用 k[(n & 3) ^ e]，写成 k[((n-1) & 3) ^ e] 会静默出错。
 *
 * 说明：xencode 的 s() 在 JS 里按 UTF-16 code unit 取字符，本实现按字节打包。
 * 用户名/密码/ip/ac_id/enc_ver 全是 ASCII 时两者完全一致；一旦出现非 ASCII
 * 字符（中文密码等）结果会不同，届时需要先转 UTF-16LE。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 小写十六进制 MD5。
 * @param hex_out 至少 33 字节
 */
void srun_md5_hex(const void *data, size_t len, char *hex_out);

/**
 * @brief HMAC-MD5(key, msg) 的小写十六进制，对应登录页的 md5(password, token)。
 * @param hex_out 至少 33 字节
 */
void srun_hmac_md5_hex(const char *key, const char *msg, char *hex_out);

/**
 * @brief 小写十六进制 SHA1，用于 chksum。
 * @param hex_out 至少 41 字节
 */
void srun_sha1_hex(const char *data, size_t len, char *hex_out);

/**
 * @brief SRun 自定义字母表 base64（带 '=' 补齐）。
 * @param out_cap 建议 4 * ((len + 2) / 3) + 1
 */
void srun_base64(const uint8_t *data, size_t len, char *out, size_t out_cap);

/**
 * @brief xencode（XXTEA 变体）。
 *
 * 输出长度恒为 4 * (ceil(in_len / 4) + 1) 字节，不做长度裁剪。
 * @param out_cap 需 >= 4 * (in_len / 4 + 2)
 */
void srun_xencode(const uint8_t *in, size_t in_len, const char *key,
                  uint8_t *out, size_t out_cap, size_t *out_len);

/**
 * @brief 生成 info 字段："{SRBX1}" + base64(xencode(json, token))。
 */
void srun_encode_info(const char *json, const char *token, char *out, size_t out_cap);

/**
 * @brief 自检：用固定输入跑一遍算法，把中间结果打到串口。
 *
 * 只在 bring-up 阶段调用；正常情况下一次都不用跑。
 */
void srun_crypto_selftest(void);

#ifdef __cplusplus
}
#endif
