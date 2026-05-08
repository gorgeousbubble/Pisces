"""
models.py — Pydantic 数据模型与 SQLite 数据库初始化
"""

from __future__ import annotations

import aiosqlite
from datetime import datetime
from typing import Optional, List
from pydantic import BaseModel

import config


# ---------------------------------------------------------------------------
# Pydantic 响应模型
# ---------------------------------------------------------------------------

class RecordingInfo(BaseModel):
    filename: str
    start_time: str          # ISO 8601
    end_time: Optional[str]  # 录像结束时间，录制中为 None
    size_bytes: int
    storage: str             # "sd" | "server"


class SnapshotInfo(BaseModel):
    filename: str
    taken_at: str            # ISO 8601
    size_bytes: int
    sd_saved: bool
    url: str


class McuStatus(BaseModel):
    mcu_online: bool
    net_state: str
    fps: int
    sd_free_mb: int
    uptime_sec: int
    drop_count: int
    cam_available: bool
    sd_available: bool
    sd_low_space: bool
    last_seen: Optional[str]  # ISO 8601，MCU 离线时为最后已知时间


class SnapshotRequest(BaseModel):
    quality: Optional[int] = 85
    width: Optional[int]   = 1280
    height: Optional[int]  = 720


class SnapshotResponse(BaseModel):
    url: str
    filename: str
    size_bytes: int
    sd_saved: bool


class RecordingListResponse(BaseModel):
    recordings: List[RecordingInfo]
    total: int


class StatusResponse(BaseModel):
    status: McuStatus


# ---------------------------------------------------------------------------
# SQLite 数据库初始化
# ---------------------------------------------------------------------------

CREATE_RECORDINGS_TABLE = """
CREATE TABLE IF NOT EXISTS recordings (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    filename    TEXT NOT NULL UNIQUE,
    start_time  TEXT NOT NULL,
    end_time    TEXT,
    size_bytes  INTEGER NOT NULL DEFAULT 0,
    storage     TEXT NOT NULL DEFAULT 'sd'
);
"""

CREATE_SNAPSHOTS_TABLE = """
CREATE TABLE IF NOT EXISTS snapshots (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    filename    TEXT NOT NULL UNIQUE,
    taken_at    TEXT NOT NULL,
    size_bytes  INTEGER NOT NULL DEFAULT 0,
    sd_saved    INTEGER NOT NULL DEFAULT 1
);
"""

CREATE_STATUS_LOG_TABLE = """
CREATE TABLE IF NOT EXISTS mcu_status_log (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    recorded_at TEXT NOT NULL,
    fps         INTEGER,
    sd_free_mb  INTEGER,
    drop_count  INTEGER,
    net_state   TEXT,
    cam_available INTEGER,
    sd_available  INTEGER
);
"""

CREATE_RECORDINGS_IDX = """
CREATE INDEX IF NOT EXISTS idx_recordings_start_time
ON recordings (start_time);
"""

CREATE_SNAPSHOTS_IDX = """
CREATE INDEX IF NOT EXISTS idx_snapshots_taken_at
ON snapshots (taken_at);
"""


async def init_db() -> None:
    """创建数据库表（幂等，可重复调用）。"""
    config.DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    async with aiosqlite.connect(config.DB_PATH) as db:
        await db.execute(CREATE_RECORDINGS_TABLE)
        await db.execute(CREATE_SNAPSHOTS_TABLE)
        await db.execute(CREATE_STATUS_LOG_TABLE)
        await db.execute(CREATE_RECORDINGS_IDX)
        await db.execute(CREATE_SNAPSHOTS_IDX)
        await db.commit()


async def insert_recording(filename: str, start_time: str,
                           size_bytes: int = 0,
                           storage: str = "sd") -> None:
    async with aiosqlite.connect(config.DB_PATH) as db:
        await db.execute(
            "INSERT OR IGNORE INTO recordings (filename, start_time, size_bytes, storage) "
            "VALUES (?, ?, ?, ?)",
            (filename, start_time, size_bytes, storage),
        )
        await db.commit()


async def update_recording_end(filename: str, end_time: str,
                               size_bytes: int) -> None:
    async with aiosqlite.connect(config.DB_PATH) as db:
        await db.execute(
            "UPDATE recordings SET end_time=?, size_bytes=? WHERE filename=?",
            (end_time, size_bytes, filename),
        )
        await db.commit()


async def insert_snapshot(filename: str, taken_at: str,
                          size_bytes: int, sd_saved: bool) -> None:
    async with aiosqlite.connect(config.DB_PATH) as db:
        await db.execute(
            "INSERT OR IGNORE INTO snapshots (filename, taken_at, size_bytes, sd_saved) "
            "VALUES (?, ?, ?, ?)",
            (filename, taken_at, size_bytes, int(sd_saved)),
        )
        await db.commit()


async def insert_status_log(status: dict) -> None:
    """写入状态日志，并保留最近 N 条。"""
    async with aiosqlite.connect(config.DB_PATH) as db:
        await db.execute(
            "INSERT INTO mcu_status_log "
            "(recorded_at, fps, sd_free_mb, drop_count, net_state, cam_available, sd_available) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)",
            (
                datetime.utcnow().isoformat() + "Z",
                status.get("fps", 0),
                status.get("sd_free_mb", 0),
                status.get("drop_count", 0),
                status.get("net_state", "unknown"),
                int(status.get("cam_available", True)),
                int(status.get("sd_available", True)),
            ),
        )
        # 保留最近 MAX 条
        await db.execute(
            "DELETE FROM mcu_status_log WHERE id NOT IN "
            "(SELECT id FROM mcu_status_log ORDER BY id DESC LIMIT ?)",
            (config.STATUS_LOG_MAX_ROWS,),
        )
        await db.commit()
