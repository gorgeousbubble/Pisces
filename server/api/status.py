"""
status.py — MCU 状态查询 API

GET /api/status
    → 200 McuStatus（MCU 在线时返回实时数据，离线时返回最后已知状态）
"""

import logging

from fastapi import APIRouter, Depends

import models
import stream_receiver
from api.auth import require_auth

logger = logging.getLogger("api.status")
router = APIRouter(prefix="/api", dependencies=[Depends(require_auth)])


@router.get(
    "/status",
    response_model=models.McuStatus,
    summary="查询 MCU 运行状态",
)
async def get_status() -> models.McuStatus:
    """
    返回 MCU 当前运行状态。
    - MCU 在线：返回最新实时数据
    - MCU 离线：返回最后已知状态，mcu_online=false
    """
    s = stream_receiver.last_known_status
    return models.McuStatus(
        mcu_online=stream_receiver.mcu_connected,
        net_state=s.get("net_state", "unknown"),
        fps=s.get("fps", 0),
        sd_free_mb=s.get("sd_free_mb", 0),
        uptime_sec=s.get("uptime_sec", 0),
        drop_count=s.get("drop_count", 0),
        cam_available=s.get("cam_available", True),
        sd_available=s.get("sd_available", True),
        sd_low_space=s.get("sd_low_space", False),
        last_seen=s.get("last_seen"),
    )
