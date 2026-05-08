"""
recordings.py — 历史录像管理 API

GET  /api/recordings?start=<ISO8601>&end=<ISO8601>
     → 200 RecordingListResponse
     → 400 时间范围参数格式错误

GET  /api/recordings/{filename}
     → 200 Transfer-Encoding: chunked（流式下载/播放）
     → 404 文件不存在
"""

import logging
from datetime import datetime, timezone
from typing import Optional

import aiofiles
import aiosqlite
from fastapi import APIRouter, Depends, HTTPException, Query, status
from fastapi.responses import StreamingResponse

import config
import models
from api.auth import require_auth

logger = logging.getLogger("api.recordings")
router = APIRouter(prefix="/api", dependencies=[Depends(require_auth)])

CHUNK_SIZE = 64 * 1024  # 64KB 分块传输


def _parse_iso8601(value: str, param_name: str) -> datetime:
    """解析 ISO 8601 时间字符串，失败则抛出 400。"""
    for fmt in (
        "%Y-%m-%dT%H:%M:%SZ",
        "%Y-%m-%dT%H:%M:%S",
        "%Y-%m-%dT%H:%M:%S.%fZ",
        "%Y-%m-%dT%H:%M:%S.%f",
        "%Y-%m-%d",
    ):
        try:
            return datetime.strptime(value, fmt).replace(tzinfo=timezone.utc)
        except ValueError:
            continue
    raise HTTPException(
        status_code=status.HTTP_400_BAD_REQUEST,
        detail=f"Parameter '{param_name}' is not a valid ISO 8601 datetime: {value!r}",
    )


@router.get(
    "/recordings",
    response_model=models.RecordingListResponse,
    summary="获取录像文件列表",
)
async def list_recordings(
    start: Optional[str] = Query(None, description="开始时间（ISO 8601）"),
    end:   Optional[str] = Query(None, description="结束时间（ISO 8601）"),
) -> models.RecordingListResponse:
    # 解析并校验时间范围参数
    start_dt: Optional[datetime] = None
    end_dt:   Optional[datetime] = None

    if start is not None:
        start_dt = _parse_iso8601(start, "start")
    if end is not None:
        end_dt = _parse_iso8601(end, "end")

    if start_dt and end_dt and start_dt > end_dt:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="'start' must be earlier than 'end'",
        )

    # 查询数据库
    async with aiosqlite.connect(config.DB_PATH) as db:
        db.row_factory = aiosqlite.Row

        if start_dt and end_dt:
            cursor = await db.execute(
                "SELECT filename, start_time, end_time, size_bytes, storage "
                "FROM recordings "
                "WHERE start_time >= ? AND start_time <= ? "
                "ORDER BY start_time DESC",
                (start_dt.isoformat(), end_dt.isoformat()),
            )
        elif start_dt:
            cursor = await db.execute(
                "SELECT filename, start_time, end_time, size_bytes, storage "
                "FROM recordings WHERE start_time >= ? ORDER BY start_time DESC",
                (start_dt.isoformat(),),
            )
        elif end_dt:
            cursor = await db.execute(
                "SELECT filename, start_time, end_time, size_bytes, storage "
                "FROM recordings WHERE start_time <= ? ORDER BY start_time DESC",
                (end_dt.isoformat(),),
            )
        else:
            cursor = await db.execute(
                "SELECT filename, start_time, end_time, size_bytes, storage "
                "FROM recordings ORDER BY start_time DESC"
            )

        rows = await cursor.fetchall()

    recordings = [
        models.RecordingInfo(
            filename=row["filename"],
            start_time=row["start_time"],
            end_time=row["end_time"],
            size_bytes=row["size_bytes"],
            storage=row["storage"],
        )
        for row in rows
    ]

    return models.RecordingListResponse(recordings=recordings, total=len(recordings))


@router.get(
    "/recordings/{filename}",
    summary="下载或在线播放录像文件",
)
async def get_recording(filename: str) -> StreamingResponse:
    # 防止路径穿越攻击
    if "/" in filename or "\\" in filename or ".." in filename:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="Invalid filename",
        )

    file_path = config.RECORDINGS_DIR / filename
    if not file_path.exists() or not file_path.is_file():
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Recording '{filename}' not found",
        )

    file_size = file_path.stat().st_size

    async def file_generator():
        async with aiofiles.open(file_path, "rb") as f:
            while True:
                chunk = await f.read(CHUNK_SIZE)
                if not chunk:
                    break
                yield chunk

    # 根据文件扩展名决定 Content-Type
    media_type = "video/x-mjpeg" if filename.endswith(".mjpeg") else "application/octet-stream"

    return StreamingResponse(
        file_generator(),
        media_type=media_type,
        headers={
            "Content-Disposition": f'attachment; filename="{filename}"',
            "Content-Length": str(file_size),
            "Accept-Ranges": "bytes",
        },
    )
