/*
  PS2 無線手把 → 電腦（無人機足球模擬器）
  ------------------------------------------------------------
  板子：Arduino UNO
  函式庫：PS2X_lib  https://github.com/madsci1016/Arduino-PS2X

  接線（與你原本的接線相同）：
    DAT -> D12    CMD -> D11    ATT -> D10    CLK -> D13
    GND -> GND    VCC -> 3.3V（傑森創工教學建議 3.3V，不要接 5V）

  序列埠：115200 baud
  ------------------------------------------------------------
  輸出格式（給網頁用，每 20 ms 一行）：
      J,LX,LY,RX,RY,BTN,MODE
    LX/LY/RX/RY = 0~255（中心約 128，上=0、左=0）
    BTN  = 16 位元按鍵遮罩（十六進位，按下=1）
    MODE = 手把模式位元組（73=類比模式紅燈，41=數位模式，79=壓力模式）
  以 # 開頭的行是給人看的訊息，網頁會自動忽略。

  在 Arduino「序列埠監控視窗」輸入：
    d  切換診斷模式（每 0.25 秒顯示原始值與每軸最小/最大值）
    r  重設最小/最大值統計
    c  重新設定（重新配對）手把
  ------------------------------------------------------------
  ★ X 軸不動的排查（診斷模式會直接告訴你是哪一種）：
    1. MODE 不是 73/79：手把不在類比模式 → 按 ANALOG 鍵讓紅燈亮，再輸入 c。
    2. 某軸 min/max 完全不變（例如 LX 一直 128）：
       a. 接收器改接 3.3V，GND 確實共地。
       b. 有些副廠無線手把有「MODE」鍵，會切換左搖桿與十字鍵，按一次再測。
       c. 通訊時序太快：打開 libraries/PS2X_lib/PS2X_lib.h，
          在 #ifdef __AVR__ 區段把
              #define CTRL_CLK        4   改成 10
              #define CTRL_BYTE_DELAY 3   改成 15
          存檔後重新上傳。無線接收器常需要較慢的時脈。
       d. DAT 腳加 10kΩ 上拉電阻到 3.3V。
    3. 數值在跳但範圍很小：搖桿電位器接觸不良或手把電池電量不足。
*/

#include <PS2X_lib.h>

PS2X ps2x;

const byte PS2_DAT = 12;
const byte PS2_CMD = 11;
const byte PS2_ATT = 10;
const byte PS2_CLK = 13;

const unsigned long SEND_INTERVAL_MS = 20;    // 50 Hz 給網頁
const unsigned long DIAG_INTERVAL_MS = 250;   // 診斷模式 4 Hz
const unsigned long RETRY_INTERVAL_MS = 2000;

bool controllerReady = false;
bool diagMode = false;
unsigned long lastInitTime = 0;
unsigned long lastSendTime = 0;
unsigned long lastDiagTime = 0;
unsigned long lastModeWarn = 0;

// 每軸最小/最大值，用來判斷某軸是否「完全沒動」
byte axMin[4], axMax[4];
const char* const AX_NAME[4] = {"LX", "LY", "RX", "RY"};

void resetStats() {
  for (byte i = 0; i < 4; i++) { axMin[i] = 255; axMax[i] = 0; }
}

void initializeController() {
  Serial.println(F("# Connecting to PS2 controller..."));

  // 參數順序：CLK、CMD、ATT、DAT、壓力感測、震動
  byte error = ps2x.config_gamepad(PS2_CLK, PS2_CMD, PS2_ATT, PS2_DAT, false, false);

  Serial.print(F("# Config code = "));
  Serial.println(error);

  if (error == 0) {
    controllerReady = true;
    resetStats();
    Serial.print(F("# PS2 READY, type = "));
    switch (ps2x.readType()) {
      case 1:  Serial.println(F("DualShock")); break;
      case 2:  Serial.println(F("GuitarHero")); break;
      case 3:  Serial.println(F("Wireless Sony DualShock")); break;
      default: Serial.println(F("Unknown (common for wireless clones, OK)")); break;
    }
    Serial.println(F("# Type d + Enter for diagnostics."));
  } else {
    controllerReady = false;
    switch (error) {
      case 1: Serial.println(F("# No controller found. Check wiring / 3.3V / pairing.")); break;
      case 2: Serial.println(F("# Controller rejected config. Press ANALOG, check pairing.")); break;
      case 3: Serial.println(F("# Pressure mode config failed.")); break;
      default: Serial.println(F("# Unknown init error.")); break;
    }
    Serial.println(F("# Retrying in 2 s..."));
  }
}

// 取得四軸（固定順序 LX, LY, RX, RY）
void readAxes(byte ax[4]) {
  ax[0] = ps2x.Analog(PSS_LX);
  ax[1] = ps2x.Analog(PSS_LY);
  ax[2] = ps2x.Analog(PSS_RX);
  ax[3] = ps2x.Analog(PSS_RY);
}

// 手把回傳的第 2 個位元組 = 模式 ID
// PS2X_lib 的 Analog(n) 直接回傳 PS2data[n]，n=1 就是模式位元組
byte readModeByte() {
  return ps2x.Analog(1);
}

bool isAnalogMode(byte mode) {
  return mode == 0x73 || mode == 0x79;
}

void sendFrame(const byte ax[4], unsigned int buttons, byte mode) {
  Serial.print('J');
  for (byte i = 0; i < 4; i++) { Serial.print(','); Serial.print(ax[i]); }
  Serial.print(',');
  Serial.print(buttons, HEX);
  Serial.print(',');
  Serial.println(mode, HEX);
}

void printDiagnostics(const byte ax[4], unsigned int buttons, byte mode) {
  Serial.print(F("# mode=0x"));
  Serial.print(mode, HEX);
  Serial.print(isAnalogMode(mode) ? F(" (analog) ") : F(" (NOT analog!) "));
  for (byte i = 0; i < 4; i++) {
    Serial.print(AX_NAME[i]);
    Serial.print('=');
    Serial.print(ax[i]);
    Serial.print('[');
    Serial.print(axMin[i]);
    Serial.print('-');
    Serial.print(axMax[i]);
    Serial.print(F("] "));
  }
  Serial.print(F("btn=0x"));
  Serial.println(buttons, HEX);

  // 自動判讀：某軸全程沒變化
  for (byte i = 0; i < 4; i++) {
    if (axMax[i] >= axMin[i] && (axMax[i] - axMin[i]) < 4) {
      // 只有在其他軸確實有動過時才提示，避免剛開機誤判
      bool othersMoved = false;
      for (byte j = 0; j < 4; j++) if (j != i && axMax[j] - axMin[j] > 60) othersMoved = true;
      if (othersMoved) {
        Serial.print(F("#   -> "));
        Serial.print(AX_NAME[i]);
        Serial.println(F(" never changed. See X-axis checklist at top of sketch."));
      }
    }
  }
}

void handleSerialCommands() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'd' || c == 'D') {
      diagMode = !diagMode;
      Serial.println(diagMode ? F("# Diagnostics ON") : F("# Diagnostics OFF"));
    } else if (c == 'r' || c == 'R') {
      resetStats();
      Serial.println(F("# Min/max reset. Move every stick to all four corners."));
    } else if (c == 'c' || c == 'C') {
      controllerReady = false;
      initializeController();
      lastInitTime = millis();
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);  // 等待接收器上電、與手把配對
  Serial.println();
  Serial.println(F("# === PS2 -> Drone Soccer bridge ==="));
  resetStats();
  initializeController();
  lastInitTime = millis();
}

void loop() {
  handleSerialCommands();

  if (!controllerReady) {
    if (millis() - lastInitTime >= RETRY_INTERVAL_MS) {
      initializeController();
      lastInitTime = millis();
    }
    return;
  }

  ps2x.read_gamepad(false, 0);

  byte ax[4];
  readAxes(ax);
  byte mode = readModeByte();
  unsigned int buttons = ps2x.ButtonDataByte();  // 按下 = 1

  for (byte i = 0; i < 4; i++) {
    if (ax[i] < axMin[i]) axMin[i] = ax[i];
    if (ax[i] > axMax[i]) axMax[i] = ax[i];
  }

  // 不在類比模式時，每 3 秒提醒一次
  if (!isAnalogMode(mode) && millis() - lastModeWarn > 3000) {
    lastModeWarn = millis();
    Serial.print(F("# Mode 0x"));
    Serial.print(mode, HEX);
    Serial.println(F(" is not analog. Press ANALOG (red LED on), then send c."));
  }

  unsigned long now = millis();
  if (diagMode) {
    if (now - lastDiagTime >= DIAG_INTERVAL_MS) {
      lastDiagTime = now;
      printDiagnostics(ax, buttons, mode);
    }
  } else if (now - lastSendTime >= SEND_INTERVAL_MS) {
    lastSendTime = now;
    sendFrame(ax, buttons, mode);
  }

  delay(5);
}
