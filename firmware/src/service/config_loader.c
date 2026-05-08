/**
 * @file config_loader.c
 * @brief 配置文件加载器实现
 *
 * 从 SD 卡根目录 /config.ini 读取 INI 格式配置，
 * 解析失败时回退到编译期默认值。
 */

#include "config_loader.h"
#include "log.h"
#include "ff.h"       /* FatFs */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#define TAG "CFG"

#define CONFIG_FILE_PATH   "0:/config.ini"
#define INI_LINE_MAX       128U

/* -----------------------------------------------------------------------
 * 全局配置实例
 * ----------------------------------------------------------------------- */
ipcam_config_t g_ipcam_config;

/* -----------------------------------------------------------------------
 * 私有辅助函数
 * ----------------------------------------------------------------------- */

/** 去除字符串首尾空白 */
static char *str_trim(char *s)
{
    /* 去首部空白 */
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    /* 去尾部空白 */
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end-- = '\0';
    }
    return s;
}

/** 解析单行 key=value，返回 key 和 value 的指针（原地修改 line） */
static bool parse_kv(char *line, char **key, char **value)
{
    char *eq = strchr(line, '=');
    if (eq == NULL) {
        return false;
    }
    *eq = '\0';
    *key   = str_trim(line);
    *value = str_trim(eq + 1);
    return (strlen(*key) > 0U);
}

/** 验证并应用单个配置项 */
static void apply_kv(const char *section, const char *key, const char *value)
{
    if (strcmp(section, "wifi") == 0) {
        if (strcmp(key, "ssid") == 0) {
            if (strlen(value) > 0U && strlen(value) < IPCAM_SSID_MAX_LEN) {
                strncpy(g_ipcam_config.wifi_ssid, value, IPCAM_SSID_MAX_LEN - 1U);
                g_ipcam_config.wifi_ssid[IPCAM_SSID_MAX_LEN - 1U] = '\0';
            } else {
                LOG_W(TAG, "wifi.ssid invalid length, using default");
            }
        } else if (strcmp(key, "password") == 0) {
            if (strlen(value) < IPCAM_PASS_MAX_LEN) {
                strncpy(g_ipcam_config.wifi_password, value, IPCAM_PASS_MAX_LEN - 1U);
                g_ipcam_config.wifi_password[IPCAM_PASS_MAX_LEN - 1U] = '\0';
            }
        }
    } else if (strcmp(section, "server") == 0) {
        if (strcmp(key, "ip") == 0) {
            if (strlen(value) > 0U && strlen(value) < IPCAM_IP_MAX_LEN) {
                strncpy(g_ipcam_config.server_ip, value, IPCAM_IP_MAX_LEN - 1U);
                g_ipcam_config.server_ip[IPCAM_IP_MAX_LEN - 1U] = '\0';
            }
        } else if (strcmp(key, "port") == 0) {
            long port = strtol(value, NULL, 10);
            if (port > 0 && port <= 65535) {
                g_ipcam_config.server_port = (uint16_t)port;
            } else {
                LOG_W(TAG, "server.port out of range [1,65535], using default %u",
                      IPCAM_DEFAULT_SERVER_PORT);
            }
        }
    } else if (strcmp(section, "camera") == 0) {
        if (strcmp(key, "fps") == 0) {
            long fps = strtol(value, NULL, 10);
            if (fps >= 1 && fps <= 30) {
                g_ipcam_config.target_fps = (uint8_t)fps;
            } else {
                LOG_W(TAG, "camera.fps out of range [1,30], using default %u",
                      IPCAM_DEFAULT_TARGET_FPS);
            }
        } else if (strcmp(key, "quality") == 0) {
            long q = strtol(value, NULL, 10);
            if (q >= 50 && q <= 95) {
                g_ipcam_config.jpeg_quality = (uint8_t)q;
            } else {
                LOG_W(TAG, "camera.quality out of range [50,95], using default %u",
                      IPCAM_DEFAULT_JPEG_QUALITY);
            }
        }
    }
}

/* -----------------------------------------------------------------------
 * config_reset_to_default
 * ----------------------------------------------------------------------- */
void config_reset_to_default(void)
{
    memset(&g_ipcam_config, 0, sizeof(g_ipcam_config));
    strncpy(g_ipcam_config.wifi_ssid,      IPCAM_DEFAULT_WIFI_SSID,     IPCAM_SSID_MAX_LEN - 1U);
    strncpy(g_ipcam_config.wifi_password,  IPCAM_DEFAULT_WIFI_PASSWORD, IPCAM_PASS_MAX_LEN - 1U);
    strncpy(g_ipcam_config.server_ip,      IPCAM_DEFAULT_SERVER_IP,     IPCAM_IP_MAX_LEN  - 1U);
    g_ipcam_config.server_port   = IPCAM_DEFAULT_SERVER_PORT;
    g_ipcam_config.jpeg_quality  = IPCAM_DEFAULT_JPEG_QUALITY;
    g_ipcam_config.target_fps    = IPCAM_DEFAULT_TARGET_FPS;
}

/* -----------------------------------------------------------------------
 * config_load
 * ----------------------------------------------------------------------- */
ipcam_status_t config_load(void)
{
    /* 先填充默认值，后续逐项覆盖 */
    config_reset_to_default();

    FIL     fil;
    FRESULT fr = f_open(&fil, CONFIG_FILE_PATH, FA_READ);
    if (fr != FR_OK) {
        LOG_W(TAG, "config.ini not found (FRESULT=%d), using defaults", (int)fr);
        return IPCAM_ERR_NOT_FOUND;
    }

    char    line[INI_LINE_MAX];
    char    section[32] = {0};
    bool    parse_error = false;

    while (f_gets(line, sizeof(line), &fil) != NULL) {
        char *p = str_trim(line);

        /* 跳过空行和注释 */
        if (*p == '\0' || *p == ';' || *p == '#') {
            continue;
        }

        /* 解析 section 头 [section] */
        if (*p == '[') {
            char *end = strchr(p, ']');
            if (end != NULL) {
                *end = '\0';
                strncpy(section, p + 1, sizeof(section) - 1U);
                section[sizeof(section) - 1U] = '\0';
                /* 转小写 */
                for (char *c = section; *c; c++) {
                    *c = (char)tolower((unsigned char)*c);
                }
            }
            continue;
        }

        /* 解析 key=value */
        char *key = NULL, *value = NULL;
        if (parse_kv(p, &key, &value)) {
            /* 转 key 为小写 */
            for (char *c = key; *c; c++) {
                *c = (char)tolower((unsigned char)*c);
            }
            apply_kv(section, key, value);
        } else {
            LOG_W(TAG, "config.ini: malformed line: %s", p);
            parse_error = true;
        }
    }

    f_close(&fil);

    if (parse_error) {
        LOG_W(TAG, "config.ini loaded with warnings, some params use defaults");
    } else {
        LOG_I(TAG, "config.ini loaded OK: ssid=%s server=%s:%u fps=%u quality=%u",
              g_ipcam_config.wifi_ssid,
              g_ipcam_config.server_ip,
              g_ipcam_config.server_port,
              g_ipcam_config.target_fps,
              g_ipcam_config.jpeg_quality);
    }

    return IPCAM_OK;
}

/* -----------------------------------------------------------------------
 * config_save
 * ----------------------------------------------------------------------- */
ipcam_status_t config_save(void)
{
    FIL     fil;
    FRESULT fr = f_open(&fil, CONFIG_FILE_PATH, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        LOG_E(TAG, "config_save: f_open failed (%d)", (int)fr);
        return IPCAM_ERR_IO;
    }

    UINT bw;
    char buf[256];
    int  len;

    len = snprintf(buf, sizeof(buf),
                   "[wifi]\r\n"
                   "ssid     = %s\r\n"
                   "password = %s\r\n"
                   "\r\n"
                   "[server]\r\n"
                   "ip       = %s\r\n"
                   "port     = %u\r\n"
                   "\r\n"
                   "[camera]\r\n"
                   "fps      = %u\r\n"
                   "quality  = %u\r\n",
                   g_ipcam_config.wifi_ssid,
                   g_ipcam_config.wifi_password,
                   g_ipcam_config.server_ip,
                   g_ipcam_config.server_port,
                   g_ipcam_config.target_fps,
                   g_ipcam_config.jpeg_quality);

    if (len > 0) {
        f_write(&fil, buf, (UINT)len, &bw);
    }

    f_close(&fil);
    LOG_I(TAG, "config.ini saved");
    return IPCAM_OK;
}
