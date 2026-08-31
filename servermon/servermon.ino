// macmon.ino — 服务器监控 LED 矩阵显示（支持 UDP + MQTT）

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiUdp.h>
#include <PubSubClient.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

// ---------- 矩阵配置 ----------
#define PANEL_RES_X 64
#define PANEL_RES_Y 64
#define PANEL_CHAIN 1

// 常见的64x64面板引脚定义（根据不同面板可能需要调整）
#define R1_PIN 25
#define G1_PIN 26
#define B1_PIN 27
#define R2_PIN 14
#define G2_PIN 12
#define B2_PIN 13
#define A_PIN  23
#define B_PIN  19
#define C_PIN  5
#define D_PIN  17
#define E_PIN  18
#define LAT_PIN 4
#define OE_PIN  15
#define CLK_PIN 16

#define BUTTON_PIN 32

MatrixPanel_I2S_DMA *dma_display = nullptr;

// ---------- WiFi / 配网 / UDP / 配置持久化 ----------
const char *AP_SSID   = "FNMON-CFG";
const char *AP_HOST   = "192.168.4.1";

Preferences prefs;
WebServer server(80);
WiFiUDP udp;

// ---------- MQTT ----------
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

String savedSSID = "";
String savedPass = "";

uint32_t udpPort = 9000;
uint8_t  brightness = 64;
bool     autoCycle = false;
uint32_t cycleIntervalSec = 10;
unsigned long lastCycleTime = 0;

// MQTT 配置
String mqttBroker = "161.33.33.35";
uint16_t mqttPort = 1883;
String mqttTopic = "OSAKA";
bool mqttEnabled = false;
bool mqttConnected = false;
unsigned long lastMqttReconnect = 0;
const unsigned long MQTT_RECONNECT_INTERVAL = 5000;

String colColorECP = "#00FF00";
String colColorPCP = "#00FF00";
String colColorGPU = "#00FF00";
String colColorMEM = "#00FF00";
String colColorPWR = "#00FFFF";
String colColorTitle = "#FFFFFF";
String serverTitle = "FNOS";

String lbl[5] = {"CPU", "MEM", "DSK", "RXK", "TXK"};
const char* sourceLabels[] = {"CPU%", "MEM%", "DSK%", "RXK", "TXK", "ANE%"};
uint8_t dataSource[5] = {0, 1, 2, 3, 4};

enum State { S_PROVISION, S_CONNECT, S_SHOWIP, S_DATA };
State state = S_PROVISION;
unsigned long lastChange = 0;
unsigned long lastRedraw = 0;
unsigned long credsSavedAt = 0;
bool credsJustSaved = false;

enum DisplayMode { MODE_TEXT, MODE_GRAPHIC };
DisplayMode displayMode = MODE_TEXT;
unsigned long lastBtnPress = 0;

float pcpu = 0, ecpu = 0, gpu = 0, ane = 0, mem = 0, pwr = 0;
float s_ecpu = 0, s_pcpu = 0, s_gpu = 0, s_mem = 0, s_pwr = 0;
float netRxMax = 1.0f, netTxMax = 1.0f;
const float EMA_ALPHA = 0.2f;
bool haveData = false;

// ---------- Picopixel 3x5 字体 ----------
static const uint8_t GLYPHS[59][5] = {
  {0,0,0,0,0}, {2,2,2,0,2}, {5,5,0,0,0}, {5,7,5,7,5}, {0,0,0,0,0},
  {4,1,2,4,1}, {0,0,0,0,0}, {2,2,0,0,0}, {1,2,2,2,1}, {4,2,2,2,4},
  {0,0,0,0,0}, {0,2,7,2,0}, {0,0,0,2,2}, {0,0,7,0,0}, {0,0,0,0,2},
  {1,1,2,4,4}, {2,5,5,5,2}, {2,6,2,2,7}, {7,1,2,4,7}, {7,1,3,1,7},
  {5,5,7,1,1}, {7,4,7,1,7}, {7,4,7,5,7}, {7,1,2,2,2}, {7,5,7,5,7},
  {7,5,7,1,7}, {0,2,0,2,0}, {0,2,0,2,2}, {1,2,4,2,1}, {0,7,0,7,0},
  {4,2,1,2,4}, {7,1,2,0,2}, {0,0,0,0,0}, {2,5,7,5,5}, {6,5,6,5,6},
  {3,4,4,4,3}, {6,5,5,5,6}, {7,4,6,4,7}, {7,4,6,4,4}, {3,4,5,5,3},
  {5,5,7,5,5}, {7,2,2,2,7}, {1,1,1,5,2}, {5,5,6,5,5}, {4,4,4,4,7},
  {5,7,7,5,5}, {5,7,7,7,5}, {2,5,5,5,2}, {6,5,6,4,4}, {2,5,5,6,3},
  {6,5,6,5,5}, {7,4,7,1,7}, {7,2,2,2,2}, {5,5,5,5,7}, {5,5,5,5,2},
  {5,5,7,7,5}, {5,5,2,5,5}, {5,5,2,2,2}, {7,1,2,4,7}
};

// ---------- 绘制辅助 ----------
#define COLOR_GREEN  dma_display->color565(0, 255, 0)
#define COLOR_YELLOW dma_display->color565(255, 255, 0)
#define COLOR_RED    dma_display->color565(255, 0, 0)
#define COLOR_WHITE  dma_display->color565(255, 255, 255)
#define COLOR_GRAY   dma_display->color565(50, 50, 50)
#define COLOR_BLUE   dma_display->color565(0, 180, 255)
#define PANEL_W (PANEL_RES_X * PANEL_CHAIN)
#define PANEL_H (PANEL_RES_Y)

uint16_t hexToColor565(const String &hexStr, uint16_t defaultColor) {
  String s = hexStr;
  s.trim();
  if (s.startsWith("#")) s = s.substring(1);
  if (s.length() != 6) return defaultColor;
  long rgb = strtol(s.c_str(), NULL, 16);
  uint8_t r = (rgb >> 16) & 0xFF;
  uint8_t g = (rgb >> 8) & 0xFF;
  uint8_t b = rgb & 0xFF;
  return dma_display->color565(r, g, b);
}

const uint8_t *getGlyph(char c) {
  int idx = (unsigned char)c - 0x20;
  if (idx < 0 || idx >= 59) return nullptr;
  return GLYPHS[idx];
}

void drawCharScaled(int x, int y, char c, int scale, uint16_t color) {
  const uint8_t *g = getGlyph(c);
  if (!g) return;
  for (int r = 0; r < 5; r++) {
    for (int col = 0; col < 3; col++) {
      if (g[r] & (0x4 >> col)) {
        int px = x + col * scale;
        int py = y + r * scale;
        if (px >= 0 && px < PANEL_W && py >= 0 && py < PANEL_H) {
          if (scale == 1) {
            dma_display->drawPixel(px, py, color);
          } else {
            dma_display->fillRect(px, py, scale, scale, color);
          }
        }
      }
    }
  }
}

int textWidthScaled(const char *s, int scale) {
  int n = strlen(s);
  return n > 0 ? n * (3 * scale + 1) - 1 : 0;
}

void drawTextScaled(int x, int y, const char *s, int scale, uint16_t color) {
  while (*s) {
    drawCharScaled(x, y, *s, scale, color);
    x += 3 * scale + 1;
    s++;
    if (x >= PANEL_W) break;
  }
}

int pickScale(const char *s) {
  return textWidthScaled(s, 2) <= PANEL_W ? 2 : 1;
}

void drawTextFit(int centerX, int y, const char *s, uint16_t color) {
  int scale = pickScale(s);
  int w = textWidthScaled(s, scale);
  drawTextScaled(centerX - w / 2, y, s, scale, color);
}

void clearAll() {
  dma_display->fillScreen(0);
}

// ---------- 数据源辅助函数 ----------
float getValueBySource(uint8_t source) {
  switch(source) {
    case 0: return s_ecpu;
    case 1: return s_pcpu;
    case 2: return s_gpu;
    case 3: return s_mem;
    case 4: return s_pwr;
    case 5: return ane;
    default: return 0;
  }
}

float getMaxBySource(uint8_t source) {
  switch(source) {
    case 0: return 100.0f;
    case 1: return 100.0f;
    case 2: return 100.0f;
    case 3: return netRxMax;
    case 4: return netTxMax;
    case 5: return 100.0f;
    default: return 100.0f;
  }
}

uint16_t getColorBySource(uint8_t source) {
  switch(source) {
    case 0: return hexToColor565(colColorECP, COLOR_GREEN);
    case 1: return hexToColor565(colColorPCP, COLOR_GREEN);
    case 2: return hexToColor565(colColorGPU, COLOR_GREEN);
    case 3: return hexToColor565(colColorMEM, COLOR_GREEN);
    case 4: return hexToColor565(colColorPWR, COLOR_WHITE);
    case 5: return COLOR_WHITE;
    default: return COLOR_GREEN;
  }
}

// ---------- 显示函数 ----------
void showProvision() {
  clearAll();
  drawTextFit(32, 8,  "WIFI", COLOR_WHITE);
  drawTextFit(32, 20, AP_SSID, COLOR_GREEN);
  drawTextFit(32, 32, "URL", COLOR_WHITE);
  drawTextFit(32, 44, AP_HOST, COLOR_GREEN);
  dma_display->flipDMABuffer();
}

void showConnecting() {
  clearAll();
  drawTextFit(32, 27, "CONNECT", COLOR_GREEN);
  drawTextFit(32, 38, "WIFI...", COLOR_WHITE);
  dma_display->flipDMABuffer();
}

void showIP() {
  char line[40];
  snprintf(line, sizeof(line), "%s", WiFi.localIP().toString().c_str());
  clearAll();
  drawTextFit(32, 22, "CONNECTED", COLOR_GREEN);
  drawTextFit(32, 34, line, COLOR_WHITE);
  dma_display->flipDMABuffer();
}

void showData() {
  char line[40];
  // 顶部标题行 (第0行, 字高 5px, Y: 1~5)
  const int TITLE_Y = 1;
  // 5 行数据起始 Y 坐标 (每行字高 10px, 行间距统一严格 1px, 底部留白 2px):
  const int lineY[5] = { 8, 19, 30, 41, 52 };

  dma_display->fillScreen(0);

  // 1. 绘制顶部第0行：服务器名称 / 标题 (强制使用 3*5 像素最小字体 scale=1, 字高5px)
  uint16_t titleColor = hexToColor565(colColorTitle, COLOR_WHITE);
  int titleW = textWidthScaled(serverTitle.c_str(), 1);
  int titleX = (PANEL_W - titleW) / 2;
  if (titleX < 0) titleX = 0;
  drawTextScaled(titleX, TITLE_Y, serverTitle.c_str(), 1, titleColor);

  auto autoColor = [](float ratio) -> uint16_t {
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    uint8_t r, g;
    if (ratio < 0.5f) {
      r = (uint8_t)(ratio * 2.0f * 255);
      g = 255;
    } else {
      r = 255;
      g = (uint8_t)((1.0f - ratio) * 2.0f * 255);
    }
    return dma_display->color565(r, g, 0);
  };

  if (displayMode == MODE_TEXT) {
    for (int i = 0; i < 5; i++) {
      int y = lineY[i];
      float val = getValueBySource(dataSource[i]);
      uint16_t color = getColorBySource(dataSource[i]);
      float maxVal = getMaxBySource(dataSource[i]);
      uint16_t valColor = autoColor(maxVal > 0 ? val / maxVal : 0);

      drawTextScaled(2, y, lbl[i].c_str(), 2, color);

      if (dataSource[i] == 3 || dataSource[i] == 4) {
        if (val >= 1024.0f) snprintf(line, sizeof(line), "%.1fM", val/1024.0f);
        else if (val >= 100.0f) snprintf(line, sizeof(line), "%.0fK", val);
        else snprintf(line, sizeof(line), "%.1fK", val);
      } else {
        snprintf(line, sizeof(line), "%d%%", (int)val);
      }
      int valX = PANEL_W - 2 - textWidthScaled(line, 2);
      if (valX < 20) valX = 20;
      drawTextScaled(valX, y, line, 2, valColor);
    }
  } else {
    const int barX = 25;
    const int barW = 37;
    const int barH = 10;

    for (int i = 0; i < 5; i++) {
      int y = lineY[i];
      float val = getValueBySource(dataSource[i]);
      uint16_t color = getColorBySource(dataSource[i]);
      float maxVal = getMaxBySource(dataSource[i]);

      drawTextScaled(2, y, lbl[i].c_str(), 2, color);

      float ratio = maxVal > 0 ? val / maxVal : 0;
      if (ratio < 0.0f) ratio = 0.0f;
      if (ratio > 1.0f) ratio = 1.0f;
      int fillW = (int)(ratio * barW);

      dma_display->fillRect(barX, y, barW, barH, COLOR_GRAY);
      if (fillW > 0) {
        dma_display->fillRect(barX, y, fillW, barH, autoColor(ratio));
      }
    }
  }

  dma_display->flipDMABuffer();
}

// ---------- 数据解析（UDP / MQTT 共用） ----------
float getJsonFloat(const char *buf, const char *key) {
  char pat[32];
  snprintf(pat, sizeof(pat), "\"%s\":", key);
  const char *p = strstr(buf, pat);
  if (!p) return NAN;
  p += strlen(pat);
  while (*p == ' ') p++;
  return atof(p);
}

float getNetworkFloat(const char *buf, const char *subKey) {
  const char *p = strstr(buf, "\"network_speed\"");
  if (!p) return NAN;
  p = strchr(p, '{');
  if (!p) return NAN;
  p++;
  p = strchr(p, '\"');
  if (!p) return NAN;
  p = strchr(p + 1, '\"');
  if (!p) return NAN;
  const char *obj = strchr(p, '{');
  if (!obj) return NAN;
  const char *objEnd = strchr(obj + 1, '}');
  char pat[32];
  snprintf(pat, sizeof(pat), "\"%s\":", subKey);
  const char *q = strstr(obj, pat);
  if (!q || (objEnd && q > objEnd)) return NAN;
  q += strlen(pat);
  while (*q == ' ') q++;
  return atof(q);
}

// 解析 JSON 数据并更新内部状态（UDP 和 MQTT 共用）
void processData(const char *buf) {
  ecpu = getJsonFloat(buf, "cpu_percent");
  pcpu = getJsonFloat(buf, "mem_percent");
  gpu  = getJsonFloat(buf, "disk_percent");
  ane  = getJsonFloat(buf, "gpu_percent");

  // 优先直接获取顶层 rx_kb_s / tx_kb_s，兼容嵌套在 network_speed 下的格式
  mem  = getJsonFloat(buf, "rx_kb_s");
  if (isnan(mem)) mem = getNetworkFloat(buf, "rx_kb_s");

  pwr  = getJsonFloat(buf, "tx_kb_s");
  if (isnan(pwr)) pwr = getNetworkFloat(buf, "tx_kb_s");

  if (isnan(ecpu)) return;
  if (isnan(pcpu)) pcpu = 0;
  if (isnan(gpu))  gpu  = 0;
  if (isnan(ane))  ane  = 0;
  if (isnan(mem))  mem  = 0;
  if (isnan(pwr))  pwr  = 0;

  if (mem > netRxMax) netRxMax = mem;
  if (pwr > netTxMax) netTxMax = pwr;
  if (netRxMax < 1.0f) netRxMax = 1.0f;
  if (netTxMax < 1.0f) netTxMax = 1.0f;

  if (!haveData) {
    s_ecpu = ecpu; s_pcpu = pcpu; s_gpu = gpu; s_mem = mem; s_pwr = pwr;
  } else {
    s_ecpu = EMA_ALPHA * ecpu + (1.0f - EMA_ALPHA) * s_ecpu;
    s_pcpu = EMA_ALPHA * pcpu + (1.0f - EMA_ALPHA) * s_pcpu;
    s_gpu  = EMA_ALPHA * gpu  + (1.0f - EMA_ALPHA) * s_gpu;
    s_mem  = EMA_ALPHA * mem  + (1.0f - EMA_ALPHA) * s_mem;
    s_pwr  = EMA_ALPHA * pwr  + (1.0f - EMA_ALPHA) * s_pwr;
  }
  haveData = true;
}

// ---------- UDP 解析 ----------
bool parseUdp() {
  int len = udp.parsePacket();
  if (len <= 0) return false;
  char *buf = (char *)malloc(1024);
  if (!buf) return false;
  int n = udp.read(buf, 1023);
  if (n <= 0) { free(buf); return false; }
  buf[n] = 0;

  processData(buf);
  free(buf);
  return true;
}

// ---------- MQTT ----------
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // 拼接 payload 为字符串
  char buf[1024];
  unsigned int copyLen = length < sizeof(buf) - 1 ? length : sizeof(buf) - 1;
  memcpy(buf, payload, copyLen);
  buf[copyLen] = 0;

  processData(buf);
}

bool mqttReconnect() {
  if (mqtt.connected()) return true;
  if (mqttBroker.length() == 0 || mqttTopic.length() == 0) return false;

  // 匿名连接，不需要用户名密码
  String clientId = "FNMON-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (mqtt.connect(clientId.c_str())) {
    mqttConnected = true;
    mqtt.subscribe(mqttTopic.c_str());
    return true;
  }
  mqttConnected = false;
  return false;
}

void mqttLoop() {
  if (!mqttEnabled || mqttBroker.length() == 0) return;

  if (!mqtt.connected()) {
    unsigned long now = millis();
    if (now - lastMqttReconnect >= MQTT_RECONNECT_INTERVAL) {
      lastMqttReconnect = now;
      mqttReconnect();
    }
  } else {
    mqtt.loop();
  }
}

// ---------- Web 处理 ----------
void handleRoot() {
  if (state == S_PROVISION) {
    String html = String(
      "<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<style>body{font-family:sans-serif;text-align:center;margin-top:40px;}"
      "input{font-size:18px;padding:8px;width:80%;margin:6px;}"
      "button{font-size:18px;padding:10px 24px;margin-top:12px;}</style>"
      "</head><body><h2>FnMon WiFi 配置</h2>"
      "<form method='POST' action='/save'>"
      "<p>WiFi 名称 (SSID)</p><input name='ssid' placeholder='请输入 WiFi 名称' required>"
      "<p>WiFi 密码</p><input type='password' name='pass' placeholder='请输入 WiFi 密码'>"
      "<br><button type='submit'>保存并连接</button></form></body></html>");
    server.send(200, "text/html", html);
    return;
  }

  String sourceOptions = "";
  for (int i = 0; i < 6; i++) {
    sourceOptions += "<option value='" + String(i) + "'>" + String(sourceLabels[i]) + "</option>";
  }

  String rowConfigs = "";
  for (int i = 0; i < 5; i++) {
    rowConfigs += "<div class='row-config'>";
    rowConfigs += "<div><label>行 " + String(i+1) + " 标题(3字母)</label>";
    rowConfigs += "<input name='lbl" + String(i) + "' maxlength='3' value='" + lbl[i] + "' style='width:60px;'></div>";
    rowConfigs += "<div><label>数据源</label>";
    rowConfigs += "<select name='src" + String(i) + "'>";
    for (int j = 0; j < 6; j++) {
      rowConfigs += "<option value='" + String(j) + "'";
      if (dataSource[i] == j) rowConfigs += " selected";
      rowConfigs += ">" + String(sourceLabels[j]) + "</option>";
    }
    rowConfigs += "</select></div>";
    rowConfigs += "</div>";
  }

  // MQTT 连接状态指示
  String mqttStatusHtml = "";
  if (mqttEnabled && mqttBroker.length() > 0) {
    if (mqttConnected) {
      mqttStatusHtml = "<div class='status-ok'>✅ MQTT 已连接 — " + mqttBroker + ":" + String(mqttPort) + " / " + mqttTopic + "</div>";
    } else {
      mqttStatusHtml = "<div class='status-err'>❌ MQTT 未连接 — " + mqttBroker + ":" + String(mqttPort) + "</div>";
    }
  } else {
    mqttStatusHtml = "<div class='status-off'>⚪ MQTT 未启用</div>";
  }

  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>"
    "body{font-family:sans-serif;max-width:650px;margin:20px auto;padding:15px;background:#f5f5f5;}"
    "div.card{background:#fff;padding:20px;border-radius:8px;box-shadow:0 2px 8px rgba(0,0,0,0.1);margin-bottom:15px;}"
    "h2{margin-top:0;color:#333;}label{display:block;margin:8px 0 4px;font-weight:bold;}"
    "input,select{width:100%;padding:6px;box-sizing:border-box;font-size:14px;margin-bottom:8px;}"
    "input[type='color']{height:40px;padding:2px;cursor:pointer;}"
    ".row-config{display:flex;gap:12px;align-items:center;border-bottom:1px solid #eee;padding:6px 0;}"
    ".row-config div{flex:1;}"
    ".row-config label{font-size:12px;margin:2px 0;}"
    ".row-config input,.row-config select{width:100%;margin:2px 0;font-size:13px;}"
    ".row-config input{width:70px;}"
    ".color-row{display:flex;align-items:center;justify-content:space-between;margin-bottom:6px;}"
    ".color-row label{margin:0;}"
    ".color-row input{width:80px;margin:0;}"
    "button{background:#007bff;color:#fff;border:none;padding:10px 16px;font-size:15px;border-radius:4px;width:100%;cursor:pointer;}"
    "button:hover{background:#0056b3;}"
    "button.warning{background:#6c757d;}"
    "button.warning:hover{background:#5a6268;}"
    ".status-ok{background:#d4edda;color:#155724;padding:10px;border-radius:6px;margin-bottom:12px;font-weight:bold;}"
    ".status-err{background:#f8d7da;color:#721c24;padding:10px;border-radius:6px;margin-bottom:12px;font-weight:bold;}"
    ".status-off{background:#e2e3e5;color:#383d41;padding:10px;border-radius:6px;margin-bottom:12px;}"
    ".toggle-row{display:flex;align-items:center;gap:10px;margin-bottom:8px;}"
    ".toggle-row input[type=checkbox]{width:auto;}"
    "</style></head><body>"
    "<div class='card'><h2>⚡ FnMon 设置后台</h2>"
    "<form method='POST' action='/config'>"
    "<label>服务器名称 (顶部标题行)</label><input name='srv_name' maxlength='16' value='" + serverTitle + "' placeholder='例如: FNOS / DS920+ / NAS'>"
    "<label>屏幕亮度 (1-255)</label><input type='number' name='brightness' min='1' max='255' value='" + String(brightness) + "'>"
    "<label>轮播模式</label><select name='autocycle'>"
    "<option value='0'" + String(!autoCycle ? " selected" : "") + ">关闭 (按键手动切换)</option>"
    "<option value='1'" + String(autoCycle ? " selected" : "") + ">开启 (自动定时轮播)</option>"
    "</select>"
    "<label>轮播切换间隔 (秒)</label><input type='number' name='cycle_sec' min='2' max='3600' value='" + String(cycleIntervalSec) + "'>"
    "<h3>📊 每行显示配置 (共5行指标)</h3>"
    + rowConfigs +
    "<h3>🎨 各栏目颜色设置</h3>"
    "<div class='color-row'><label>顶部标题颜色</label><input type='color' name='c_title' value='" + colColorTitle + "'></div>"
    "<div class='color-row'><label>ECPU 颜色</label><input type='color' name='c_ecpu' value='" + colColorECP + "'></div>"
    "<div class='color-row'><label>PCPU 颜色</label><input type='color' name='c_pcpu' value='" + colColorPCP + "'></div>"
    "<div class='color-row'><label>GPU 颜色</label><input type='color' name='c_gpu' value='" + colColorGPU + "'></div>"
    "<div class='color-row'><label>MEM 颜色</label><input type='color' name='c_mem' value='" + colColorMEM + "'></div>"
    "<div class='color-row'><label>PWR 颜色</label><input type='color' name='c_pwr' value='" + colColorPWR + "'></div>"
    "<button type='submit'>💾 保存配置并生效</button></form></div>"

    "<div class='card'><h3>📡 数据源配置</h3>"
    "<form method='POST' action='/config'>"
    "<p style='color:#666;margin-top:0;'>配置 UDP 或 MQTT 接收服务器监控数据</p>"

    "<h4>🔌 UDP 接收</h4>"
    "<label>UDP 接收端口</label><input type='number' name='port' value='" + String(udpPort) + "'>"

    "<h4>📶 MQTT 接收</h4>"
    + mqttStatusHtml +
    "<div class='toggle-row'><input type='checkbox' name='mqtt_enabled' id='mqtt_enabled' value='1'" + String(mqttEnabled ? " checked" : "") + "><label for='mqtt_enabled' style='margin:0;'>启用 MQTT</label></div>"
    "<label>MQTT Broker 地址</label><input name='mqtt_broker' value='" + mqttBroker + "' placeholder='例如: 192.168.1.100 或 broker.example.com'>"
    "<label>MQTT 端口</label><input type='number' name='mqtt_port' value='" + String(mqttPort) + "'>"
    "<label>MQTT Topic</label><input name='mqtt_topic' value='" + mqttTopic + "' placeholder='例如: servermon/stats'>"
    "<button type='submit'>💾 保存数据源配置</button></form></div>"

    "<div class='card'><h3>📶 WiFi 配网修改</h3>"
    "<form method='POST' action='/save'>"
    "<label>WiFi SSID</label><input name='ssid' value='" + savedSSID + "'>"
    "<label>WiFi 密码</label><input type='password' name='pass' placeholder='修改密码'>"
    "<button type='submit' class='warning'>🔄 重置 WiFi 重新连接</button></form></div>"
    "</body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  ssid.trim(); pass.trim();
  if (ssid.length() == 0) {
    server.send(200, "text/html", "<h3>SSID 不能为空</h3>");
    return;
  }
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  savedSSID = ssid;
  savedPass = pass;
  server.send(200, "text/html", "<h3>配置成功，设备正在重启...</h3>");
  credsJustSaved = true;
  credsSavedAt = millis();
}

void handleConfigSave() {
  if (server.hasArg("srv_name")) {
    serverTitle = server.arg("srv_name");
    serverTitle.trim();
    if (serverTitle.length() > 16) serverTitle = serverTitle.substring(0, 16);
    prefs.putString("srv_name", serverTitle);
  }
  if (server.hasArg("c_title")) {
    colColorTitle = server.arg("c_title");
    prefs.putString("c_title", colColorTitle);
  }
  if (server.hasArg("brightness")) brightness = server.arg("brightness").toInt();
  if (server.hasArg("autocycle")) autoCycle = (server.arg("autocycle") == "1");
  if (server.hasArg("cycle_sec")) cycleIntervalSec = server.arg("cycle_sec").toInt();
  if (server.hasArg("c_ecpu")) colColorECP = server.arg("c_ecpu");
  if (server.hasArg("c_pcpu")) colColorPCP = server.arg("c_pcpu");
  if (server.hasArg("c_gpu"))  colColorGPU  = server.arg("c_gpu");
  if (server.hasArg("c_mem"))  colColorMEM  = server.arg("c_mem");
  if (server.hasArg("c_pwr"))  colColorPWR  = server.arg("c_pwr");

  // UDP 端口
  if (server.hasArg("port")) {
    udpPort = server.arg("port").toInt();
    prefs.putUInt("port", udpPort);
  }

  // MQTT 配置
  mqttEnabled = server.hasArg("mqtt_enabled");
  prefs.putBool("mqtt_en", mqttEnabled);

  if (server.hasArg("mqtt_broker")) {
    mqttBroker = server.arg("mqtt_broker");
    mqttBroker.trim();
    prefs.putString("mqtt_brk", mqttBroker);
  }
  if (server.hasArg("mqtt_port")) {
    mqttPort = server.arg("mqtt_port").toInt();
    if (mqttPort == 0) mqttPort = 1883;
    prefs.putUInt("mqtt_pt", mqttPort);
  }
  if (server.hasArg("mqtt_topic")) {
    mqttTopic = server.arg("mqtt_topic");
    mqttTopic.trim();
    prefs.putString("mqtt_top", mqttTopic);
  }

  // MQTT 配置变更，重置连接状态，让主循环自动重连
  mqtt.disconnect();
  mqttConnected = false;
  lastMqttReconnect = 0;

  for (int i = 0; i < 5; i++) {
    String key = "lbl" + String(i);
    if (server.hasArg(key)) {
      String val = server.arg(key);
      val.trim();
      if (val.length() > 3) val = val.substring(0, 3);
      if (val.length() > 0) {
        lbl[i] = val;
        prefs.putString(key.c_str(), val);
      }
    }
    key = "src" + String(i);
    if (server.hasArg(key)) {
      uint8_t src = server.arg(key).toInt();
      if (src < 6) {
        dataSource[i] = src;
        prefs.putUChar(key.c_str(), src);
      }
    }
  }

  prefs.putUChar("bright", brightness);
  prefs.putBool("autocycle", autoCycle);
  prefs.putUInt("cyclesec", cycleIntervalSec);
  prefs.putString("c_ecpu", colColorECP);
  prefs.putString("c_pcpu", colColorPCP);
  prefs.putString("c_gpu", colColorGPU);
  prefs.putString("c_mem", colColorMEM);
  prefs.putString("c_pwr", colColorPWR);

  dma_display->setBrightness8(brightness);
  udp.stop();
  udp.begin(udpPort);

  server.send(200, "text/html", "<h3>✅ 配置已成功更新并生效！</h3><a href='/'>返回设置后台</a>");
  if (state == S_DATA && haveData) showData();
}

// ---------- 配网 / 连接 ----------
void startProvisioning() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/config", HTTP_POST, handleConfigSave);
  server.begin();
  state = S_PROVISION;
  lastChange = millis();
}

void connectSaved() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(savedSSID.c_str(), savedPass.c_str());
  state = S_CONNECT;
  lastChange = millis();
}

void checkButtonAndCycle() {
  if (digitalRead(BUTTON_PIN) == LOW) {
    if (millis() - lastBtnPress > 300) {
      displayMode = (displayMode == MODE_TEXT) ? MODE_GRAPHIC : MODE_TEXT;
      lastBtnPress = millis();
      lastCycleTime = millis();
      if (state == S_DATA && haveData) {
        showData();
      }
    }
  }

  if (autoCycle && state == S_DATA && haveData) {
    if (millis() - lastCycleTime >= (unsigned long)cycleIntervalSec * 1000) {
      displayMode = (displayMode == MODE_TEXT) ? MODE_GRAPHIC : MODE_TEXT;
      lastCycleTime = millis();
      showData();
    }
  }
}

// ---------- 尝试不同的面板配置 ----------
bool initDisplay(int mode) {
  HUB75_I2S_CFG mxconfig(PANEL_RES_X, PANEL_RES_Y, PANEL_CHAIN);
  mxconfig.gpio.e = E_PIN;
  mxconfig.double_buff = true;

  if (mode == 1) {
    // 默认配置，只设E引脚
  } else if (mode == 2) {
    // 显式指定所有引脚
    mxconfig.gpio.r1 = R1_PIN;
    mxconfig.gpio.g1 = G1_PIN;
    mxconfig.gpio.b1 = B1_PIN;
    mxconfig.gpio.r2 = R2_PIN;
    mxconfig.gpio.g2 = G2_PIN;
    mxconfig.gpio.b2 = B2_PIN;
    mxconfig.gpio.a = A_PIN;
    mxconfig.gpio.b = B_PIN;
    mxconfig.gpio.c = C_PIN;
    mxconfig.gpio.d = D_PIN;
    mxconfig.gpio.e = E_PIN;
    mxconfig.gpio.lat = LAT_PIN;
    mxconfig.gpio.oe = OE_PIN;
    mxconfig.gpio.clk = CLK_PIN;
  } else if (mode == 3) {
    // 显式指定所有引脚并降低时钟
    mxconfig.gpio.r1 = R1_PIN;
    mxconfig.gpio.g1 = G1_PIN;
    mxconfig.gpio.b1 = B1_PIN;
    mxconfig.gpio.r2 = R2_PIN;
    mxconfig.gpio.g2 = G2_PIN;
    mxconfig.gpio.b2 = B2_PIN;
    mxconfig.gpio.a = A_PIN;
    mxconfig.gpio.b = B_PIN;
    mxconfig.gpio.c = C_PIN;
    mxconfig.gpio.d = D_PIN;
    mxconfig.gpio.e = E_PIN;
    mxconfig.gpio.lat = LAT_PIN;
    mxconfig.gpio.oe = OE_PIN;
    mxconfig.gpio.clk = CLK_PIN;
    mxconfig.i2sspeed = HUB75_I2S_CFG::HZ_8M;
  } else if (mode == 4) {
    // 1/16扫描模式
    mxconfig.gpio.r1 = R1_PIN;
    mxconfig.gpio.g1 = G1_PIN;
    mxconfig.gpio.b1 = B1_PIN;
    mxconfig.gpio.r2 = R2_PIN;
    mxconfig.gpio.g2 = G2_PIN;
    mxconfig.gpio.b2 = B2_PIN;
    mxconfig.gpio.a = A_PIN;
    mxconfig.gpio.b = B_PIN;
    mxconfig.gpio.c = C_PIN;
    mxconfig.gpio.d = D_PIN;
    mxconfig.gpio.e = E_PIN;
    mxconfig.gpio.lat = LAT_PIN;
    mxconfig.gpio.oe = OE_PIN;
    mxconfig.gpio.clk = CLK_PIN;
    mxconfig.mx_height = 32;
    mxconfig.double_buff = true;
  } else if (mode == 5) {
    // 交换R1/R2等引脚
    mxconfig.gpio.r1 = R2_PIN;
    mxconfig.gpio.g1 = G2_PIN;
    mxconfig.gpio.b1 = B2_PIN;
    mxconfig.gpio.r2 = R1_PIN;
    mxconfig.gpio.g2 = G1_PIN;
    mxconfig.gpio.b2 = B1_PIN;
    mxconfig.gpio.a = A_PIN;
    mxconfig.gpio.b = B_PIN;
    mxconfig.gpio.c = C_PIN;
    mxconfig.gpio.d = D_PIN;
    mxconfig.gpio.e = E_PIN;
    mxconfig.gpio.lat = LAT_PIN;
    mxconfig.gpio.oe = OE_PIN;
    mxconfig.gpio.clk = CLK_PIN;
  } else if (mode == 6) {
    // 无E引脚
    mxconfig.gpio.r1 = R1_PIN;
    mxconfig.gpio.g1 = G1_PIN;
    mxconfig.gpio.b1 = B1_PIN;
    mxconfig.gpio.r2 = R2_PIN;
    mxconfig.gpio.g2 = G2_PIN;
    mxconfig.gpio.b2 = B2_PIN;
    mxconfig.gpio.a = A_PIN;
    mxconfig.gpio.b = B_PIN;
    mxconfig.gpio.c = C_PIN;
    mxconfig.gpio.d = D_PIN;
    mxconfig.gpio.lat = LAT_PIN;
    mxconfig.gpio.oe = OE_PIN;
    mxconfig.gpio.clk = CLK_PIN;
    mxconfig.mx_height = 16;
    mxconfig.double_buff = true;
  }

  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  return dma_display->begin();
}

// ---------- setup ----------
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("Starting...");

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // 读取配置
  prefs.begin("macmon", false);
  savedSSID = prefs.getString("ssid", "");
  savedPass = prefs.getString("pass", "");
  udpPort   = prefs.getUInt("port", 9000);
  brightness = prefs.getUChar("bright", 64);
  autoCycle = prefs.getBool("autocycle", false);
  cycleIntervalSec = prefs.getUInt("cyclesec", 10);
  colColorECP = prefs.getString("c_ecpu", "#00FF00");
  colColorPCP = prefs.getString("c_pcpu", "#00FF00");
  colColorGPU = prefs.getString("c_gpu",  "#00FF00");
  colColorMEM = prefs.getString("c_mem",  "#00FF00");
  colColorPWR = prefs.getString("c_pwr",  "#FFFFFF");
  colColorTitle = prefs.getString("c_title", "#FFFFFF");
  serverTitle = prefs.getString("srv_name", "FNOS");

  // 读取 MQTT 配置
  mqttEnabled = prefs.getBool("mqtt_en", false);
  mqttBroker = prefs.getString("mqtt_brk", "");
  mqttPort = prefs.getUInt("mqtt_pt", 1883);
  mqttTopic = prefs.getString("mqtt_top", "");

  for (int i = 0; i < 5; i++) {
    String key = "lbl" + String(i);
    String defaultLabel = lbl[i];
    String val = prefs.getString(key.c_str(), defaultLabel);
    if (val.length() > 0) lbl[i] = val;
  }
  for (int i = 0; i < 5; i++) {
    String key = "src" + String(i);
    dataSource[i] = prefs.getUChar(key.c_str(), dataSource[i]);
  }

  // ===== 尝试不同配置模式 =====
  bool success = false;
  int modes[] = {1, 2, 3, 4, 5, 6};
  const char* modeNames[] = {"默认", "完整配置(1/32)", "慢速时钟", "1/16扫描", "交换R1/R2", "无E引脚(32x32)"};

  for (int i = 0; i < 6 && !success; i++) {
    Serial.printf("尝试配置模式 %d: %s\n", modes[i], modeNames[i]);

    // 删除旧的display对象
    if (dma_display != nullptr) {
      delete dma_display;
      dma_display = nullptr;
      delay(100);
    }

    success = initDisplay(modes[i]);

    if (success) {
      Serial.printf("配置模式 %d 成功!\n", modes[i]);
      dma_display->setBrightness8(brightness);
      dma_display->clearScreen();
      break;
    } else {
      Serial.printf("配置模式 %d 失败\n", modes[i]);
    }
  }

  if (!success) {
    Serial.println("所有配置模式都失败！");
    // 尝试最后一次使用最简单的配置
    HUB75_I2S_CFG mxconfig(PANEL_RES_X, PANEL_RES_Y, PANEL_CHAIN);
    mxconfig.double_buff = true;
    dma_display = new MatrixPanel_I2S_DMA(mxconfig);
    if (dma_display->begin()) {
      Serial.println("使用最小配置成功");
      dma_display->setBrightness8(brightness);
      dma_display->clearScreen();
    } else {
      Serial.println("所有尝试都失败，程序将挂起");
      while(1) { delay(1000); }
    }
  }

  // 初始化 MQTT
  mqtt.setServer(mqttBroker.c_str(), mqttPort);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(2048);

  if (savedSSID.length() > 0) {
    Serial.println("saved WiFi found, connecting...");
    connectSaved();
  } else {
    Serial.println("no saved WiFi, starting provisioning");
    startProvisioning();
    showProvision();
  }
}

// ---------- loop ----------
void loop() {
  checkButtonAndCycle();

  if (state == S_PROVISION) {
    server.handleClient();
    if (millis() - lastRedraw >= 250) {
      showProvision();
      lastRedraw = millis();
    }
    if (credsJustSaved && millis() - credsSavedAt >= 1500) {
      ESP.restart();
    }
  }
  else if (state == S_CONNECT) {
    if (WiFi.status() == WL_CONNECTED) {
      state = S_SHOWIP;
      lastChange = millis();
      showIP();
      Serial.println("WiFi connected: " + WiFi.localIP().toString());
    } else if (millis() - lastChange >= 30000) {
      startProvisioning();
      showProvision();
    } else {
      showConnecting();
      delay(400);
    }
  }
  else if (state == S_SHOWIP) {
    if (millis() - lastChange >= 5000) {
      udp.begin(udpPort);
      server.on("/", handleRoot);
      server.on("/save", HTTP_POST, handleSave);
      server.on("/config", HTTP_POST, handleConfigSave);
      server.begin();

      // WiFi 连上后尝试 MQTT 连接
      if (mqttEnabled && mqttBroker.length() > 0 && mqttTopic.length() > 0) {
        mqtt.setServer(mqttBroker.c_str(), mqttPort);
        mqtt.setCallback(mqttCallback);
        mqttReconnect();
      }

      state = S_DATA;
      lastChange = millis();
    }
  }
  else if (state == S_DATA) {
    server.handleClient();
    bool updated = parseUdp();
    mqttLoop();
    if (!haveData && millis() - lastChange > 6000) {
      dma_display->fillScreen(0);
      drawTextFit(32, 26, "WAIT", COLOR_GREEN);
      drawTextFit(32, 38, "DATA", COLOR_WHITE);
      dma_display->flipDMABuffer();
      delay(200);
    } else if (updated) {
      showData();
    } else {
      delay(10);
    }
  }
}
