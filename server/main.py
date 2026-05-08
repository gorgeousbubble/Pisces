"""
main.py — FastAPI 应用入口

启动顺序（lifespan）：
  1. 创建存储目录
  2. 初始化 SQLite 数据库
  3. 启动 MCU TCP 流接收服务器
  4. 启动 MCU 心跳监控后台任务
  5. 注册所有 API 路由
  6. 启动 Uvicorn HTTP 服务
"""

import asyncio
import logging
from contextlib import asynccontextmanager
from typing import AsyncGenerator

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

import config
import models
import stream_receiver
from api import stream, recordings, snapshots, status
from api import recordings_sync

# ---------------------------------------------------------------------------
# 日志配置
# ---------------------------------------------------------------------------
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    datefmt="%Y-%m-%dT%H:%M:%S",
)
logger = logging.getLogger("main")


# ---------------------------------------------------------------------------
# 应用生命周期管理
# ---------------------------------------------------------------------------
@asynccontextmanager
async def lifespan(app: FastAPI) -> AsyncGenerator[None, None]:
    # ---- 启动阶段 ----
    logger.info("=== Pisces IP Camera Server starting ===")

    # 1. 创建存储目录
    config.RECORDINGS_DIR.mkdir(parents=True, exist_ok=True)
    config.SNAPSHOTS_DIR.mkdir(parents=True, exist_ok=True)
    logger.info("Storage directories ready: %s", config.STORAGE_PATH)

    # 2. 初始化数据库
    await models.init_db()
    logger.info("Database initialized: %s", config.DB_PATH)

    # 3. 启动 MCU TCP 服务器
    tcp_server = await stream_receiver.start_tcp_server()

    # 4. 启动心跳监控后台任务
    heartbeat_task = asyncio.create_task(
        stream_receiver.heartbeat_monitor(),
        name="heartbeat_monitor",
    )

    logger.info(
        "Server ready — HTTP API on %s:%d, MCU stream on %s:%d",
        config.API_HOST, config.API_PORT,
        config.MCU_STREAM_HOST, config.MCU_STREAM_PORT,
    )
    logger.info("Auth enabled: %s", config.AUTH_ENABLED)

    yield  # 应用运行中

    # ---- 关闭阶段 ----
    logger.info("Shutting down...")
    heartbeat_task.cancel()
    try:
        await heartbeat_task
    except asyncio.CancelledError:
        pass
    tcp_server.close()
    await tcp_server.wait_closed()
    logger.info("=== Pisces IP Camera Server stopped ===")


# ---------------------------------------------------------------------------
# FastAPI 应用实例
# ---------------------------------------------------------------------------
app = FastAPI(
    title="Pisces IP Camera Server",
    description="基于 NXP K64 MCU 的家用网络摄像头监控系统服务端",
    version="1.0.0",
    lifespan=lifespan,
    docs_url="/docs",
    redoc_url="/redoc",
)

# CORS（允许局域网内浏览器直接访问）
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# ---------------------------------------------------------------------------
# 注册路由
# ---------------------------------------------------------------------------
app.include_router(stream.router,            tags=["视频流"])
app.include_router(recordings.router,        tags=["录像管理"])
app.include_router(recordings_sync.router,   tags=["录像同步"])
app.include_router(snapshots.router,         tags=["拍照"])
app.include_router(status.router,            tags=["系统状态"])


# ---------------------------------------------------------------------------
# 健康检查（无需认证）
# ---------------------------------------------------------------------------
@app.get("/health", tags=["系统状态"], summary="健康检查")
async def health_check() -> dict:
    return {
        "status": "ok",
        "mcu_online": stream_receiver.mcu_connected,
        "stream_clients": len(stream_receiver.client_queues),
    }


# ---------------------------------------------------------------------------
# 直接运行入口（开发调试用）
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    import uvicorn

    uvicorn.run(
        "main:app",
        host=config.API_HOST,
        port=config.API_PORT,
        reload=False,
        log_level="info",
    )
