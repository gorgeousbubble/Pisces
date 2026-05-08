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
# 拍照超时
# ---------------------------------------------------------------------------
SNAPSHOT_TIMEOUT_S: float = float(_env("SNAPSHOT_TIMEOUT_S", "3.0"))

# ---------------------------------------------------------------------------
# 状态历史保留条数
# ---------------------------------------------------------------------------
STATUS_LOG_MAX_ROWS: int = _env_int("STATUS_LOG_MAX_ROWS", 100)
