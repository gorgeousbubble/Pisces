"""
config.py — 服务器端配置管理

优先级：环境变量 > .env 文件 > 代码默认值
生产环境通过环境变量注入敏感参数（AUTH_PASSWORD 等）。
"""

import os
from pathlib import Path
from dotenv import load_dotenv

# 加载 .env 文件（开发环境使用，生产环境用真实环境变量）
load_dotenv(Path(__file__).parent / ".env")


def _env(key: str, default: str) -> str:
    return os.environ.get(key, default)


def _env_int(key: str, default: int) -> int:
    try:
        return int(os.environ.get(key, str(default)))
    except ValueError:
        return default


def _env_bool(key: str, default: bool) -> bool:
    val = os.environ.get(key, str(default)).lower()
    return val in ("1", "true", "yes")


# ---------------------------------------------------------------------------
# MCU 推流接收
# ---------------------------------------------------------------------------
MCU_STREAM_HOST: str = _env("MCU_STREAM_HOST", "0.0.0.0")
MCU_STREAM_PORT: int = _env_int("MCU_STREAM_PORT", 8554)

# MCU 心跳超时：超过此时间未收到帧则标记为离线
MCU_HEARTBEAT_TIMEOUT_S: int = _env_int("MCU_HEARTBEAT_TIMEOUT_S", 10)

# ---------------------------------------------------------------------------
# FastAPI HTTP 服务
# ---------------------------------------------------------------------------
API_HOST: str = _env("API_HOST", "0.0.0.0")
API_PORT: int = _env_int("API_PORT", 8000)

# ---------------------------------------------------------------------------
# 并发客户端限制
# ---------------------------------------------------------------------------
MAX_STREAM_CLIENTS: int = _env_int("MAX_STREAM_CLIENTS", 10)

# frame_buffer 队列深度（每个客户端独立队列）
FRAME_QUEUE_SIZE: int = _env_int("FRAME_QUEUE_SIZE", 5)

# ---------------------------------------------------------------------------
# 传输大小上限（防止损坏/恶意数据导致内存耗尽）
# ---------------------------------------------------------------------------
# 单帧 MJPEG 上限（MCU 720P JPEG 一般 <100KB，留足余量）
MAX_FRAME_SIZE: int = _env_int("MAX_FRAME_SIZE", 512 * 1024)
# MJPEG 解析缓冲总上限：超过则判定流损坏并断开连接
MAX_STREAM_BUF_SIZE: int = _env_int("MAX_STREAM_BUF_SIZE", 1024 * 1024)
# 拍照上传上限
MAX_SNAPSHOT_SIZE: int = _env_int("MAX_SNAPSHOT_SIZE", 2 * 1024 * 1024)
# 状态 JSON 上限
MAX_STATUS_BODY_SIZE: int = _env_int("MAX_STATUS_BODY_SIZE", 8 * 1024)

# ---------------------------------------------------------------------------
# 存储路径
# ---------------------------------------------------------------------------
STORAGE_PATH: Path = Path(_env("STORAGE_PATH", "/var/ipcam"))
RECORDINGS_DIR: Path = STORAGE_PATH / "recordings"
SNAPSHOTS_DIR: Path  = STORAGE_PATH / "snapshots"
DB_PATH: Path        = STORAGE_PATH / "ipcam.db"

# ---------------------------------------------------------------------------
# SD 卡空间告警阈值
# ---------------------------------------------------------------------------
SD_LOW_SPACE_MB: int = _env_int("SD_LOW_SPACE_MB", 50)

# ---------------------------------------------------------------------------
# 身份认证（外网访问保护）
# ---------------------------------------------------------------------------
AUTH_ENABLED: bool  = _env_bool("AUTH_ENABLED", True)
AUTH_USERNAME: str  = _env("AUTH_USERNAME", "admin")
AUTH_PASSWORD: str  = _env("AUTH_PASSWORD", "changeme")  # 生产环境必须通过环境变量覆盖

# ---------------------------------------------------------------------------
# CORS 允许的源（逗号分隔）
# 默认 "*"（局域网便利）；外网部署应设为具体域名白名单。
# 注意：为安全起见 allow_credentials 固定为 False——"*" 与凭证共存会让任意
# 站点带凭证跨源访问。Basic Auth 通过 Authorization 头工作，不依赖该开关。
# ---------------------------------------------------------------------------
CORS_ALLOW_ORIGINS: list = [
    o.strip() for o in _env("CORS_ALLOW_ORIGINS", "*").split(",") if o.strip()
]

# ---------------------------------------------------------------------------
# 拍照超时
# ---------------------------------------------------------------------------
SNAPSHOT_TIMEOUT_S: float = float(_env("SNAPSHOT_TIMEOUT_S", "3.0"))

# ---------------------------------------------------------------------------
# 状态历史保留条数
# ---------------------------------------------------------------------------
STATUS_LOG_MAX_ROWS: int = _env_int("STATUS_LOG_MAX_ROWS", 100)
