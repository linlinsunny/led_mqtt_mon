# FnMon — LED 矩阵服务器监控 / LED Matrix Server Monitor

<div align="center">

📡 **通过 UDP/MQTT 实时显示服务器状态的 ESP32 LED 矩阵监控系统**

*Real-time server status display on ESP32 LED matrix via UDP/MQTT*

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-ESP32-orange.svg)]()
[![Python](https://img.shields.io/badge/python-3.6+-green.svg)]()

</div>

---

## 📸 效果演示 / Preview

```
┌──────────────────────┐
│        FNOS          │  ← 服务器名称 / Server Name
├──────────────────────┤
│ CPU   45%            │  ← CPU 使用率 / CPU Usage
│ MEM   67%            │  ← 内存使用率 / Memory Usage
│ DSK   30%            │  ← 磁盘使用率 / Disk Usage
│ RXK  1.2M            │  ← 网络下载 / Network RX
│ TXK  512K            │  ← 网络上传 / Network TX
└──────────────────────┘
      64x64 LED Matrix
```

---

## 🏗️ 系统架构 / Architecture

```
┌─────────────────┐      UDP/MQTT       ┌─────────────────┐      HUB75      ┌──────────────┐
│  Server (Python) │  ───────────────▶  │   ESP32 (WiFi)   │  ────────────▶ │  64x64 LED   │
│  采集系统数据     │                     │  接收 + 显示      │                │  Matrix Panel │
└─────────────────┘                     └─────────────────┘                └──────────────┘
        │                                       │
        ▼                                       ▼
   config.json                           Web 后台配置
   (推送目标)                            (ESP32 IP:80)
```

---

## 📁 项目结构 / Project Structure

```
led_mqtt_mon/
├── README.md                 # 说明文档 / Documentation
├── servermon/                # ESP32 显示端 / ESP32 Display Firmware
│   └── servermon.ino         # Arduino 固件 / Arduino Firmware
├── server/                   # Python 服务端 / Python Server
│   ├── servermon.py          # 数据采集与推送 / Data Collector & Pusher
│   ├── config.json           # 配置文件 / Configuration
│   ├── deploy.sh             # 一键部署脚本 / One-click Deploy Script
│   └── README.md             # 服务端文档 / Server Documentation
└── .gitignore
```

---

## 🚀 快速开始 / Quick Start

### 1. 服务端部署 / Server Setup

#### 一键部署（推荐）/ One-click Deploy (Recommended)

```bash
git clone https://github.com/linlinsunny/led_mqtt_mon.git
cd led_mqtt_mon
sudo bash server/deploy.sh
```

脚本会交互式引导你配置 ESP32 IP、MQTT 等信息。

*The script will guide you through ESP32 IP, MQTT settings interactively.*

#### 手动部署 / Manual Setup

```bash
# 安装依赖 / Install dependencies
pip install psutil paho-mqtt

# 运行 / Run
python3 server/servermon.py --udp-host 192.168.1.100
```

### 2. ESP32 固件烧录 / ESP32 Firmware Flash

1. 安装 [Arduino IDE](https://www.arduino.cc/en/software) 或 [PlatformIO](https://platformio.org/)
2. 安装 ESP32 开发板支持
3. 安装依赖库：
   - `ESP32-HUB75-MatrixPanel-I2S-DMA`
   - `PubSubClient` (by Nick O'Leary)
4. 打开 `servermon/servermon.ino`
5. 选择开发板 `ESP32 Dev Module`，烧录

### 3. 首次配置 / First-time Configuration

1. ESP32 上电后会创建 WiFi 热点 `FNMON-CFG`
2. 手机/电脑连接该热点
3. 浏览器访问 `192.168.4.1`
4. 输入你家 WiFi 的 SSID 和密码，保存
5. ESP32 重启后连接你的 WiFi
6. 访问 ESP32 的 IP 地址，配置数据源（UDP/MQTT）

---

## ⚙️ 配置说明 / Configuration

### 服务端配置 / Server Config (`server/config.json`)

```json
{
    "interval": 2,
    "udp": {
        "enabled": true,
        "host": "192.168.1.100",
        "port": 9000
    },
    "mqtt": {
        "enabled": false,
        "broker": "161.33.33.35",
        "port": 1883,
        "topic": "servermon/stats",
        "client_id": "fnmon-server"
    },
    "disk_path": "/",
    "net_interface": null
}
```

### ESP32 Web 后台 / ESP32 Web Backend

连接 ESP32 的 WiFi 后，浏览器访问其 IP 地址即可配置：

| 设置项 | 说明 |
|--------|------|
| 服务器名称 | LED 顶部显示的标题 (最多16字符) |
| 屏幕亮度 | 1-255 |
| 轮播模式 | 按键切换 / 自动轮播 |
| 数据源 | UDP 端口 / MQTT Broker 地址和 Topic |
| 颜色设置 | 各栏目的 LED 颜色 |

---

## 🔌 数据格式 / Data Format

服务端推送的 JSON 格式（UDP 和 MQTT 通用）：

```json
{
    "cpu_percent": 45.2,
    "mem_percent": 67.8,
    "disk_percent": 30.1,
    "gpu_percent": 0,
    "rx_kb_s": 1024.5,
    "tx_kb_s": 512.3
}
```

---

## 🛠️ 命令行参数 / CLI Options

### 服务端 / Server

```bash
python3 servermon.py [OPTIONS]

Options:
  -c, --config FILE       配置文件路径 / Config file path
  --udp-host HOST         UDP 目标 IP / UDP target IP
  --udp-port PORT         UDP 端口 / UDP port
  --mqtt-broker HOST      MQTT Broker 地址 / MQTT broker address
  --mqtt-port PORT        MQTT 端口 / MQTT port
  --mqtt-topic TOPIC      MQTT Topic
  --interval SEC          推送间隔秒数 / Push interval in seconds
  --disk PATH             磁盘监控路径 / Disk monitor path
```

### 一键部署 / Deploy Script

```bash
sudo bash server/deploy.sh
```

部署后的管理命令：

```bash
systemctl status fnmon-server       # 查看状态 / Check status
journalctl -u fnmon-server -f       # 实时日志 / Live logs
systemctl restart fnmon-server      # 重启 / Restart
systemctl stop fnmon-server         # 停止 / Stop
nano /opt/fnmon-server/config.json  # 修改配置 / Edit config
```

---

## 📋 硬件需求 / Hardware Requirements

### ESP32 显示端 / ESP32 Display

| 组件 | 型号 | 数量 |
|------|------|------|
| 主控 | ESP32 DevKit V1 | 1 |
| LED 面板 | 64x64 HUB75 (1/32 scan) | 1 |
| 按键 | 轻触按键 (切换显示模式) | 1 |

### 引脚接线 / Wiring

| ESP32 Pin | HUB75 Pin |
|-----------|-----------|
| GPIO 25 | R1 |
| GPIO 26 | G1 |
| GPIO 27 | B1 |
| GPIO 14 | R2 |
| GPIO 12 | G2 |
| GPIO 13 | B2 |
| GPIO 23 | A |
| GPIO 19 | B |
| GPIO 5  | C |
| GPIO 17 | D |
| GPIO 18 | E |
| GPIO 4  | LAT |
| GPIO 15 | OE |
| GPIO 16 | CLK |
| GPIO 32 | 按键 (BUTTON) |

---

## ❓ 常见问题 / FAQ

### MQTT 连接失败？

1. 确认 Broker 已安装并运行：`systemctl status mosquitto`
2. 确认 Broker 监听所有接口：编辑 `/etc/mosquitto/mosquitto.conf` 添加 `listener 1883`
3. 确认防火墙放行 1883 端口：`sudo ufw allow 1883`
4. ESP32 在内网，Broker 在公网？确认 ESP32 的 WiFi 可以访问公网

### LED 面板不亮？

固件会自动尝试 6 种面板配置，查看串口输出确认哪种成功。如果都不行，检查引脚接线。

### 如何添加 GPU 监控？

服务端使用 `psutil` 采集数据，暂不支持 GPU。可通过 `nvidia-smi` 扩展，修改 `servermon.py` 的 `SystemCollector.collect()` 方法。

---

## 📄 许可证 / License

[MIT License](LICENSE)

---

## 🙏 致谢 / Credits

- [ESP32-HUB75-MatrixPanel-I2S-DMA](https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-I2S-DMA) — LED 矩阵驱动 / LED Matrix Driver
- [PubSubClient](https://github.com/knolleary/pubsubclient) — MQTT 客户端 / MQTT Client
- [psutil](https://github.com/giampaolo/psutil) — 系统监控 / System Monitoring
