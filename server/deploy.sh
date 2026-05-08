#!/bin/bash
# deploy.sh — 树莓派 5 一键部署脚本
#
# 用法：
#   chmod +x deploy.sh
#   ./deploy.sh
#
# 前置条件：
#   - Raspberry Pi OS (64-bit) 已安装
#   - Python 3.11+ 已安装
#   - 以 pi 用户运行

set -euo pipefail

INSTALL_DIR="/opt/ipcam-server"
STORAGE_DIR="/var/ipcam"
SERVICE_NAME="ipcam"
PYTHON="python3"

echo "=== Pisces IP Camera Server — Deploy ==="

# 1. 创建安装目录
echo "[1/7] Creating directories..."
sudo mkdir -p "$INSTALL_DIR"
sudo mkdir -p "$STORAGE_DIR/recordings"
sudo mkdir -p "$STORAGE_DIR/snapshots"
sudo chown -R pi:pi "$INSTALL_DIR" "$STORAGE_DIR"

# 2. 复制源码
echo "[2/7] Copying source files..."
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
sudo cp -r "$SCRIPT_DIR"/*.py "$INSTALL_DIR/"
sudo cp -r "$SCRIPT_DIR"/api "$INSTALL_DIR/"
sudo cp "$SCRIPT_DIR/requirements.txt" "$INSTALL_DIR/"
sudo chown -R pi:pi "$INSTALL_DIR"

# 3. 创建 .env 文件（若不存在）
if [ ! -f "$INSTALL_DIR/.env" ]; then
    echo "[3/7] Creating .env from example..."
    sudo cp "$SCRIPT_DIR/.env.example" "$INSTALL_DIR/.env"
    sudo chown pi:pi "$INSTALL_DIR/.env"
    echo "  ⚠️  请编辑 $INSTALL_DIR/.env 设置 WiFi 密码和认证信息"
else
    echo "[3/7] .env already exists, skipping"
fi

# 4. 创建 Python 虚拟环境并安装依赖
echo "[4/7] Setting up Python virtual environment..."
cd "$INSTALL_DIR"
$PYTHON -m venv venv
./venv/bin/pip install --upgrade pip --quiet
./venv/bin/pip install -r requirements.txt --quiet
echo "  Dependencies installed"

# 5. 安装 systemd 服务
echo "[5/7] Installing systemd service..."
sudo cp "$SCRIPT_DIR/ipcam.service" /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable "$SERVICE_NAME"

# 6. 安装 Nginx 配置（若 Nginx 已安装）
if command -v nginx &>/dev/null; then
    echo "[6/7] Configuring Nginx..."
    sudo cp "$SCRIPT_DIR/nginx/ipcam.conf" /etc/nginx/sites-available/ipcam
    sudo ln -sf /etc/nginx/sites-available/ipcam /etc/nginx/sites-enabled/ipcam
    # 移除默认站点
    sudo rm -f /etc/nginx/sites-enabled/default
    if sudo nginx -t 2>/dev/null; then
        sudo systemctl reload nginx
        echo "  Nginx configured"
    else
        echo "  ⚠️  Nginx config test failed, please check /etc/nginx/sites-available/ipcam"
    fi
else
    echo "[6/7] Nginx not found, skipping (install with: sudo apt install nginx)"
fi

# 7. 启动服务
echo "[7/7] Starting service..."
sudo systemctl start "$SERVICE_NAME"
sleep 2

if systemctl is-active --quiet "$SERVICE_NAME"; then
    echo ""
    echo "✅ Deployment complete!"
    echo "   Service status: $(systemctl is-active $SERVICE_NAME)"
    echo "   API:    http://$(hostname -I | awk '{print $1}'):8000"
    echo "   Docs:   http://$(hostname -I | awk '{print $1}'):8000/docs"
    echo "   Health: http://$(hostname -I | awk '{print $1}'):8000/health"
    echo ""
    echo "   View logs: sudo journalctl -u $SERVICE_NAME -f"
else
    echo "❌ Service failed to start"
    echo "   Check logs: sudo journalctl -u $SERVICE_NAME -n 50"
    exit 1
fi
