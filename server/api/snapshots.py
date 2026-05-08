"""
snapshots.py — 远程拍照 API

POST /api/snapshot
     Body: SnapshotRequest（可选 quality/width/height）
     → 200 SnapshotResponse（含照片 URL）
     → 503 MCU 未连接
     → 504 MCU 超时未响应

GET  /api/snapshots/{filename}
     → 200 image/jpeg
     → 404 文件不存在
"""

import asyncio
import logging
from datetime import datetime, timezone

import aiofiles
from fastapi import APIRouter, Depends, HTTPException, status
from fastapi.responses import FileResponse

import config
import models
import stream_receiver
from api.auth import require_auth

logger = logging.getLogger("api.snapshots")
router = APIRouter(prefix="/api", dependencies=[Depends(require_auth)])


@router.post(
    "/snapshot",
    response_model=models.SnapshotResponse,
    summary="远程触发拍照",
)
async def take_snapshot(
    req: models.SnapshotRequest = models.SnapshotRequest(),
) -> models.SnapshotResponse:
    # 检查 MCU 是否在线
    if not stream_receiver.mcu_connected:
        raise HTTPException(
            status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
            detail="Camera is offline. MCU not connected.",
        )

    # 创建 Future，等待 MCU 返回照片数据
    loop = asyncio.get_event_loop()
    future: asyncio.Future = loop.create_future()
    stream_receiver.snapshot_future = future

    # 向 MCU 发送拍照命令
    cmd = {
        "cmd": "snapshot",
        "quality": req.quality,
        "width": req.width,
        "height": req.height,
    }
    sent = await stream_receiver.send_command_to_mcu(cmd)
    if not sent:
        stream_receiver.snapshot_future = None
        raise HTTPException(
            status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
            detail="Failed to send snapshot command to MCU",
        )

    # 等待 MCU 返回照片（超时 SNAPSHOT_TIMEOUT_S 秒）
    try:
        jpeg_data, sd_failed = await asyncio.wait_for(
            future, timeout=config.SNAPSHOT_TIMEOUT_S
        )
    except asyncio.TimeoutError:
        stream_receiver.snapshot_future = None
        raise HTTPException(
            status_code=status.HTTP_504_GATEWAY_TIMEOUT,
            detail=f"MCU did not respond within {config.SNAPSHOT_TIMEOUT_S}s",
        )
    finally:
        stream_receiver.snapshot_future = None

    # 生成文件名并保存到服务器本地
    now = datetime.now(timezone.utc)
    filename = f"SNAP_{now.strftime('%Y%m%d_%H%M%S')}.jpg"
    file_path = config.SNAPSHOTS_DIR / filename
    config.SNAPSHOTS_DIR.mkdir(parents=True, exist_ok=True)

    async with aiofiles.open(file_path, "wb") as f:
        await f.write(jpeg_data)

    size_bytes = len(jpeg_data)

    # 写入数据库索引
    await models.insert_snapshot(
        filename=filename,
        taken_at=now.isoformat(),
        size_bytes=size_bytes,
        sd_saved=not sd_failed,
    )

    if sd_failed:
        logger.warning("Snapshot '%s' was NOT saved to MCU SD card", filename)
    else:
        logger.info("Snapshot '%s' saved (%d bytes)", filename, size_bytes)

    return models.SnapshotResponse(
        url=f"/api/snapshots/{filename}",
        filename=filename,
        size_bytes=size_bytes,
        sd_saved=not sd_failed,
    )


@router.get(
    "/snapshots/{filename}",
    summary="获取照片文件",
)
async def get_snapshot(filename: str) -> FileResponse:
    # 防止路径穿越攻击
    if "/" in filename or "\\" in filename or ".." in filename:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="Invalid filename",
        )

    file_path = config.SNAPSHOTS_DIR / filename
    if not file_path.exists() or not file_path.is_file():
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Snapshot '{filename}' not found",
        )

    return FileResponse(
        path=file_path,
        media_type="image/jpeg",
        filename=filename,
    )
