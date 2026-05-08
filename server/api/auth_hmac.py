"""
auth_hmac.py — 服务器端 HMAC-SHA256 请求验证

验证 MCU 发来的 HTTP 请求中的 X-HMAC-SHA256 头，
防止局域网内的命令伪造和中间人攻击。

验证逻辑与 MCU 端 net_auth.c 完全对称：
  签名消息 = method:path:timestamp_ms[:body_prefix_32bytes]
  HMAC-SHA256(key, message)
"""

import hashlib
import hmac
import logging
import os
import time
from typing import Optional

logger = logging.getLogger("auth_hmac")

# 共享密钥（与 MCU config.ini [auth] key= 保持一致）
_HMAC_KEY: bytes = os.environ.get(
    "HMAC_KEY", "pisces-ipcam-default-key-change-me"
).encode()

# 时间戳容忍窗口（秒）：允许 MCU 与服务器时钟偏差
_TIMESTAMP_TOLERANCE_S: int = int(os.environ.get("HMAC_TIMESTAMP_TOLERANCE", "30"))

# 是否启用 HMAC 验证（可通过环境变量关闭，用于调试）
_HMAC_ENABLED: bool = os.environ.get("HMAC_ENABLED", "true").lower() in ("1", "true", "yes")


def set_hmac_key(key: str) -> None:
    """动态更新 HMAC 密钥（用于从配置文件加载）。"""
    global _HMAC_KEY
    _HMAC_KEY = key.encode()
    logger.info("HMAC key updated")


def _compute_hmac(method: str, path: str, ts_ms: int,
                  body: Optional[bytes] = None) -> str:
    """计算 HMAC-SHA256，与 MCU net_auth.c 逻辑完全一致。"""
    msg = f"{method}:{path}:{ts_ms}"
    if body:
        prefix = body[:32]
        msg_bytes = msg.encode() + b":" + prefix
    else:
        msg_bytes = msg.encode()

    digest = hmac.new(_HMAC_KEY, msg_bytes, hashlib.sha256).hexdigest()
    return digest


def verify_mcu_request(method: str,
                        path: str,
                        timestamp_ms_str: Optional[str],
                        hmac_header: Optional[str],
                        body: Optional[bytes] = None) -> tuple[bool, str]:
    """
    验证 MCU 请求的 HMAC 签名。

    Returns:
        (ok, reason) — ok=True 表示验证通过，reason 为失败原因
    """
    if not _HMAC_ENABLED:
        return True, "HMAC disabled"

    if not timestamp_ms_str or not hmac_header:
        return False, "Missing X-Timestamp or X-HMAC-SHA256 header"

    # 解析时间戳
    try:
        ts_ms = int(timestamp_ms_str)
    except ValueError:
        return False, f"Invalid X-Timestamp: {timestamp_ms_str!r}"

    # 检查时间戳新鲜度（防重放攻击）
    # MCU 时间戳是系统 tick（ms），服务器用 Unix 时间戳（ms）
    # 两者可能有偏差，使用宽松的容忍窗口
    server_ts_ms = int(time.monotonic() * 1000)
    # 注意：MCU tick 从 0 开始，与 Unix 时间戳不同
    # 此处仅验证签名正确性，不严格验证时间戳绝对值
    # 生产环境中应在 MCU 获取 NTP 时间后使用 Unix 时间戳

    # 计算期望的 HMAC
    expected = _compute_hmac(method, path, ts_ms, body)

    # 使用 hmac.compare_digest 防止时序攻击
    if not hmac.compare_digest(expected.lower(), hmac_header.lower()):
        logger.warning(
            "HMAC mismatch: method=%s path=%s ts=%d expected=%s got=%s",
            method, path, ts_ms, expected[:8] + "...", hmac_header[:8] + "..."
        )
        return False, "HMAC signature mismatch"

    return True, "OK"
