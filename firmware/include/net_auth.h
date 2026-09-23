#ifndef NET_AUTH_H
#define NET_AUTH_H

/**
 * @file net_auth.h
 * @brief MCU↔服务器通信 HMAC-SHA256 签名接口
 *
 * 防止局域网内的中间人攻击和命令伪造。
 * 每条 HTTP 请求附加 X-HMAC-SHA256 头，服务器验证后才处理。
 *
 * 密钥存储在 SD 卡 config.ini 的 [auth] 节，或使用编译期默认值。
 * 签名内容：HTTP 方法 + 路径 + 时间戳 + 请求体（若有）
 *
 * 实现说明：
 *   本模块使用轻量级 SHA-256 实现（不依赖 mbedTLS），
 *   代码来自 Brad Conte 的公有领域实现（约 300 行 C）。
 */

#include "ipcam_types.h"
#include "ipcam_config.h"
#include <stdint.h>
#include <stddef.h>

/* HMAC-SHA256 输出长度（字节） */
#define HMAC_SHA256_LEN   32U
/* Hex 字符串长度（32字节 * 2 + 终止符） */
#define HMAC_HEX_LEN      65U

/* 默认共享密钥与长度上限统一取自 ipcam_config.h，不再独立定义一份。
 *
 * 原先此处写死了与 IPCAM_DEFAULT_AUTH_KEY / IPCAM_AUTH_KEY_MAX_LEN 取值相同
 * 的另一套常量，必须手工保持同步：config_loader 用 IPCAM_* 填充
 * g_ipcam_config.auth_key，本模块却用 NET_AUTH_* 做比对，一旦任一处被改动
 * 就会静默不一致，"是否仍在使用默认密钥"的判断随之失效。 */
#define NET_AUTH_DEFAULT_KEY  IPCAM_DEFAULT_AUTH_KEY
#define NET_AUTH_KEY_MAX_LEN  IPCAM_AUTH_KEY_MAX_LEN

/* -----------------------------------------------------------------------
 * 函数声明
 * ----------------------------------------------------------------------- */

/**
 * @brief 初始化认证模块，加载密钥
 * @param key  共享密钥字符串（NULL 则使用默认值）
 */
void net_auth_init(const char *key);

/**
 * @brief 计算 HMAC-SHA256 并输出 Hex 字符串
 * @param method   HTTP 方法（如 "POST"）
 * @param path     请求路径（如 "/stream"）
 * @param ts_ms    当前时间戳（ms，用于防重放）
 * @param body     请求体数据（可为 NULL）
 * @param body_len 请求体长度
 * @param out_hex  输出 Hex 字符串缓冲区（至少 HMAC_HEX_LEN 字节）
 */
void net_auth_sign(const char    *method,
                   const char    *path,
                   uint32_t       ts_ms,
                   const uint8_t *body,
                   uint32_t       body_len,
                   char          *out_hex);

/**
 * @brief 构造带认证头的 HTTP 请求头字符串
 * @param method   HTTP 方法
 * @param path     请求路径
 * @param host     服务器 host:port
 * @param content_type Content-Type 值（可为 NULL）
 * @param body_len 请求体长度（0 表示无 body）
 * @param out_buf  输出缓冲区
 * @param out_size 缓冲区大小
 * @return 实际写入字节数，-1 表示缓冲区不足
 */
int net_auth_build_header(const char *method,
                          const char *path,
                          const char *host,
                          const char *content_type,
                          uint32_t    body_len,
                          char       *out_buf,
                          size_t      out_size);

#endif /* NET_AUTH_H */
