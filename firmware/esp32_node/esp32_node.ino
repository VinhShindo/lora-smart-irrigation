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

const char NODE_ID[] = "NODE_01";

const unsigned long SEND_INTERVAL = 5000;
const int LORA_RETRY_MAX = 5;

const unsigned long HEARTBEAT_TIMEOUT = 60000;
unsigned long lastGatewaySeen = 0;

DHT dht(PIN_DHT, DHT11);

float ema_t = 0, ema_h = 0, ema_l = 0;
float alpha = 0.1;

bool pumpStatus = false;
char mode[8] = "READY";

float default_amp = 0.5;
float default_flow = 1.2;
float last_trigger_soil = 0;

unsigned long lastSend = 0;
bool loraReady = false;

int cachedSoil = 0;
unsigned long bootTime = 0;

/* ===== ADDED: COMMAND PRIORITY LOCK ===== */
bool cmdProcessing = false;
unsigned long cmdLockUntil = 0;
const unsigned long CMD_LOCK_TIME = 2000;
/* ========================================= */

void initLoRa() {
  LoRa.setPins(PIN_LORA_SS, PIN_LORA_RST, PIN_LORA_DIO0);

  for (int i = 1; i <= LORA_RETRY_MAX; i++) {
    Serial.printf("[LORA] Đang thử kết nối lần %d/%d...\n", i, LORA_RETRY_MAX);

    if (LoRa.begin(433E6)) {

      // ===== CẤU HÌNH RADIO (PHẢI GIỐNG GATEWAY) =====
      LoRa.setSyncWord(0xA5);

      LoRa.setSpreadingFactor(9);
      LoRa.setSignalBandwidth(125E3);
      LoRa.setCodingRate4(5);
      LoRa.setTxPower(17);
      LoRa.setPreambleLength(8);
      LoRa.enableCrc();

      LoRa.receive();

      loraReady = true;
      Serial.println("[LORA] Khởi tạo THÀNH CÔNG (Configured)");
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
    int v = analogRead(PIN_SOIL);

    if (v < 100) v = 100;
    if (v > 4000) v = 4000;

    sum += v;
    delay(5);
  }
  int raw = sum / 10;
  int soil = map(raw, 4095, 1000, 0, 100);
  soil = constrain(soil, 0, 100);
  return soil;
}

bool sendUplink(const char* data) {
  if (!loraReady) return false;

  LoRa.beginPacket();
  LoRa.print(data);
  LoRa.endPacket(true);   // wait TX done

  delay(120);              // radio settle

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
  bootTime = millis();
  initLoRa();
}

void loop() {

  /* ===== ADDED: GLOBAL CMD LOCK CHECK ===== */
  // if (cmdProcessing && millis() < cmdLockUntil) {
  //   return;
  // }
  // cmdProcessing = false;
  /* ======================================== */

  if (loraReady) {
    int packetSize = LoRa.parsePacket();
    if (packetSize) {
      char msg[128];
      int i = 0;

      while (LoRa.available() && i < 127) {
        msg[i++] = (char)LoRa.read();
      }

      msg[i] = '\0';

      Serial.print("[LORA][RX] Nhận lệnh: ");
      Serial.println(msg);

      if (strncmp(msg, "HB", 2) == 0) {
        lastGatewaySeen = millis();
        Serial.println("[HB] RX");
      }

      if (strncmp(msg,"CMD",3)==0) {
        if (cmdProcessing && millis() < cmdLockUntil) {
          Serial.println("[CMD] Locked - ignore new CMD");
          return;
        }

        cmdProcessing = true;
        cmdLockUntil = millis() + 1500;

        lastGatewaySeen = millis();

        int soil_now = readSoil();

        char* v[4];
        int idx = 0;

        char* token = strtok(msg, ",");

        while (token && idx < 4) {
          v[idx++] = token;
          token = strtok(NULL, ",");
        }

        if (idx < 4) return;

        const char* target = v[1];
        const char* cmd_id = v[2];
        const char* action = v[3];

        if (strcmp(target, NODE_ID) == 0) {
          last_trigger_soil = soil_now;

          // digitalWrite(PIN_RELAY, pumpStatus ? LOW : HIGH);

          bool success = true;
          String errorCode = "No";
          String message = "OK";

          float flow = default_flow;
          float amp  = default_amp;

          if (strcmp(action, "ON") == 0) {
            strcpy(mode, "CLD");
            pumpStatus = true;
          }
          else if (strcmp(action, "OFF") == 0) {
            strcpy(mode, "CLD");
            pumpStatus = false;
          }
          else if (strcmp(action, "AUTO") == 0) {
            strcpy(mode, "SEN");
          }
          else {
            success = false;
            errorCode = "INVALID_CMD";
            message = "Unknown action";
          }

          digitalWrite(PIN_RELAY, pumpStatus ? LOW : HIGH);

          unsigned long executedAt = millis();

          char ack[128];

          snprintf(ack, sizeof(ack),
          "ACK,%s,%s,%d,%s,%s,%s,%s,%.1f,%.1f,%.1f,%lu",
          NODE_ID,
          cmd_id,
          success ? 1 : 0,
          errorCode.c_str(),
          message.c_str(),
          pumpStatus ? "ON":"OFF",
          mode,
          flow,
          amp,
          last_trigger_soil,
          executedAt
          );

          Serial.print("[LORA][TX ACK] ");
          Serial.println(ack);
          sendUplink(ack);
          lastSend = millis();
          delay(150);

          // cmdProcessing = true;
          // cmdLockUntil = millis() + CMD_LOCK_TIME;

          cmdProcessing = false;

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

  if (strcmp(mode, "SEN") == 0) {
    if (cachedSoil < 30 && !pumpStatus) {
      pumpStatus = true;
      last_trigger_soil = cachedSoil;
    } else if (cachedSoil > 70 && pumpStatus) {
      pumpStatus = false;
      last_trigger_soil = cachedSoil;
    }
  }

  digitalWrite(PIN_RELAY, pumpStatus ? LOW : HIGH);

  if (strcmp(mode, "SEN") != 0 &&
      (
        (lastGatewaySeen == 0 && millis() - bootTime > HEARTBEAT_TIMEOUT) ||
        (lastGatewaySeen > 0 && millis() - lastGatewaySeen > HEARTBEAT_TIMEOUT)
      )
  ) {
    strcpy(mode, "SEN");
  }

  if (!cmdProcessing && millis() - lastSend > SEND_INTERVAL && loraReady) {
    lastSend = millis();
    unsigned long uptimeSec = (millis() - bootTime) / 1000;
    char payload[128];

    snprintf(payload, sizeof(payload),
    "%s,%.1f,%.1f,%d,%d,%d,%s,%lu,%.1f,%.1f,%.1f",
    NODE_ID,
    ema_t,
    ema_h,
    cachedSoil,
    (int)ema_l,
    pumpStatus ? 1 : 0,
    mode,
    uptimeSec,
    default_amp,
    default_flow,
    last_trigger_soil
    );
                     
    Serial.print("[LORA][UPLINK] ");
    Serial.println(payload);
    sendUplink(payload);
  }
}