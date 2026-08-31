#!/bin/bash
# FnMon Server 一键部署脚本
# 用法: sudo bash deploy.sh

set -e

# ---------- 颜色输出 ----------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

info()  { echo -e "${CYAN}[INFO]${NC} $1"; }
ok()    { echo -e "${GREEN}[  OK]${NC} $1"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $1"; }
err()   { echo -e "${RED}[ERR]${NC} $1"; }

# ---------- 检查 root ----------
if [ "$EUID" -ne 0 ]; then
    err "请使用 root 运行: sudo bash deploy.sh"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SERVICE_NAME="fnmon-server"
INSTALL_DIR="/opt/fnmon-server"

echo ""
echo "=========================================="
echo "   FnMon Server 一键部署"
echo "=========================================="
echo ""

# ---------- 1. 安装 Python 和依赖 ----------
info "1/5 安装 Python3 和 pip..."
apt-get update -qq
apt-get install -y -qq python3 python3-pip python3-venv > /dev/null 2>&1
ok "Python3 已安装: $(python3 --version)"

# ---------- 2. 创建项目目录和虚拟环境 ----------
info "2/5 部署程序到 $INSTALL_DIR..."
mkdir -p "$INSTALL_DIR"

# 复制文件
cp "$SCRIPT_DIR/servermon.py" "$INSTALL_DIR/"
cp "$SCRIPT_DIR/config.json"  "$INSTALL_DIR/"

# 创建虚拟环境
python3 -m venv "$INSTALL_DIR/venv"
source "$INSTALL_DIR/venv/bin/activate"

# 安装依赖
pip install --upgrade pip -q
pip install psutil paho-mqtt -q
ok "依赖安装完成"

# ---------- 3. 交互式配置 ----------
echo ""
echo "------------------------------------------"
echo "  服务器监控配置"
echo "------------------------------------------"
echo ""

# ESP32 UDP 目标
read -p "ESP32 IP 地址 (用于 UDP 推送): " ESP32_IP
ESP32_IP="${ESP32_IP:-192.168.1.100}"

# UDP 端口
read -p "UDP 端口 [9000]: " UDP_PORT
UDP_PORT="${UDP_PORT:-9000}"

# 推送间隔
read -p "推送间隔(秒) [2]: " INTERVAL
INTERVAL="${INTERVAL:-2}"

# MQTT 配置
echo ""
read -p "是否启用 MQTT? (y/n) [n]: " USE_MQTT
USE_MQTT="${USE_MQTT:-n}"

MQTT_BROKER=""
MQTT_PORT="1883"
MQTT_TOPIC=""

if [ "$USE_MQTT" = "y" ] || [ "$USE_MQTT" = "Y" ]; then
    read -p "MQTT Broker 地址: " MQTT_BROKER
    read -p "MQTT 端口 [1883]: " MQTT_PORT
    MQTT_PORT="${MQTT_PORT:-1883}"
    read -p "MQTT Topic: " MQTT_TOPIC
fi

# ---------- 4. 生成配置文件 ----------
info "3/5 生成配置文件..."

cat > "$INSTALL_DIR/config.json" << EOF
{
    "interval": $INTERVAL,
    "udp": {
        "enabled": true,
        "host": "$ESP32_IP",
        "port": $UDP_PORT
    },
    "mqtt": {
        "enabled": $( [ "$USE_MQTT" = "y" ] || [ "$USE_MQTT" = "Y" ] && echo "true" || echo "false" ),
        "broker": "$MQTT_BROKER",
        "port": $MQTT_PORT,
        "topic": "$MQTT_TOPIC",
        "client_id": "fnmon-server"
    },
    "disk_path": "/",
    "net_interface": null
}
EOF

ok "配置文件已生成: $INSTALL_DIR/config.json"

# ---------- 5. 创建 systemd 服务 ----------
info "4/5 配置 systemd 服务..."

cat > "/etc/systemd/system/${SERVICE_NAME}.service" << EOF
[Unit]
Description=FnMon Server Monitor
After=network.target

[Service]
Type=simple
User=root
WorkingDirectory=$INSTALL_DIR
ExecStart=$INSTALL_DIR/venv/bin/python3 $INSTALL_DIR/servermon.py -c $INSTALL_DIR/config.json
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable "$SERVICE_NAME" > /dev/null 2>&1
systemctl restart "$SERVICE_NAME"

sleep 2

if systemctl is-active --quiet "$SERVICE_NAME"; then
    ok "服务已启动并设置开机自启"
else
    err "服务启动失败，查看日志: journalctl -u $SERVICE_NAME -f"
    exit 1
fi

# ---------- 完成 ----------
echo ""
echo "=========================================="
echo -e "  ${GREEN}部署完成！${NC}"
echo "=========================================="
echo ""
echo "  程序目录:  $INSTALL_DIR"
echo "  配置文件:  $INSTALL_DIR/config.json"
echo "  服务名称:  $SERVICE_NAME"
echo ""
echo "  常用命令:"
echo "    查看状态:  systemctl status $SERVICE_NAME"
echo "    查看日志:  journalctl -u $SERVICE_NAME -f"
echo "    重启服务:  systemctl restart $SERVICE_NAME"
echo "    停止服务:  systemctl stop $SERVICE_NAME"
echo "    修改配置:  nano $INSTALL_DIR/config.json"
echo ""
echo "  修改配置后重启: systemctl restart $SERVICE_NAME"
echo ""
