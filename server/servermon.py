#!/usr/bin/env python3
"""
FnMon Server — 服务器监控数据采集与推送
采集 CPU / 内存 / 磁盘 / 网络数据，通过 UDP 或 MQTT 推送到 ESP32 LED 矩阵显示端。

用法:
    python3 servermon.py                  # 使用默认配置
    python3 servermon.py -c config.json   # 指定配置文件
    python3 servermon.py --udp 192.168.1.100 --port 9000  # 命令行指定 UDP 目标

依赖:
    pip install psutil paho-mqtt
"""

import argparse
import json
import socket
import time
import sys
import os
import signal

try:
    import psutil
except ImportError:
    print("缺少 psutil，请运行: pip install psutil")
    sys.exit(1)

# MQTT 可选依赖
try:
    import paho.mqtt.client as mqtt
    HAS_MQTT = True
except ImportError:
    HAS_MQTT = False

# ---------- 默认配置 ----------
DEFAULT_CONFIG = {
    # 推送间隔（秒）
    "interval": 2,

    # UDP 目标
    "udp": {
        "enabled": True,
        "host": "192.168.1.100",  # ESP32 的 IP 地址
        "port": 9000
    },

    # MQTT 目标
    "mqtt": {
        "enabled": False,
        "broker": "192.168.1.200",  # MQTT Broker 地址
        "port": 1883,
        "topic": "servermon/stats",
        "client_id": "fnmon-server"
    },

    # 磁盘监控路径（Linux/macOS）
    "disk_path": "/",

    # 网络接口（None = 自动检测）
    "net_interface": None
}


def load_config(config_path=None):
    """加载配置，命令行参数 > 配置文件 > 默认值"""
    config = DEFAULT_CONFIG.copy()

    if config_path and os.path.exists(config_path):
        with open(config_path, 'r') as f:
            if config_path.endswith('.json'):
                user_config = json.load(f)
            else:
                # 尝试 YAML
                try:
                    import yaml
                    user_config = yaml.safe_load(f)
                except ImportError:
                    print("YAML 配置需要安装 pyyaml: pip install pyyaml")
                    sys.exit(1)
        # 递归合并配置
        _deep_merge(config, user_config)

    return config


def _deep_merge(base, override):
    """递归合并字典"""
    for key, value in override.items():
        if key in base and isinstance(base[key], dict) and isinstance(value, dict):
            _deep_merge(base[key], value)
        else:
            base[key] = value


# ---------- 数据采集 ----------
class SystemCollector:
    """采集系统监控数据"""

    def __init__(self, disk_path="/", net_interface=None):
        self.disk_path = disk_path
        self.net_interface = net_interface
        self._prev_net = None
        self._prev_time = None
        # 预热一次网络计数器
        self._get_net_speed()

    def collect(self):
        """采集一次数据，返回 JSON 字符串"""
        cpu_percent = psutil.cpu_percent(interval=None)
        mem = psutil.virtual_memory()
        disk = psutil.disk_usage(self.disk_path)
        rx_kb_s, tx_kb_s = self._get_net_speed()

        data = {
            "cpu_percent": round(cpu_percent, 1),
            "mem_percent": round(mem.percent, 1),
            "disk_percent": round(disk.percent, 1),
            "gpu_percent": 0,  # GPU 数据需要特殊驱动，预留
            "rx_kb_s": round(rx_kb_s, 1),
            "tx_kb_s": round(tx_kb_s, 1),
            "timestamp": int(time.time())
        }

        # 添加可选的详细信息（ESP32 会忽略不认识的字段）
        data["mem_total_gb"] = round(mem.total / (1024**3), 1)
        data["mem_used_gb"] = round(mem.used / (1024**3), 1)
        data["disk_total_gb"] = round(disk.total / (1024**3), 1)
        data["disk_used_gb"] = round(disk.used / (1024**3), 1)

        return json.dumps(data)

    def _get_net_speed(self):
        """计算网络速率 (KB/s)"""
        counters = psutil.net_io_counters(pernic=True)

        # 选择接口
        if self.net_interface and self.net_interface in counters:
            c = counters[self.net_interface]
        else:
            # 自动选择：跳过 lo，选第一个有流量的接口
            c = None
            for iface, counter in counters.items():
                if iface == "lo":
                    continue
                if counter.bytes_sent > 0 or counter.bytes_recv > 0:
                    c = counter
                    break
            if c is None:
                c = psutil.net_io_counters()

        now = time.time()

        if self._prev_net is None or self._prev_time is None:
            self._prev_net = c
            self._prev_time = now
            return 0.0, 0.0

        dt = now - self._prev_time
        if dt <= 0:
            dt = 1.0

        rx_bytes = c.bytes_recv - self._prev_net.bytes_recv
        tx_bytes = c.bytes_sent - self._prev_net.bytes_sent

        # 处理计数器翻转（很罕见但可能发生）
        if rx_bytes < 0:
            rx_bytes = c.bytes_recv
        if tx_bytes < 0:
            tx_bytes = c.bytes_sent

        rx_kb_s = rx_bytes / dt / 1024.0
        tx_kb_s = tx_bytes / dt / 1024.0

        self._prev_net = c
        self._prev_time = now

        return rx_kb_s, tx_kb_s


# ---------- UDP 推送 ----------
class UDPPusher:
    """通过 UDP 推送数据到 ESP32"""

    def __init__(self, host, port):
        self.host = host
        self.port = port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        print(f"[UDP] 目标: {host}:{port}")

    def send(self, data):
        try:
            self.sock.sendto(data.encode('utf-8'), (self.host, self.port))
        except Exception as e:
            print(f"[UDP] 发送失败: {e}")

    def close(self):
        self.sock.close()


# ---------- MQTT 推送 ----------
class MQTTPusher:
    """通过 MQTT 推送数据到 ESP32"""

    def __init__(self, broker, port, topic, client_id):
        if not HAS_MQTT:
            print("MQTT 需要安装 paho-mqtt: pip install paho-mqtt")
            sys.exit(1)

        self.topic = topic
        self.client = mqtt.Client(client_id=client_id, protocol=mqtt.MQTTv311)
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.connected = False

        print(f"[MQTT] Broker: {broker}:{port}, Topic: {topic}")
        try:
            self.client.connect(broker, port, keepalive=60)
            self.client.loop_start()
        except Exception as e:
            print(f"[MQTT] 连接失败: {e}")

    def _on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            self.connected = True
            print("[MQTT] 连接成功")
        else:
            self.connected = False
            print(f"[MQTT] 连接失败, rc={rc}")

    def _on_disconnect(self, client, userdata, rc):
        self.connected = False
        if rc != 0:
            print(f"[MQTT] 意外断开, rc={rc}")

    def send(self, data):
        if not self.connected:
            return
        try:
            self.client.publish(self.topic, data, qos=0)
        except Exception as e:
            print(f"[MQTT] 发送失败: {e}")

    def close(self):
        self.client.loop_stop()
        self.client.disconnect()


# ---------- 主程序 ----------
class ServerMon:
    """FnMon Server 主程序"""

    def __init__(self, config):
        self.config = config
        self.running = True
        self.collector = SystemCollector(
            disk_path=config.get("disk_path", "/"),
            net_interface=config.get("net_interface")
        )
        self.pushers = []

        # 初始化 UDP
        udp_cfg = config.get("udp", {})
        if udp_cfg.get("enabled", False):
            self.pushers.append(
                UDPPusher(udp_cfg["host"], udp_cfg.get("port", 9000))
            )

        # 初始化 MQTT
        mqtt_cfg = config.get("mqtt", {})
        if mqtt_cfg.get("enabled", False):
            self.pushers.append(
                MQTTPusher(
                    mqtt_cfg["broker"],
                    mqtt_cfg.get("port", 1883),
                    mqtt_cfg["topic"],
                    mqtt_cfg.get("client_id", "fnmon-server")
                )
            )

        if not self.pushers:
            print("错误: UDP 和 MQTT 都未启用，请检查配置")
            sys.exit(1)

    def run(self):
        """主循环"""
        interval = self.config.get("interval", 2)
        print(f"[ServerMon] 启动，推送间隔: {interval}s")
        print(f"[ServerMon] 活跃推送通道: {len(self.pushers)}")
        print("[ServerMon] 按 Ctrl+C 停止\n")

        # 预热 CPU 采集（psutil.cpu_percent 需要先调用一次）
        psutil.cpu_percent(interval=None)
        time.sleep(0.5)

        try:
            while self.running:
                data = self.collector.collect()

                for pusher in self.pushers:
                    pusher.send(data)

                # 简单的控制台输出
                d = json.loads(data)
                sys.stdout.write(
                    f"\r[CPU] {d['cpu_percent']:5.1f}%  "
                    f"[MEM] {d['mem_percent']:5.1f}%  "
                    f"[DSK] {d['disk_percent']:5.1f}%  "
                    f"[NET] ↓{d['rx_kb_s']:7.1f}K ↑{d['tx_kb_s']:7.1f}K"
                )
                sys.stdout.flush()

                time.sleep(interval)

        except KeyboardInterrupt:
            print("\n\n正在停止...")
        finally:
            self.cleanup()

    def cleanup(self):
        self.running = False
        for pusher in self.pushers:
            pusher.close()
        print("[ServerMon] 已停止")


def main():
    parser = argparse.ArgumentParser(
        description="FnMon Server — 采集服务器数据推送到 ESP32 LED 矩阵"
    )
    parser.add_argument("-c", "--config", help="配置文件路径 (JSON 或 YAML)")
    parser.add_argument("--udp-host", help="UDP 目标 IP (覆盖配置)")
    parser.add_argument("--udp-port", type=int, help="UDP 目标端口 (覆盖配置)")
    parser.add_argument("--mqtt-broker", help="MQTT Broker 地址 (覆盖配置)")
    parser.add_argument("--mqtt-port", type=int, help="MQTT 端口 (覆盖配置)")
    parser.add_argument("--mqtt-topic", help="MQTT Topic (覆盖配置)")
    parser.add_argument("--interval", type=float, help="推送间隔秒数")
    parser.add_argument("--disk", help="磁盘监控路径 (默认 /)")

    args = parser.parse_args()

    # 加载配置
    config = load_config(args.config)

    # 命令行参数覆盖
    if args.udp_host:
        config["udp"]["enabled"] = True
        config["udp"]["host"] = args.udp_host
    if args.udp_port:
        config["udp"]["port"] = args.udp_port
    if args.mqtt_broker:
        config["mqtt"]["enabled"] = True
        config["mqtt"]["broker"] = args.mqtt_broker
    if args.mqtt_port:
        config["mqtt"]["port"] = args.mqtt_port
    if args.mqtt_topic:
        config["mqtt"]["topic"] = args.mqtt_topic
    if args.interval:
        config["interval"] = args.interval
    if args.disk:
        config["disk_path"] = args.disk

    # 信号处理
    servermon = ServerMon(config)

    def signal_handler(sig, frame):
        servermon.running = False

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    servermon.run()


if __name__ == "__main__":
    main()
