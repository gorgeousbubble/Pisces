# Pisces 服务器端

基于 **FastAPI + asyncio** 的树莓派 5 服务器，负责接收 MCU 推送的视频流，并向用户提供 HTTP API。

## 目录结构

```
server/
├── main.py              # FastAPI 应用入口，lifespan 管理
├── config.py            # 配置管理（环境变量 / .env 文件）
├── models.py            # Pydantic 模型 + SQLite 数据库操作
├── stream_receiver.py   # MCU TCP 流接收器，帧广播，命令下发
├── api/
│   ├── auth.py          # Basic Auth 依赖项
│   ├── stream.py        # GET /stream/live（MJPEG 实时流）
│   ├── recordings.py    # GET /api/recordings（录像列表/下载）
│   ├── snapshots.py     # POST /api/snapshot，GET /api/snapshots/{file}
│   └── status.py        # GET /api/status（MCU 状态）
├── nginx/
│   └── ipcam.conf       # Nginx 反向代理 + SSL 配置
├── ipcam.service        # systemd 服务单元文件
├── deploy.sh            # 一键部署脚本
├── requirements.txt     # Python 依赖
└── .env.example         # 环境变量示例（复制为 .env 并填写）
```

## 快速部署

### 前置条件

- 树莓派 5，Raspberry Pi OS 64-bit
- Python 3.11+（`python3 --version`）
- Nginx（`sudo apt install nginx`）

### 一键部署

```bash
# 在树莓派上执行
git clone <repo-url> pisces
cd pisces/server
chmod +x deploy.sh
./deploy.sh
```

### 手动部署

```bash
# 1. 安装依赖
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt

# 2. 配置环境变量
cp .env.example .env
nano .env   # 修改 AUTH_PASSWORD 等敏感参数

# 3. 创建存储目录
sudo mkdir -p /var/ipcam/recordings /var/ipcam/snapshots
sudo chown -R pi:pi /var/ipcam

# 4. 启动服务（开发模式）
python main.py
# 或
uvicorn main:app --host 0.0.0.0 --port 8000 --reload
```

## API 接口

服务启动后访问 `http://<树莓派IP>:8000/docs` 查看完整 Swagger 文档。

| 方法 | 路径 | 说明 |
|------|------|------|
| `GET` | `/stream/live` | 实时 MJPEG 视频流 |
| `GET` | `/api/recordings` | 录像文件列表（支持时间范围筛选） |
| `GET` | `/api/recordings/{filename}` | 下载/播放录像文件 |
| `POST` | `/api/snapshot` | 远程触发拍照 |
| `GET` | `/api/snapshots/{filename}` | 获取照片文件 |
| `GET` | `/api/status` | MCU 运行状态 |
| `GET` | `/health` | 服务健康检查（无需认证） |

### 示例请求

```bash
# 查看状态
curl -u admin:changeme http://192.168.1.100:8000/api/status

# 触发拍照
curl -u admin:changeme -X POST http://192.168.1.100:8000/api/snapshot \
     -H "Content-Type: application/json" \
     -d '{"quality": 85, "width": 1280, "height": 720}'

# 查询录像列表（按时间范围）
curl -u admin:changeme \
     "http://192.168.1.100:8000/api/recordings?start=2026-05-08T00:00:00Z&end=2026-05-08T23:59:59Z"

# 实时视频流（浏览器直接打开）
# http://admin:changeme@192.168.1.100:8000/stream/live
```

## 配置说明

所有配置通过环境变量或 `.env` 文件设置：

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `MCU_STREAM_PORT` | `8554` | 接收 MCU 推流的 TCP 端口 |
| `API_PORT` | `8000` | FastAPI HTTP 端口 |
| `STORAGE_PATH` | `/var/ipcam` | 录像和照片存储根目录 |
| `MAX_STREAM_CLIENTS` | `10` | 最大并发视频流客户端数 |
| `AUTH_ENABLED` | `true` | 是否启用 Basic Auth |
| `AUTH_USERNAME` | `admin` | 认证用户名 |
| `AUTH_PASSWORD` | `changeme` | 认证密码（**生产环境必须修改**） |
| `SNAPSHOT_TIMEOUT_S` | `3.0` | 拍照命令超时（秒） |
| `MCU_HEARTBEAT_TIMEOUT_S` | `10` | MCU 心跳超时（秒） |

## 外网访问

1. 配置 Nginx（`nginx/ipcam.conf`）
2. 申请 SSL 证书：`sudo certbot --nginx -d your-domain.com`
3. 路由器端口映射：外网 TCP 443 → 树莓派 IP:443
4. 确保 `AUTH_ENABLED=true` 且密码足够强

## 查看日志

```bash
# 实时日志
sudo journalctl -u ipcam -f

# 最近 100 行
sudo journalctl -u ipcam -n 100

# 重启服务
sudo systemctl restart ipcam
```
