# FnMon Server — 服务器监控数据采集端

采集服务器的 CPU、内存、磁盘、网络数据，通过 UDP 或 MQTT 推送到 ESP32 LED 矩阵显示端。

## 安装依赖

```bash
pip install psutil paho-mqtt   # paho-mqtt 为可选（使用 MQTT 时需要）
```

## 快速开始

### 1. 只用 UDP（最简单）

```bash
# 直接指定 ESP32 的 IP，立即开始推送
python3 servermon.py --udp-host 192.168.1.100
```

### 2. 只用 MQTT

```bash
python3 servermon.py --mqtt-broker 192.168.1.200 --mqtt-topic servermon/stats
```

### 3. UDP + MQTT 同时推送

```bash
python3 servermon.py \
    --udp-host 192.168.1.100 \
    --mqtt-broker 192.168.1.200 \
    --mqtt-topic servermon/stats
```

### 4. 使用配置文件

```bash
# 编辑 config.json，然后：
python3 servermon.py -c config.json
```

## 配置文件说明 (config.json)

```json
{
    "interval": 2,                    // 推送间隔（秒）

    "udp": {
        "enabled": true,              // 启用 UDP 推送
        "host": "192.168.1.100",      // ESP32 的 IP 地址
        "port": 9000                  // ESP32 的 UDP 监听端口
    },

    "mqtt": {
        "enabled": false,             // 启用 MQTT 推送
        "broker": "192.168.1.200",    // MQTT Broker 地址
        "port": 1883,                 // MQTT 端口
        "topic": "servermon/stats",   // MQTT Topic
        "client_id": "fnmon-server"   // MQTT Client ID
    },

    "disk_path": "/",                 // 磁盘监控路径
    "net_interface": null             // 网络接口（null = 自动）
}
```

## ESP32 端配置

在 ESP32 的 Web 后台（连接 WiFi 后访问其 IP 地址）中：

1. **数据源配置** → UDP 接收端口设为 `9000`
2. 如果使用 MQTT：勾选"启用 MQTT"，填写 Broker 地址和 Topic（与服务端一致）

## 推送的数据格式 (JSON)

```json
{
    "cpu_percent": 45.2,
    "mem_percent": 67.8,
    "disk_percent": 30.1,
    "gpu_percent": 0,
    "rx_kb_s": 1024.5,
    "tx_kb_s": 512.3,
    "timestamp": 1693500000
}
```

## 后台运行

```bash
# 使用 nohup
nohup python3 servermon.py -c config.json &

# 或使用 systemd（见下方）
```

### systemd 服务配置

创建 `/etc/systemd/system/fnmon-server.service`:

```ini
[Unit]
Description=FnMon Server Monitor
After=network.target

[Service]
Type=simple
User=root
WorkingDirectory=/path/to/server
ExecStart=/usr/bin/python3 /path/to/server/servermon.py -c /path/to/server/config.json
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl enable fnmon-server
sudo systemctl start fnmon-server
sudo systemctl status fnmon-server
```
