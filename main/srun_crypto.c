#include "srun_crypto.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "srun_crypto";

/* SRun / jquery-base64 的 _ALPHA */
static const char B64_ALPHA[] =
    "LVoJPiCN2R8G90yg+hmFHuacZ1OWMnrsSTXkYpUq/3dlbfKwv6xztjI7DeBE45QA";

#define XE_MAX_WORDS 512

/* =======================================================================
 * MD5 / SHA1 / HMAC-MD5 —— 自带实现
 *
 * 为什么不用 mbedtls：ESP-IDF v6.1 带的是 mbedtls 4.x，一次性接口
 * mbedtls_md_hmac() 已经移除（只剩 PSA Crypto），而这三样只是登录时算一次
 * 摘要，为此引入 PSA 初始化反而更重。这里按 RFC 1321 / RFC 3174 / RFC 2104
 * 手写，约 120 行，少一个版本依赖。
 * ======================================================================= */

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

/* ------------------------------------------------------------------ MD5 */

static const uint32_t MD5_K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static const uint8_t MD5_S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static void md5_block(uint32_t st[4], const uint8_t blk[64])
{
    uint32_t m[16];

    for (int i = 0; i < 16; i++) {
        m[i] = (uint32_t)blk[i * 4] |
               ((uint32_t)blk[i * 4 + 1] << 8) |
               ((uint32_t)blk[i * 4 + 2] << 16) |
               ((uint32_t)blk[i * 4 + 3] << 24);
    }

    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];

    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;

        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }

        const uint32_t tmp = d;
        d = c;
        c = b;
        b = b + ROTL32(a + f + MD5_K[i] + m[g], MD5_S[i]);
        a = tmp;
    }

    st[0] += a;
    st[1] += b;
    st[2] += c;
    st[3] += d;
}

static void md5_raw(const uint8_t *data, size_t len, uint8_t out[16])
{
    uint32_t st[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    uint8_t tail[128];
    size_t i = 0;

    for (; i + 64 <= len; i += 64) {
        md5_block(st, data + i);
    }

    const size_t rest = len - i;
    const size_t tail_len = (rest + 8 < 56) ? 64 : 128;

    memset(tail, 0, sizeof(tail));
    if (rest > 0) {
        memcpy(tail, data + i, rest);
    }
    tail[rest] = 0x80;

    const uint64_t bits = (uint64_t)len * 8;
    for (int k = 0; k < 8; k++) {
        tail[tail_len - 8 + k] = (uint8_t)((bits >> (8 * k)) & 0xFF);
    }

    md5_block(st, tail);
    if (tail_len == 128) {
        md5_block(st, tail + 64);
    }

    for (int k = 0; k < 4; k++) {
        out[k * 4 + 0] = (uint8_t)(st[k] & 0xFF);
        out[k * 4 + 1] = (uint8_t)((st[k] >> 8) & 0xFF);
        out[k * 4 + 2] = (uint8_t)((st[k] >> 16) & 0xFF);
        out[k * 4 + 3] = (uint8_t)((st[k] >> 24) & 0xFF);
    }
}

/* ----------------------------------------------------------------- SHA1 */

static void sha1_block(uint32_t st[5], const uint8_t blk[64])
{
    uint32_t w[80];

    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)blk[i * 4] << 24) |
               ((uint32_t)blk[i * 4 + 1] << 16) |
               ((uint32_t)blk[i * 4 + 2] << 8) |
               (uint32_t)blk[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = ROTL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4];

    for (int i = 0; i < 80; i++) {
        uint32_t f, k;

        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }

        const uint32_t tmp = ROTL32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = ROTL32(b, 30);
        b = a;
        a = tmp;
    }

    st[0] += a;
    st[1] += b;
    st[2] += c;
    st[3] += d;
    st[4] += e;
}

static void sha1_raw(const uint8_t *data, size_t len, uint8_t out[20])
{
    uint32_t st[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint8_t tail[128];
    size_t i = 0;

    for (; i + 64 <= len; i += 64) {
        sha1_block(st, data + i);
    }

    const size_t rest = len - i;
    const size_t tail_len = (rest + 8 < 56) ? 64 : 128;

    memset(tail, 0, sizeof(tail));
    if (rest > 0) {
        memcpy(tail, data + i, rest);
    }
    tail[rest] = 0x80;

    const uint64_t bits = (uint64_t)len * 8;
    for (int k = 0; k < 8; k++) {
        tail[tail_len - 1 - k] = (uint8_t)((bits >> (8 * k)) & 0xFF);
    }

    sha1_block(st, tail);
    if (tail_len == 128) {
        sha1_block(st, tail + 64);
    }

    for (int k = 0; k < 5; k++) {
        out[k * 4 + 0] = (uint8_t)((st[k] >> 24) & 0xFF);
        out[k * 4 + 1] = (uint8_t)((st[k] >> 16) & 0xFF);
        out[k * 4 + 2] = (uint8_t)((st[k] >> 8) & 0xFF);
        out[k * 4 + 3] = (uint8_t)(st[k] & 0xFF);
    }
}

/* ------------------------------------------------------------- 对外封装 */

static void to_hex(const uint8_t *raw, size_t n, char *hex)
{
    for (size_t i = 0; i < n; i++) {
        sprintf(&hex[i * 2], "%02x", raw[i]);
    }
    hex[n * 2] = '\0';
}

void srun_md5_hex(const void *data, size_t len, char *hex_out)
{
    uint8_t d[16];
    md5_raw((const uint8_t *)data, len, d);
    to_hex(d, sizeof(d), hex_out);
}

void srun_sha1_hex(const char *data, size_t len, char *hex_out)
{
    uint8_t d[20];
    sha1_raw((const uint8_t *)data, len, d);
    to_hex(d, sizeof(d), hex_out);
}

void srun_hmac_md5_hex(const char *key, const char *msg, char *hex_out)
{
    uint8_t k[64];
    uint8_t ipad[64];
    uint8_t opad[64];
    uint8_t inner[16];
    uint8_t outer[16];
    const size_t klen = strlen(key);
    const size_t mlen = strlen(msg);

    memset(k, 0, sizeof(k));
    if (klen > 64) {
        md5_raw((const uint8_t *)key, klen, k); /* 超长 key 先摘要 */
    } else {
        memcpy(k, key, klen);
    }

    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5C;
    }

    /* inner = MD5(ipad || msg)，msg 最长按 1 KB 处理 */
    {
        uint8_t buf[64 + 1024];
        if (mlen > sizeof(buf) - 64) {
            ESP_LOGE(TAG, "hmac msg too long");
            hex_out[0] = '\0';
            return;
        }
        memcpy(buf, ipad, 64);
        memcpy(buf + 64, msg, mlen);
        md5_raw(buf, 64 + mlen, inner);
    }

    {
        uint8_t buf[64 + 16];
        memcpy(buf, opad, 64);
        memcpy(buf + 64, inner, 16);
        md5_raw(buf, 64 + 16, outer);
    }

    to_hex(outer, 16, hex_out);
}

/* ---------------------------------------------------------------- base64 */

void srun_base64(const uint8_t *data, size_t len, char *out, size_t out_cap)
{
    if (out_cap < (len + 2) / 3 * 4 + 1) {
        if (out_cap > 0) {
            out[0] = '\0';
        }
        return;
    }

    const size_t imax = len - len % 3;
    size_t o = 0;

    for (size_t i = 0; i < imax; i += 3) {
        const uint32_t b10 = ((uint32_t)data[i] << 16) |
                             ((uint32_t)data[i + 1] << 8) |
                             (uint32_t)data[i + 2];
        out[o++] = B64_ALPHA[(b10 >> 18) & 63];
        out[o++] = B64_ALPHA[(b10 >> 12) & 63];
        out[o++] = B64_ALPHA[(b10 >> 6) & 63];
        out[o++] = B64_ALPHA[b10 & 63];
    }

    const size_t rest = len - imax;
    if (rest == 1) {
        const uint32_t b10 = (uint32_t)data[imax] << 16;
        out[o++] = B64_ALPHA[(b10 >> 18) & 63];
        out[o++] = B64_ALPHA[(b10 >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rest == 2) {
        const uint32_t b10 = ((uint32_t)data[imax] << 16) | ((uint32_t)data[imax + 1] << 8);
        out[o++] = B64_ALPHA[(b10 >> 18) & 63];
        out[o++] = B64_ALPHA[(b10 >> 12) & 63];
        out[o++] = B64_ALPHA[(b10 >> 6) & 63];
        out[o++] = '=';
    }

    out[o] = '\0';
}

/* --------------------------------------------------------------- xencode */

void srun_xencode(const uint8_t *in, size_t in_len, const char *key,
                  uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (out_len != NULL) {
        *out_len = 0;
    }
    if (in_len == 0) {
        return;
    }

    const size_t vn = (in_len + 3) / 4; /* 数据字数 */
    const size_t total = vn + 1;        /* 末尾再放一个长度字 */
    if (total > XE_MAX_WORDS || out_cap < total * 4) {
        ESP_LOGE(TAG, "xencode buffer too small (%u words)", (unsigned)total);
        return;
    }

    uint32_t v[XE_MAX_WORDS];
    memset(v, 0, total * sizeof(uint32_t));

    for (size_t i = 0; i < in_len; i++) {
        v[i / 4] |= (uint32_t)in[i] << (8 * (i % 4));
    }
    v[total - 1] = (uint32_t)in_len;

    uint32_t k[4] = {0, 0, 0, 0};
    const size_t klen = strlen(key);
    for (size_t i = 0; i < klen && i < 16; i++) {
        k[i / 4] |= (uint32_t)(uint8_t)key[i] << (8 * (i % 4));
    }

    const size_t n = total - 1;
    uint32_t z = v[n];
    uint32_t y = v[0];
    uint32_t d = 0;
    const uint32_t c = 0x9E3779B9u; /* 0x86014019 | 0x183639A0 */
    int q = (int)(6 + 52 / (n + 1));

    while (q-- > 0) {
        d += c;
        const uint32_t e = (d >> 2) & 3u;

        for (size_t p = 0; p < n; p++) {
            y = v[p + 1];
            uint32_t m = (z >> 5) ^ (y << 2);
            m += (y >> 3) ^ (z << 4) ^ (d ^ y);
            m += k[(p & 3u) ^ e] ^ z;
            z = v[p] = v[p] + m;
        }

        /* JS 的 for 结束后 p == n，收尾这一轮用的是 n，不是 n-1 */
        y = v[0];
        uint32_t m = (z >> 5) ^ (y << 2);
        m += (y >> 3) ^ (z << 4) ^ (d ^ y);
        m += k[(n & 3u) ^ e] ^ z;
        z = v[n] = v[n] + m;
    }

    for (size_t i = 0; i < total; i++) {
        out[i * 4 + 0] = (uint8_t)(v[i] & 0xFF);
        out[i * 4 + 1] = (uint8_t)((v[i] >> 8) & 0xFF);
        out[i * 4 + 2] = (uint8_t)((v[i] >> 16) & 0xFF);
        out[i * 4 + 3] = (uint8_t)((v[i] >> 24) & 0xFF);
    }

    if (out_len != NULL) {
        *out_len = total * 4;
    }
}

void srun_encode_info(const char *json, const char *token, char *out, size_t out_cap)
{
    uint8_t enc[XE_MAX_WORDS * 4];
    size_t enc_len = 0;

    out[0] = '\0';
    srun_xencode((const uint8_t *)json, strlen(json), token, enc, sizeof(enc), &enc_len);
    if (enc_len == 0) {
        return;
    }

    const size_t b64_cap = (enc_len + 2) / 3 * 4 + 1;
    if (out_cap < b64_cap + 7) {
        ESP_LOGE(TAG, "info buffer too small");
        return;
    }

    memcpy(out, "{SRBX1}", 7);
    srun_base64(enc, enc_len, out + 7, out_cap - 7);
}

/* -------------------------------------------------------------- selftest */

/*
 * 期望值由电脑端 Python 参考实现算出，用来确认 C 版本与 JS 逐字节一致。
 * 下面用的是示例测试向量（非真实账号）：
 *   hmd5   = hmac_md5(token, password)
 *   info   = {SRBX1} + base64(encrypt(json, token))
 */
void srun_crypto_selftest(void)
{
    static const char *token = "0123456789abcdef0123456789abcdef";
    static const char *pass = "example-pass";
    static const char *json =
        "{\"username\":\"example-user\",\"password\":\"example-pass\","
        "\"ip\":\"10.0.0.1\",\"acid\":\"1\",\"enc_ver\":\"srun_bx1\"}";
    char hmd5[33];
    char info[512];

    srun_hmac_md5_hex(token, pass, hmd5);
    srun_encode_info(json, token, info, sizeof(info));

    ESP_LOGI(TAG, "selftest hmd5 = %s", hmd5);
    ESP_LOGI(TAG, "selftest info = %s", info);
}
