/*
  Claude Lamp — ESP32-C3 固件
  硬件：ESP32-C3 + 24颗 WS2812 灯珠（数据线接 GPIO3）
  功能：通过 BLE 接收命令控制灯珠颜色
  协议：模拟 Moonside 灯的 Nordic UART Service (NUS)

  支持的命令（由 Mac 上的 moonside_daemon.py 发送）：
    LEDON               — 开灯
    LEDOFF              — 关灯
    COLORrrrgggbbb      — 设置颜色（每个分量3位，如 COLOR255180050）
    BRIGHbbb            — 设置亮度（0-120，如 BRIGH060）
    THEME.xxx.r,g,b,... — 工作状态（取第一组颜色显示）
*/

#include <NimBLEDevice.h>
#include <Adafruit_NeoPixel.h>

// ── 硬件配置 ──────────────────────────────────────────────
#define DATA_PIN   3    // WS2812 数据线引脚
#define NUM_LEDS   24   // 灯珠数量

// ── BLE 配置 ──────────────────────────────────────────────
// 设备名以 MOONSIDE 开头，daemon 会按名字搜索
#define DEVICE_NAME   "MOONSIDE-LAMP"

// Nordic UART Service UUID
#define NUS_SERVICE_UUID  "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX_UUID       "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  // Mac 写入
#define NUS_TX_UUID       "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  // ESP32 通知

// ── 全局状态 ──────────────────────────────────────────────
Adafruit_NeoPixel strip(NUM_LEDS, DATA_PIN, NEO_GRB + NEO_KHZ800);

bool deviceConnected = false;
uint8_t currentR = 255, currentG = 255, currentB = 255;
uint8_t currentBrightness = 255;  // 0-255，255 为最大亮度
bool ledsOn = false;

// ── 灯珠控制函数 ──────────────────────────────────────────
void setAllColor(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, strip.Color(r, g, b));
  }
  strip.show();
}

// ── 命令解析 ──────────────────────────────────────────────
void processCommand(const std::string& cmd) {
  Serial.print("收到命令: ");
  Serial.println(cmd.c_str());

  if (cmd == "LEDON") {
    ledsOn = true;
    strip.setBrightness(currentBrightness);
    setAllColor(currentR, currentG, currentB);

  } else if (cmd == "LEDOFF") {
    ledsOn = false;
    setAllColor(0, 0, 0);

  } else if (cmd.substr(0, 5) == "COLOR" && cmd.length() >= 14) {
    // COLOR255180050 → R=255, G=180, B=50
    int r = std::stoi(cmd.substr(5, 3));
    int g = std::stoi(cmd.substr(8, 3));
    int b = std::stoi(cmd.substr(11, 3));
    currentR = r; currentG = g; currentB = b;
    ledsOn = true;
    strip.setBrightness(currentBrightness);
    setAllColor(r, g, b);

  } else if (cmd.substr(0, 5) == "BRIGH" && cmd.length() >= 8) {
    // BRIGH060 → 亮度60（Moonside 范围 0-120，映射到 0-255）
    int val = std::stoi(cmd.substr(5, 3));
    currentBrightness = map(val, 0, 120, 0, 255);
    strip.setBrightness(currentBrightness);
    if (ledsOn) {
      setAllColor(currentR, currentG, currentB);
    }

  } else if (cmd.substr(0, 6) == "THEME.") {
    // THEME 命令：取命令里第一组 R,G,B 显示（不做动画）
    // 格式：THEME.BEAT2.255,255,255,0,0,140,
    size_t secondDot = cmd.find('.', 6);
    if (secondDot != std::string::npos) {
      std::string colorPart = cmd.substr(secondDot + 1);
      int r = 255, g = 255, b = 255;
      size_t c1 = colorPart.find(',');
      size_t c2 = (c1 != std::string::npos) ? colorPart.find(',', c1 + 1) : std::string::npos;
      if (c1 != std::string::npos && c2 != std::string::npos) {
        r = std::stoi(colorPart.substr(0, c1));
        g = std::stoi(colorPart.substr(c1 + 1, c2 - c1 - 1));
        size_t c3 = colorPart.find(',', c2 + 1);
        b = std::stoi(colorPart.substr(c2 + 1, c3 - c2 - 1));
      }
      currentR = r; currentG = g; currentB = b;
    }
    ledsOn = true;
    strip.setBrightness(currentBrightness);
    setAllColor(currentR, currentG, currentB);
  }
}

// ── BLE 回调 ──────────────────────────────────────────────
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) {
    deviceConnected = true;
    Serial.println("Mac 已连接");
  }
  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) {
    deviceConnected = false;
    Serial.println("连接断开，重新广播...");
    NimBLEDevice::startAdvertising();
  }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
    std::string value = pCharacteristic->getValue();
    if (!value.empty()) {
      processCommand(value);
    }
  }
};

// ── 初始化 ────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("Claude Lamp 启动中...");

  // 初始化灯珠
  strip.begin();
  strip.setBrightness(currentBrightness);
  strip.show();  // 全部关闭

  // 初始化 BLE
  NimBLEDevice::init(DEVICE_NAME);

  NimBLEServer* pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  // 创建 NUS 服务
  NimBLEService* pService = pServer->createService(NUS_SERVICE_UUID);

  // RX 特征：Mac 写入命令到这里
  NimBLECharacteristic* pRxChar = pService->createCharacteristic(
    NUS_RX_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pRxChar->setCallbacks(new RxCallbacks());

  // TX 特征：ESP32 可通知 Mac（本项目暂不使用）
  pService->createCharacteristic(
    NUS_TX_UUID,
    NIMBLE_PROPERTY::NOTIFY
  );

  pService->start();

  // 开始广播
  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(NUS_SERVICE_UUID);
  pAdvertising->setName(DEVICE_NAME);
  pAdvertising->start();

  Serial.print("BLE 广播中，设备名: ");
  Serial.println(DEVICE_NAME);
}

// ── 主循环 ────────────────────────────────────────────────
void loop() {
  delay(100);
}
