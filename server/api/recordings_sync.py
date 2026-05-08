"""
recordings_sync.py — 录像文件索引同步

MCU 通过 POST /recordings/sync 上报新录像文件信息，
服务器将其写入 SQLite 索引，供 /api/recordings 查询。

同时提供录像文件扫描功能：
  扫描 RECORDINGS_DIR 目录，将未索引的文件补录到数据库。
"""

import logging
import re
from datetime import datetime, timezone
from pathlib import Path

import aiosqlite
from fastapi import APIRouter, Depends, HTTPException, Request, status
from pydantic import BaseModel
from typing import List, Optional

import config
import models
from api.auth import require_auth
from api.auth_hmac import verify_mcu_request

logger = logging.getLogger("recordings_sync")
router = APIRouter(prefix="/api", dependencies=[Depends(require_auth)])

# 录像文件名正则：REC_YYYYMMDD_HHMMSS[_NN].mjpeg
_REC_PATTERN = re.compile(
    r"^REC_(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})(?:_\d+)?\.mjpeg$",
    re.IGNORECASE,
)


def _parse_recording_timestamp(filename: str) -> Optional[str]:
    """从文件名解析 ISO 8601 时间戳，失败返回 None。"""
    m = _REC_PATTERN.match(filename)
    if not m:
        return None
    year, month, day, hour, minute, second = (int(x) for x in m.groups())
    try:
        dt = datetime(year, month, day, hour, minute, second, tzinfo=timezone.utc)
        return dt.isoformat()
    except ValueError:
        return None


# ---------------------------------------------------------------------------
# MCU 上报录像文件信息
# ---------------------------------------------------------------------------

class RecordingSyncItem(BaseModel):
    filename: str
    size_bytes: int
    start_time: Optional[str] = None   # ISO 8601，可由服务器从文件名解析
    end_time:   Optional[str] = None


class RecordingSyncRequest(BaseModel):
    recordings: List[RecordingSyncItem]


@router.post(
    "/recordings/sync",
    summary="MCU 上报录像文件索引",
    description="MCU 在录像文件创建/关闭时调用，将文件信息写入服务器 SQLite 索引。",
)
async def sync_recordings(
    request: Request,
    body: RecordingSyncRequest,
) -> dict:
    # 验证 HMAC（MCU 请求）
    ts  = request.headers.get("X-Timestamp")
    sig = request.headers.get("X-HMAC-SHA256")
    raw_body = await request.body()
    ok, reason = verify_mcu_request("POST", "/api/recordings/sync", ts, sig, raw_body)
    if not ok:
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail=reason)

    synced = 0
    async with aiosqlite.connect(config.DB_PATH) as db:
        for item in body.recordings:
            # 防止路径穿越
            if "/" in item.filename or "\\" in item.filename:
                logger.warning("Skipping invalid filename: %s", item.filename)
                continue

            # 从文件名解析时间戳（若未提供）
            start_time = item.start_time or _parse_recording_timestamp(item.filename)
            if not start_time:
                start_time = datetime.now(timezone.utc).isoformat()
                logger.warning("Cannot parse timestamp from %s, using now", item.filename)

            await db.execute(
                "INSERT OR IGNORE INTO recordings "
                "(filename, start_time, end_time, size_bytes, storage) "
                "VALUES (?, ?, ?, ?, 'sd')",
                (item.filename, start_time, item.end_time, item.size_bytes),
            )
            synced += 1

        await db.commit()

    logger.info("Synced %d recording(s) from MCU", synced)
    return {"synced": synced}


# ---------------------------------------------------------------------------
# 扫描本地目录，补录未索引的文件
# ---------------------------------------------------------------------------

@router.post(
    "/recordings/scan",
    summary="扫描录像目录，补录未索引文件",
)
async def scan_recordings() -> dict:
    """
    扫描 RECORDINGS_DIR，将未在 SQLite 中的 .mjpeg 文件补录到索引。
    用于服务器重启后恢复索引，或手动从 SD 卡复制文件后同步。
    """
    if not config.RECORDINGS_DIR.exists():
        return {"scanned": 0, "added": 0}

    added = 0
    scanned = 0

    async with aiosqlite.connect(config.DB_PATH) as db:
        for f in config.RECORDINGS_DIR.iterdir():
            if not f.is_file():
                continue
            if not f.name.lower().endswith(".mjpeg"):
                continue

            scanned += 1
            # 检查是否已在索引中
            cursor = await db.execute(
                "SELECT id FROM recordings WHERE filename=?", (f.name,)
            )
            row = await cursor.fetchone()
            if row:
                continue  # 已索引，跳过

            start_time = _parse_recording_timestamp(f.name)
            if not start_time:
                start_time = datetime.fromtimestamp(
                    f.stat().st_mtime, tz=timezone.utc
                ).isoformat()

            await db.execute(
                "INSERT OR IGNORE INTO recordings "
                "(filename, start_time, size_bytes, storage) VALUES (?, ?, ?, 'server')",
                (f.name, start_time, f.stat().st_size),
            )
            added += 1

        await db.commit()

    logger.info("Scan complete: %d files scanned, %d added to index", scanned, added)
    return {"scanned": scanned, "added": added}
