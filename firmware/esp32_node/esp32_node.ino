#include <LoRa.h>
#include <SPI.h>
#include <DHT.h>

#define PIN_LORA_SS    5
#define PIN_LORA_RST   14
#define PIN_LORA_DIO0 26
#define PIN_DHT        32
#define PIN_SOIL       34
#define PIN_LDR        35
#define PIN_RELAY      22

const String NODE_ID = "NODE_01";
const String SECRET_KEY = "12345";

const unsigned long SEND_INTERVAL = 5000;
const int LORA_RETRY_MAX = 5;

/* ===== HEARTBEAT CONFIG (NEW) ===== */
const unsigned long HEARTBEAT_TIMEOUT = 60000;
unsigned long lastGatewaySeen = 0;

DHT dht(PIN_DHT, DHT11);

float ema_t = 0, ema_h = 0, ema_l = 0;
float alpha = 0.1;

bool pumpStatus = false;
String mode = "READY";

float default_amp = 0.5;
float default_flow = 1.2;
float last_trigger_soil = 0;

unsigned long lastSend = 0;
bool loraReady = false;

int cachedSoil = 0;
unsigned long bootTime = 0;

void initLoRa() {
  LoRa.setPins(PIN_LORA_SS, PIN_LORA_RST, PIN_LORA_DIO0);

  for (int i = 1; i <= LORA_RETRY_MAX; i++) {
    Serial.printf("[LORA] Đang thử kết nối lần %d/%d...\n", i, LORA_RETRY_MAX);
    if (LoRa.begin(433E6)) {
      LoRa.setSyncWord(0xA5);
      LoRa.receive();
      loraReady = true;
      Serial.println("[LORA] Khởi tạo THÀNH CÔNG");
      return;
    }
    delay(1000);
  }
  Serial.println("[LORA][LỖI] Không tìm thấy module LoRa. Đang thử lại...");
  loraReady = false;
}

void processSensors() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (!isnan(t)) ema_t = alpha * t + (1 - alpha) * ema_t;
  if (!isnan(h)) ema_h = alpha * h + (1 - alpha) * ema_h;

  float l = map(analogRead(PIN_LDR), 0, 4095, 0, 100);
  ema_l = alpha * l + (1 - alpha) * ema_l;
}

int readSoil() {
  long sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += analogRead(PIN_SOIL);
    delay(5);
  }
  return constrain(map(sum / 10, 4095, 1000, 0, 100), 0, 100);
}

bool sendUplink(String data) {
  if (!loraReady) return false;
  LoRa.beginPacket();
  LoRa.print(data);
  LoRa.endPacket();
  LoRa.receive();
  return true;
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n[NODE] Đang khởi động...");
  dht.begin();
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, LOW);
  Serial.println("[BOOT] Relay: TẮT | Chế độ: READY");
  unsigned long bootTime = 0;
  initLoRa();
}

void loop() {

  if (loraReady) {
    int packetSize = LoRa.parsePacket();
    if (packetSize) {
      String msg = "";
      while (LoRa.available()) msg += (char)LoRa.read();

      Serial.println("[LORA][RX] Nhận lệnh: " + msg);

      /* ===== HEARTBEAT RX (NEW) ===== */
      if (msg.startsWith("HB")) {
        lastGatewaySeen = millis();
        Serial.println("[HB] RX");
      }

      if (msg.startsWith("CMD")) {
        lastGatewaySeen = millis();  // CMD cũng là heartbeat hợp lệ

        int soil_now = readSoil();

        int p1 = msg.indexOf(',');
        int p2 = msg.indexOf(',', p1 + 1);
        int p3 = msg.indexOf(',', p2 + 1);

        String target = msg.substring(p1 + 1, p2);
        String cmd_id = msg.substring(p2 + 1, p3);
        String action = msg.substring(p3 + 1);
        action.trim();

        if (target == NODE_ID) {
          Serial.println("[CMD] Chấp nhận lệnh ID: " + cmd_id);
          
          if (action == "ON") {
            mode = "CLD";
            pumpStatus = true;
            Serial.println("[CMD] Đang thực hiện BẬT máy bơm...");
          } else if (action == "OFF") {
            mode = "CLD";
            pumpStatus = false;
            Serial.println("[CMD] Đang thực hiện TẮT máy bơm...");
          } else if (action == "AUTO") {
            mode = "SEN";
            Serial.println("[CMD] Chuyển sang chế độ TỰ ĐỘNG (SEN)");
          }

          last_trigger_soil = soil_now;

          String ack = "ACK," + NODE_ID + "," + cmd_id + "," +
                       (pumpStatus ? "ON" : "OFF") + "," +
                       mode + "," + String(last_trigger_soil, 1);
          sendUplink(ack);
          lastSend = millis();
        } else {
          Serial.println("[CMD] Bỏ qua, không phải địa chỉ của Node này");
        }
      }
    }
  } else {
    static unsigned long lastRetry = 0;
    if (millis() - lastRetry > 10000) {
      initLoRa();
      lastRetry = millis();
    }
  }

  processSensors();
  cachedSoil = readSoil();

  if (mode == "SEN") {
    if (cachedSoil < 30 && !pumpStatus) {
      pumpStatus = true;
      last_trigger_soil = cachedSoil;
      Serial.printf("[AUTO] Độ ẩm thấp (%d%%) -> BẬT BƠM\n", cachedSoil);
    } else if (cachedSoil > 70 && pumpStatus) {
      pumpStatus = false;
      last_trigger_soil = cachedSoil;
      Serial.printf("[AUTO] Độ ẩm đủ (%d%%) -> TẮT BƠM\n", cachedSoil);
    }
  }

  digitalWrite(PIN_RELAY, pumpStatus);

  /* ===== HEARTBEAT TIMEOUT CHECK (NEW) ===== */
  if (mode != "SEN" &&
      (
        (lastGatewaySeen == 0 && millis() - bootTime > HEARTBEAT_TIMEOUT) ||
        (lastGatewaySeen > 0 && millis() - lastGatewaySeen > HEARTBEAT_TIMEOUT)
      )
  ) {
    mode = "SEN";
    Serial.println("[FAILOVER] Gateway missing → SEN MODE");
  }

  if (millis() - lastSend > SEND_INTERVAL && loraReady) {
    lastSend = millis();

    String payload = NODE_ID + "," +
                     String(ema_t, 1) + "," +
                     String(ema_h, 1) + "," +
                     String(cachedSoil) + "," +
                     String((int)ema_l) + "," +
                     (pumpStatus ? "1" : "0") + "," +
                     mode + "," +
                     SECRET_KEY + "," +                     
                     "0," +
                     String(default_amp, 1) + "," +
                     String(default_flow, 1) + "," +
                     String(last_trigger_soil, 1);

    Serial.println("[LORA][UPLINK] " + payload);
    sendUplink(payload);
  }
}
