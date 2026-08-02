"""
stream_receiver.py — MCU 视频流接收器

职责：
  1. 监听 TCP 端口，接受 MCU 的 MJPEG-over-HTTP 推流连接
  2. 解析 multipart/x-mixed-replace 帧边界，提取 JPEG 数据
  3. 将帧广播到所有已订阅的 Web 客户端队列
  4. 接收并解析 MCU 上报的状态 JSON
  5. 提供命令下发通道（向 MCU 发送 JSON 命令）
  6. 维护 MCU 在线/离线状态
"""

from __future__ import annotations

import asyncio
import json
import logging
import time
from typing import Optional

import config
import models

logger = logging.getLogger("stream_receiver")

# ---------------------------------------------------------------------------
# 全局状态（由 main.py 的 lifespan 初始化）
# ---------------------------------------------------------------------------

# 最新一帧 JPEG 数据（bytes）
latest_frame: Optional[bytes] = None

# 所有已订阅的 Web 客户端队列
client_queues: list[asyncio.Queue] = []

# MCU 连接状态
mcu_connected: bool = False
mcu_last_seen: float = 0.0          # time.monotonic() 时间戳
mcu_writer: Optional[asyncio.StreamWriter] = None  # 用于向 MCU 发送命令

# MCU 最后已知状态（离线时返回）
last_known_status: dict = {
    "mcu_online": False,
    "net_state": "offline",
    "fps": 0,
    "sd_free_mb": 0,
    "uptime_sec": 0,
    "drop_count": 0,
    "cam_available": True,
    "sd_available": True,
    "sd_low_space": False,
    "last_seen": None,
}

# 等待拍照结果的 Future（由 snapshot API 设置，由接收器 resolve）
snapshot_future: Optional[asyncio.Future] = None

# 当前活跃连接的世代号，用于多连接竞态判断：
# 每个新连接进入时分配一个递增 ID，只有仍是"当前世代"的连接
# 才允许在断开时清空 mcu_connected/mcu_writer，避免旧连接的
# 延迟 cleanup 覆盖掉新连接已经建立好的状态。
_connection_generation: int = 0


# ---------------------------------------------------------------------------
# 帧广播
# ---------------------------------------------------------------------------

def _broadcast_frame(frame: bytes) -> None:
    """将帧推送到所有客户端队列（满则丢弃最旧帧）。"""
    global latest_frame
    latest_frame = frame

    dead: list[asyncio.Queue] = []
    for q in client_queues:
        if q.full():
            try:
                q.get_nowait()   # 丢弃最旧帧
            except asyncio.QueueEmpty:
                pass
        try:
            q.put_nowait(frame)
        except asyncio.QueueFull:
            dead.append(q)

    for q in dead:
        if q in client_queues:
            client_queues.remove(q)


# ---------------------------------------------------------------------------
# MJPEG multipart 解析器
# ---------------------------------------------------------------------------

BOUNDARY = b"--frame"
JPEG_SOI  = b"\xff\xd8"   # JPEG Start Of Image
JPEG_EOI  = b"\xff\xd9"   # JPEG End Of Image


async def _parse_mjpeg_stream(reader: asyncio.StreamReader) -> None:
    """
    从 TCP 流中解析 MJPEG multipart 帧。

    MCU 推送格式：
        --frame\r\n
        Content-Type: image/jpeg\r\n
        Content-Length: <N>\r\n
        \r\n
        <JPEG bytes>
    """
    buf = b""
    content_length: Optional[int] = None

    while True:
        try:
            chunk = await asyncio.wait_for(reader.read(4096), timeout=5.0)
        except asyncio.TimeoutError:
            logger.warning("MCU stream read timeout")
            break
        except Exception as e:
            logger.warning("MCU stream read error: %s", e)
            break

        if not chunk:
            logger.info("MCU closed connection")
            break

        buf += chunk

        # 缓冲总量保护：超过上限判定流损坏，断开连接防止内存耗尽
        if len(buf) > config.MAX_STREAM_BUF_SIZE:
            logger.warning(
                "MCU stream buffer exceeded %d bytes without a complete frame, "
                "closing connection", config.MAX_STREAM_BUF_SIZE
            )
            break

        # 持续从缓冲区中提取完整帧
        while True:
            # 查找帧边界
            boundary_pos = buf.find(BOUNDARY)
            if boundary_pos == -1:
                # 保留尾部（可能是不完整的 boundary）
                if len(buf) > len(BOUNDARY):
                    buf = buf[-(len(BOUNDARY)):]
                break

            # 跳过 boundary 行，找到头部结束位置（\r\n\r\n）
            header_end = buf.find(b"\r\n\r\n", boundary_pos)
            if header_end == -1:
                break  # 头部不完整，等待更多数据

            header_section = buf[boundary_pos:header_end].decode("utf-8", errors="ignore")
            header_end += 4  # 跳过 \r\n\r\n

            # 解析 part 头：Content-Length 及拍照标记
            content_length = None
            is_snapshot = False
            snap_sd_failed = False
            for line in header_section.splitlines():
                low = line.lower()
                if low.startswith("content-length:"):
                    try:
                        content_length = int(line.split(":", 1)[1].strip())
                    except ValueError:
                        pass
                elif low.startswith("x-snapshot:"):
                    # 带 X-Snapshot 标记的 part 是拍照帧，不广播到视频流
                    is_snapshot = True
                elif low.startswith("x-sd-failed:"):
                    snap_sd_failed = (line.split(":", 1)[1].strip().lower() == "true")

            if content_length is None:
                # 没有 Content-Length，尝试用 JPEG SOI/EOI 定位
                soi = buf.find(JPEG_SOI, header_end)
                eoi = buf.find(JPEG_EOI, soi + 2) if soi != -1 else -1
                if soi == -1 or eoi == -1:
                    break
                jpeg_data = buf[soi: eoi + 2]
                buf = buf[eoi + 2:]
            else:
                # 单帧上限保护：异常大的 Content-Length 直接断开，避免无限等待/内存耗尽
                if content_length > config.MAX_FRAME_SIZE:
                    logger.warning(
                        "Frame Content-Length %d exceeds limit %d, closing connection",
                        content_length, config.MAX_FRAME_SIZE
                    )
                    return
                if len(buf) < header_end + content_length:
                    break  # 数据不足，等待
                jpeg_data = buf[header_end: header_end + content_length]
                buf = buf[header_end + content_length:]

            global mcu_last_seen
            if is_status:
                # 状态 part：body 是 JSON，更新 MCU 状态而非广播/拍照
                status_obj = _parse_status_message(jpeg_data)
                if status_obj:
                    await _apply_mcu_status(status_obj)
                mcu_last_seen = time.monotonic()
            elif is_snapshot:
                # 拍照帧：交给等待中的 snapshot Future，不广播
                if jpeg_data.startswith(JPEG_SOI):
                    await _handle_snapshot_response(jpeg_data, snap_sd_failed)
                    mcu_last_seen = time.monotonic()
            else:
                if jpeg_data.startswith(JPEG_SOI):
                    _broadcast_frame(jpeg_data)
                    mcu_last_seen = time.monotonic()


# ---------------------------------------------------------------------------
# MCU 状态消息解析（HTTP POST /status 或 JSON 行）
# ---------------------------------------------------------------------------

def _parse_status_message(data: bytes) -> Optional[dict]:
    """尝试从数据中提取 JSON 状态对象。"""
    text = data.decode("utf-8", errors="ignore")
    # 找到第一个 '{' 和最后一个 '}'
    start = text.find("{")
    end   = text.rfind("}")
    if start == -1 or end == -1 or end <= start:
        return None
    try:
        return json.loads(text[start: end + 1])
    except json.JSONDecodeError:
        return None


# ---------------------------------------------------------------------------
# 拍照响应处理
# ---------------------------------------------------------------------------

async def _apply_mcu_status(status_obj: dict) -> None:
    """更新最近已知状态并写入状态日志。

    供两条路径共用：推流内的 X-Status part、以及独立 POST /status 连接。
    """
    last_known_status.update(status_obj)
    last_known_status["mcu_online"] = True
    last_known_status["last_seen"] = (
        __import__("datetime").datetime.utcnow().isoformat() + "Z"
    )
    await models.insert_status_log(status_obj)
    logger.debug("MCU status updated: fps=%s", status_obj.get("fps"))


async def _handle_snapshot_response(jpeg_data: bytes, sd_failed: bool) -> None:
    """将拍照结果传递给等待中的 Future。"""
    global snapshot_future
    if snapshot_future and not snapshot_future.done():
        snapshot_future.set_result((jpeg_data, sd_failed))


# ---------------------------------------------------------------------------
# MCU 连接处理协程
# ---------------------------------------------------------------------------

async def _handle_mcu_connection(
    reader: asyncio.StreamReader,
    writer: asyncio.StreamWriter,
) -> None:
    global mcu_connected, mcu_writer, mcu_last_seen, last_known_status
    global _connection_generation

    peer = writer.get_extra_info("peername")
    logger.info("MCU connected from %s", peer)
    _connection_generation += 1
    my_generation = _connection_generation
    mcu_connected = True
    mcu_writer    = writer
    mcu_last_seen = time.monotonic()

    try:
        # 读取 HTTP 请求行，判断请求类型
        first_line = await asyncio.wait_for(reader.readline(), timeout=5.0)
        first_line_str = first_line.decode("utf-8", errors="ignore").strip()
        logger.debug("MCU first line: %s", first_line_str)

        if first_line_str.startswith("POST /stream"):
            # 视频流推送：解析 HTTP 头并验证 HMAC，再进入 MJPEG 解析
            headers: dict[str, str] = {}
            while True:
                line = await asyncio.wait_for(reader.readline(), timeout=5.0)
                if line in (b"\r\n", b"\n", b""):
                    break
                if b":" in line:
                    k, v = line.decode("utf-8", errors="ignore").split(":", 1)
                    headers[k.strip().lower()] = v.strip()

            from api.auth_hmac import verify_mcu_request
            ok, reason = verify_mcu_request(
                "POST", "/stream",
                headers.get("x-timestamp"),
                headers.get("x-hmac-sha256"),
            )
            if not ok:
                logger.warning("Stream push HMAC failed from %s: %s", peer, reason)
                return  # 拒绝未认证的推流连接

            await _parse_mjpeg_stream(reader)

        elif first_line_str.startswith("POST /snapshot"):
            # 拍照上传：读取 HTTP 头，验证 HMAC，获取 Content-Length
            headers = {}
            while True:
                line = await asyncio.wait_for(reader.readline(), timeout=5.0)
                if line in (b"\r\n", b"\n", b""):
                    break
                if b":" in line:
                    k, v = line.decode("utf-8", errors="ignore").split(":", 1)
                    headers[k.strip().lower()] = v.strip()

            from api.auth_hmac import verify_mcu_request
            ok, reason = verify_mcu_request(
                "POST", "/snapshot",
                headers.get("x-timestamp"),
                headers.get("x-hmac-sha256"),
            )
            if not ok:
                logger.warning("Snapshot upload HMAC failed from %s: %s", peer, reason)
                return  # 拒绝未认证的照片上传

            try:
                content_length = int(headers.get("content-length", "0"))
            except ValueError:
                logger.warning("Snapshot: invalid Content-Length header")
                return
            if content_length > config.MAX_SNAPSHOT_SIZE:
                logger.warning(
                    "Snapshot Content-Length %d exceeds limit %d, rejecting",
                    content_length, config.MAX_SNAPSHOT_SIZE
                )
                return
            sd_failed_str  = headers.get("x-sd-failed", "false").lower()
            sd_failed      = sd_failed_str == "true"

            if content_length > 0:
                jpeg_data = await asyncio.wait_for(
                    reader.readexactly(content_length), timeout=10.0
                )
                await _handle_snapshot_response(jpeg_data, sd_failed)

        elif first_line_str.startswith("POST /status"):
            # 状态上报：读取 JSON body，验证 HMAC
            headers = {}
            while True:
                line = await asyncio.wait_for(reader.readline(), timeout=5.0)
                if line in (b"\r\n", b"\n", b""):
                    break
                if b":" in line:
                    k, v = line.decode("utf-8", errors="ignore").split(":", 1)
                    headers[k.strip().lower()] = v.strip()

            try:
                content_length = int(headers.get("content-length", "0"))
            except ValueError:
                logger.warning("Status: invalid Content-Length header")
                return
            if content_length > config.MAX_STATUS_BODY_SIZE:
                logger.warning(
                    "Status Content-Length %d exceeds limit %d, rejecting",
                    content_length, config.MAX_STATUS_BODY_SIZE
                )
                return
            if content_length > 0:
                body = await asyncio.wait_for(
                    reader.readexactly(content_length), timeout=5.0
                )
                # HMAC 验证（签名不含 body，与 MCU net_auth_sign 一致）
                from api.auth_hmac import verify_mcu_request
                ok, reason = verify_mcu_request(
                    "POST", "/status",
                    headers.get("x-timestamp"),
                    headers.get("x-hmac-sha256"),
                )
                if not ok:
                    logger.warning("Status report HMAC failed: %s", reason)
                else:
                    status = _parse_status_message(body)
                    if status:
                        await _apply_mcu_status(status)
                        # 状态上报也算 MCU 活动，刷新心跳（否则纯状态连接永不刷新）
                        mcu_last_seen = time.monotonic()

        else:
            # 未知请求，读取并丢弃
            logger.warning("Unknown MCU request: %s", first_line_str)
            await reader.read(4096)

    except asyncio.TimeoutError:
        logger.warning("MCU connection timeout from %s", peer)
    except asyncio.IncompleteReadError:
        logger.info("MCU disconnected (incomplete read) from %s", peer)
    except Exception as e:
        logger.error("MCU connection error from %s: %s", peer, e)
    finally:
        # 仅当本连接仍是当前世代（未被更新的连接取代）时，
        # 才清空全局连接状态，避免旧连接的延迟 cleanup
        # 覆盖新连接已经建立好的 mcu_connected/mcu_writer
        if my_generation == _connection_generation:
            mcu_connected = False
            mcu_writer    = None
            last_known_status["mcu_online"] = False
        try:
            writer.close()
            await writer.wait_closed()
        except Exception:
            pass
        logger.info("MCU disconnected from %s", peer)


# ---------------------------------------------------------------------------
# 向 MCU 发送命令
# ---------------------------------------------------------------------------

async def send_command_to_mcu(cmd: dict) -> bool:
    """
    通过当前 MCU 连接发送 JSON 命令。
    返回 True 表示发送成功，False 表示 MCU 未连接。
    """
    if mcu_writer is None or mcu_writer.is_closing():
        return False
    try:
        payload = json.dumps(cmd).encode() + b"\r\n"
        mcu_writer.write(payload)
        await mcu_writer.drain()
        logger.info("Command sent to MCU: %s", cmd)
        return True
    except Exception as e:
        logger.error("Failed to send command to MCU: %s", e)
        return False


# ---------------------------------------------------------------------------
# 心跳检测协程（后台任务）
# ---------------------------------------------------------------------------

async def heartbeat_monitor() -> None:
    """每秒检查 MCU 心跳，超时则标记离线并关闭连接。"""
    global mcu_connected, mcu_writer
    while True:
        await asyncio.sleep(1.0)
        if mcu_connected:
            elapsed = time.monotonic() - mcu_last_seen
            if elapsed > config.MCU_HEARTBEAT_TIMEOUT_S:
                logger.warning(
                    "MCU heartbeat timeout (%.1fs), marking offline and closing", elapsed
                )
                mcu_connected = False
                last_known_status["mcu_online"] = False
                # 关闭陷入停滞但未断开的连接，释放资源
                w = mcu_writer
                mcu_writer = None
                if w is not None:
                    try:
                        w.close()
                    except Exception:
                        pass


# ---------------------------------------------------------------------------
# TCP 服务器启动入口
# ---------------------------------------------------------------------------

async def start_tcp_server() -> asyncio.Server:
    """启动 TCP 服务器，返回 server 对象供 lifespan 管理。"""
    server = await asyncio.start_server(
        _handle_mcu_connection,
        host=config.MCU_STREAM_HOST,
        port=config.MCU_STREAM_PORT,
    )
    addr = server.sockets[0].getsockname() if server.sockets else "unknown"
    logger.info("MCU TCP server listening on %s", addr)
    return server
