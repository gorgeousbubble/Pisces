#ifndef CONFIG_LOADER_H
#define CONFIG_LOADER_H

/**
 * @file config_loader.h
 * @brief 配置文件加载器接口
 *
 * 从 SD 卡根目录 config.ini 读取并解析系统配置参数。
 */

#include "ipcam_types.h"
#include "ipcam_config.h"

/**
 * @brief 从 SD 卡加载配置文件
 *
 * 读取 /config.ini，解析各参数并写入 g_ipcam_config。
 * 若文件不存在、解析失败或参数越界，则使用默认值并记录日志。
 *
 * @return IPCAM_OK（成功加载）/ IPCAM_ERR_NOT_FOUND（使用默认值）/ IPCAM_ERR_IO
 */
ipcam_status_t config_load(void);

/**
 * @brief 将当前配置写回 SD 卡（用于保存运行时修改）
 * @return IPCAM_OK / IPCAM_ERR_IO
 */
ipcam_status_t config_save(void);

/**
 * @brief 将 g_ipcam_config 重置为编译期默认值
 */
void config_reset_to_default(void);

#endif /* CONFIG_LOADER_H */
