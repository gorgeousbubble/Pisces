/**
 * @file net_auth.c
 * @brief MCU↔服务器通信 HMAC-SHA256 签名实现
 *
 * SHA-256 核心实现基于 Brad Conte 的公有领域代码
 * （https://github.com/B-Con/crypto-algorithms）
 */

#include "net_auth.h"
#include "log.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>

#define TAG "AUTH"

/* -----------------------------------------------------------------------
 * SHA-256 实现（轻量级，无外部依赖）
 * ----------------------------------------------------------------------- */
#define SHA256_BLOCK_SIZE  64U
#define SHA256_DIGEST_SIZE 32U

typedef struct {
    uint8_t  data[SHA256_BLOCK_SIZE];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} sha256_ctx_t;

static const uint32_t K[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

#define ROTRIGHT(a,b) (((a) >> (b)) | ((a) << (32U-(b))))
#define CH(x,y,z)  (((x)&(y))^(~(x)&(z)))
#define MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define EP0(x)  (ROTRIGHT(x,2U)  ^ ROTRIGHT(x,13U) ^ ROTRIGHT(x,22U))
#define EP1(x)  (ROTRIGHT(x,6U)  ^ ROTRIGHT(x,11U) ^ ROTRIGHT(x,25U))
#define SIG0(x) (ROTRIGHT(x,7U)  ^ ROTRIGHT(x,18U) ^ ((x)>>3U))
#define SIG1(x) (ROTRIGHT(x,17U) ^ ROTRIGHT(x,19U) ^ ((x)>>10U))

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t *data)
{
    uint32_t a, b, c, d, e, f, g, h, i, j, t1, t2, m[64];

    for (i = 0U, j = 0U; i < 16U; i++, j += 4U) {
        m[i] = ((uint32_t)data[j]     << 24U) |
               ((uint32_t)data[j+1U]  << 16U) |
               ((uint32_t)data[j+2U]  <<  8U) |
               ((uint32_t)data[j+3U]);
    }
    for (; i < 64U; i++) {
        m[i] = SIG1(m[i-2U]) + m[i-7U] + SIG0(m[i-15U]) + m[i-16U];
    }

    a = ctx->state[0]; b = ctx->state[1];
    c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5];
    g = ctx->state[6]; h = ctx->state[7];

    for (i = 0U; i < 64U; i++) {
        t1 = h + EP1(e) + CH(e,f,g) + K[i] + m[i];
        t2 = EP0(a) + MAJ(a,b,c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b;
    ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f;
    ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(sha256_ctx_t *ctx)
{
    ctx->datalen = 0U;
    ctx->bitlen  = 0U;
    ctx->state[0] = 0x6a09e667U; ctx->state[1] = 0xbb67ae85U;
    ctx->state[2] = 0x3c6ef372U; ctx->state[3] = 0xa54ff53aU;
    ctx->state[4] = 0x510e527fU; ctx->state[5] = 0x9b05688cU;
    ctx->state[6] = 0x1f83d9abU; ctx->state[7] = 0x5be0cd19U;
}

static void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
    for (size_t i = 0U; i < len; i++) {
        ctx->data[ctx->datalen] = data[i];
        ctx->datalen++;
        if (ctx->datalen == SHA256_BLOCK_SIZE) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512U;
            ctx->datalen = 0U;
        }
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t *hash)
{
    uint32_t i = ctx->datalen;

    if (ctx->datalen < 56U) {
        ctx->data[i++] = 0x80U;
        while (i < 56U) ctx->data[i++] = 0x00U;
    } else {
        ctx->data[i++] = 0x80U;
        while (i < SHA256_BLOCK_SIZE) ctx->data[i++] = 0x00U;
        sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56U);
    }

    ctx->bitlen += (uint64_t)ctx->datalen * 8U;
    ctx->data[63] = (uint8_t)(ctx->bitlen);
    ctx->data[62] = (uint8_t)(ctx->bitlen >> 8U);
    ctx->data[61] = (uint8_t)(ctx->bitlen >> 16U);
    ctx->data[60] = (uint8_t)(ctx->bitlen >> 24U);
    ctx->data[59] = (uint8_t)(ctx->bitlen >> 32U);
    ctx->data[58] = (uint8_t)(ctx->bitlen >> 40U);
    ctx->data[57] = (uint8_t)(ctx->bitlen >> 48U);
    ctx->data[56] = (uint8_t)(ctx->bitlen >> 56U);
    sha256_transform(ctx, ctx->data);

    for (i = 0U; i < 4U; i++) {
        hash[i]      = (uint8_t)(ctx->state[0] >> (24U - i*8U));
        hash[i+4U]   = (uint8_t)(ctx->state[1] >> (24U - i*8U));
        hash[i+8U]   = (uint8_t)(ctx->state[2] >> (24U - i*8U));
        hash[i+12U]  = (uint8_t)(ctx->state[3] >> (24U - i*8U));
        hash[i+16U]  = (uint8_t)(ctx->state[4] >> (24U - i*8U));
        hash[i+20U]  = (uint8_t)(ctx->state[5] >> (24U - i*8U));
        hash[i+24U]  = (uint8_t)(ctx->state[6] >> (24U - i*8U));
        hash[i+28U]  = (uint8_t)(ctx->state[7] >> (24U - i*8U));
    }
}

/* -----------------------------------------------------------------------
 * HMAC-SHA256
 * ----------------------------------------------------------------------- */
static void hmac_sha256(const uint8_t *key,   size_t key_len,
                        const uint8_t *msg,   size_t msg_len,
                        uint8_t       *digest)
{
    uint8_t k_ipad[SHA256_BLOCK_SIZE];
    uint8_t k_opad[SHA256_BLOCK_SIZE];
    uint8_t tk[SHA256_DIGEST_SIZE];
    sha256_ctx_t ctx;

    /* 若密钥超过块大小，先对密钥做 SHA-256 */
    if (key_len > SHA256_BLOCK_SIZE) {
        sha256_init(&ctx);
        sha256_update(&ctx, key, key_len);
        sha256_final(&ctx, tk);
        key     = tk;
        key_len = SHA256_DIGEST_SIZE;
    }

    memset(k_ipad, 0x36U, SHA256_BLOCK_SIZE);
    memset(k_opad, 0x5CU, SHA256_BLOCK_SIZE);
    for (size_t i = 0U; i < key_len; i++) {
        k_ipad[i] ^= key[i];
        k_opad[i] ^= key[i];
    }

    /* 内层：H(k_ipad || msg) */
    sha256_init(&ctx);
    sha256_update(&ctx, k_ipad, SHA256_BLOCK_SIZE);
    sha256_update(&ctx, msg, msg_len);
    sha256_final(&ctx, digest);

    /* 外层：H(k_opad || inner) */
    sha256_init(&ctx);
    sha256_update(&ctx, k_opad, SHA256_BLOCK_SIZE);
    sha256_update(&ctx, digest, SHA256_DIGEST_SIZE);
    sha256_final(&ctx, digest);
}

/* -----------------------------------------------------------------------
 * 模块状态
 * ----------------------------------------------------------------------- */
static char s_key[NET_AUTH_KEY_MAX_LEN + 1U];

/* -----------------------------------------------------------------------
 * net_auth_init
 * ----------------------------------------------------------------------- */
void net_auth_init(const char *key)
{
    if (key != NULL && strlen(key) > 0U) {
        strncpy(s_key, key, NET_AUTH_KEY_MAX_LEN);
        s_key[NET_AUTH_KEY_MAX_LEN] = '\0';
    } else {
        strncpy(s_key, NET_AUTH_DEFAULT_KEY, NET_AUTH_KEY_MAX_LEN);
        s_key[NET_AUTH_KEY_MAX_LEN] = '\0';
        LOG_W(TAG, "Using default HMAC key — change in config.ini [auth] key=...");
    }
    LOG_I(TAG, "HMAC-SHA256 auth initialized");
}

/* -----------------------------------------------------------------------
 * net_auth_sign
 * 签名内容：method + ":" + path + ":" + timestamp_ms_str + ":" + body_hex_prefix
 * ----------------------------------------------------------------------- */
void net_auth_sign(const char    *method,
                   const char    *path,
                   uint32_t       ts_ms,
                   const uint8_t *body,
                   uint32_t       body_len,
                   char          *out_hex)
{
    /* 构造签名消息：method:path:timestamp */
    char msg[256];
    int  msg_len = snprintf(msg, sizeof(msg), "%s:%s:%lu",
                            method ? method : "",
                            path   ? path   : "",
                            (unsigned long)ts_ms);
    if (msg_len <= 0) {
        out_hex[0] = '\0';
        return;
    }

    /* 若有 body，追加 body 的前 32 字节（防止签名消息过长） */
    uint8_t sign_buf[256 + 32];
    size_t  sign_len = (size_t)msg_len;
    memcpy(sign_buf, msg, sign_len);
    if (body != NULL && body_len > 0U) {
        sign_buf[sign_len++] = ':';
        uint32_t body_prefix = (body_len < 32U) ? body_len : 32U;
        memcpy(sign_buf + sign_len, body, body_prefix);
        sign_len += body_prefix;
    }

    /* 计算 HMAC-SHA256 */
    uint8_t digest[SHA256_DIGEST_SIZE];
    hmac_sha256((const uint8_t *)s_key, strlen(s_key),
                sign_buf, sign_len, digest);

    /* 转换为 Hex 字符串 */
    for (uint32_t i = 0U; i < SHA256_DIGEST_SIZE; i++) {
        snprintf(out_hex + i * 2U, 3U, "%02x", digest[i]);
    }
    out_hex[HMAC_HEX_LEN - 1U] = '\0';
}

/* -----------------------------------------------------------------------
 * net_auth_build_header
 * 构造带 HMAC 认证头的完整 HTTP 请求头
 * ----------------------------------------------------------------------- */
int net_auth_build_header(const char *method,
                          const char *path,
                          const char *host,
                          const char *content_type,
                          uint32_t    body_len,
                          char       *out_buf,
                          size_t      out_size)
{
    uint32_t ts_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    char hmac_hex[HMAC_HEX_LEN];
    net_auth_sign(method, path, ts_ms, NULL, 0U, hmac_hex);

    int len;
    if (content_type != NULL && body_len > 0U) {
        len = snprintf(out_buf, out_size,
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %lu\r\n"
            "X-Timestamp: %lu\r\n"
            "X-HMAC-SHA256: %s\r\n"
            "\r\n",
            method, path, host, content_type,
            (unsigned long)body_len,
            (unsigned long)ts_ms,
            hmac_hex);
    } else {
        len = snprintf(out_buf, out_size,
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "X-Timestamp: %lu\r\n"
            "X-HMAC-SHA256: %s\r\n"
            "\r\n",
            method, path, host,
            (unsigned long)ts_ms,
            hmac_hex);
    }

    return (len > 0 && (size_t)len < out_size) ? len : -1;
}
