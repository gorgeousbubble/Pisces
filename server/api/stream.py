"""
stream.py — 实时视频流 API

GET /stream/live
  → multipart/x-mixed-replace; boundary=frame
  → 需要 Basic Auth（AUTH_ENABLED=True 时）
  → 503 若 MCU 未连接
  → 503 若并发客户端已达上限
"""

import asyncio
import logging
from typing import AsyncGenerator

from fastapi import APIRouter, Depends, HTTPException, status
from fastapi.responses import StreamingResponse

import config
import stream_receiver
from api.auth import require_auth

logger = logging.getLogger("api.stream")
router = APIRouter()

BOUNDARY = b"frame"
PART_HEADER_TEMPLATE = (
    b"--frame\r\n"
    b"Content-Type: image/jpeg\r\n"
    b"Content-Length: {size}\r\n"
    b"\r\n"
)


async def _mjpeg_generator(queue: asyncio.Queue) -> AsyncGenerator[bytes, None]:
    """从客户端专属队列中读取帧，生成 multipart 响应体。"""
    try:
        while True:
            try:
                frame: bytes = await asyncio.wait_for(queue.get(), timeout=10.0)
            except asyncio.TimeoutError:
                # 超时：发送空边界保持连接，让客户端检测断线
                yield b"--frame\r\n\r\n"
                continue

            part_header = PART_HEADER_TEMPLATE.replace(
                b"{size}", str(len(frame)).encode()
            )
            yield part_header + frame + b"\r\n"

    except asyncio.CancelledError:
        pass
    finally:
        # 客户端断开：从广播列表移除队列
        if queue in stream_receiver.client_queues:
            stream_receiver.client_queues.remove(queue)
        logger.info(
            "Client disconnected, active clients: %d",
            len(stream_receiver.client_queues),
        )


@router.get(
    "/stream/live",
    summary="实时视频流",
    description="返回 MJPEG multipart 视频流。需要 Basic Auth（外网模式）。",
    dependencies=[Depends(require_auth)],
)
async def live_stream() -> StreamingResponse:
    # 检查 MCU 是否在线
    if not stream_receiver.mcu_connected:
        raise HTTPException(
            status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
            detail="Camera is offline. MCU not connected.",
        )

    # 检查并发客户端上限
    if len(stream_receiver.client_queues) >= config.MAX_STREAM_CLIENTS:
        raise HTTPException(
            status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
            detail=f"Maximum concurrent clients ({config.MAX_STREAM_CLIENTS}) reached.",
        )

    # 为该客户端创建独立帧队列
    queue: asyncio.Queue = asyncio.Queue(maxsize=config.FRAME_QUEUE_SIZE)
    stream_receiver.client_queues.append(queue)

    logger.info(
        "New stream client, active clients: %d",
        len(stream_receiver.client_queues),
    )

    return StreamingResponse(
        _mjpeg_generator(queue),
        media_type="multipart/x-mixed-replace; boundary=frame",
        headers={
            "Cache-Control": "no-cache, no-store, must-revalidate",
            "Pragma": "no-cache",
            "Expires": "0",
            "X-Accel-Buffering": "no",  # 禁用 Nginx 缓冲，保证实时性
        },
    )
