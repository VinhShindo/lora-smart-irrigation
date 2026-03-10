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
const char NODE_SHORT[] = "01";

/* ================= INTERVAL CONFIG ================= */
const unsigned long STATUS_INTERVAL = 5000;   // realtime
const unsigned long SENSOR_INTERVAL = 20000;  // sensor
/* =================================================== */

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

unsigned long lastStatusSend = 0;
unsigned long lastSensorSend = 0;
unsigned long lastAckTime = 0;

bool loraReady = false;

int cachedSoil = 0;
unsigned long bootTime = 0;

unsigned long lastSoilRead = 0;
const unsigned long SOIL_INTERVAL = 2000;

unsigned long txBlockedUntil = 0;

/* ===== COMMAND LOCK ===== */
bool cmdProcessing = false;
unsigned long cmdLockUntil = 0;
const unsigned long CMD_LOCK_TIME = 1500;
/* ======================== */

void initLoRa() {

  LoRa.setPins(PIN_LORA_SS, PIN_LORA_RST, PIN_LORA_DIO0);

  for (int i = 1; i <= LORA_RETRY_MAX; i++) {

    Serial.printf("[LORA] Đang thử kết nối lần %d/%d...\n", i, LORA_RETRY_MAX);

    if (LoRa.begin(433E6)) {

      /* ===== RADIO CONFIG ===== */
      LoRa.setSyncWord(0xA5);
      LoRa.setSpreadingFactor(9);
      LoRa.setSignalBandwidth(125E3);
      LoRa.setCodingRate4(5);
      LoRa.setTxPower(17);
      LoRa.setPreambleLength(8);
      LoRa.enableCrc();
      /* ======================== */

      LoRa.receive();

      loraReady = true;

      Serial.println("[LORA] Khởi tạo THÀNH CÔNG");

      return;
    }

    delay(1000);
  }

  Serial.println("[LORA][LỖI] Không tìm thấy module");
  loraReady = false;
}

/* ================= SENSOR PROCESS ================= */

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

/* ================= LORA SEND ================= */

bool sendUplink(const char* data) {

  if (!loraReady) return false;

  LoRa.idle();

  LoRa.beginPacket();
  LoRa.print(data);
  LoRa.endPacket(true);

  delay(random(80,200));

  LoRa.receive();

  return true;
}

/* ================= SETUP ================= */

void setup() {

  Serial.begin(115200);

  Serial.println("\n[NODE] Đang khởi động...");

  dht.begin();

  pinMode(PIN_RELAY, OUTPUT);

  digitalWrite(PIN_RELAY, LOW);

  Serial.println("[BOOT] Relay: TẮT | READY");

  bootTime = millis();

  initLoRa();
}

/* ================= LOOP ================= */

void loop() {

  /* ===== RX LORA ===== */

  if (loraReady) {

    int packetSize = LoRa.parsePacket();

    if (packetSize) {

      char msg[128];
      int i = 0;

      while (LoRa.available() && i < 127) {

        msg[i++] = (char)LoRa.read();
      }

      msg[i] = '\0';

      Serial.print("[LORA][RX] ");
      Serial.println(msg);

      /* ===== HEARTBEAT ===== */

      if (strncmp(msg, "HB", 2) == 0) {

        lastGatewaySeen = millis();

        Serial.println("[HB] RX");
      }

      /* ===== CMD ===== */

      if (strncmp(msg,"CMD",3)==0) {

        if (cmdProcessing && millis() < cmdLockUntil) {

          Serial.println("[CMD] Locked");

          return;
        }

        cmdProcessing = true;

        cmdLockUntil = millis() + CMD_LOCK_TIME;
        txBlockedUntil = millis() + 3000; 

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

          bool success = true;

          String errorCode = "No";
          String message = "OK";

          if (strcmp(action, "ON") == 0) {

            strcpy(mode, "CLD");
            pumpStatus = true;

          } else if (strcmp(action, "OFF") == 0) {

            strcpy(mode, "CLD");
            pumpStatus = false;

          } else if (strcmp(action, "AUTO") == 0) {

            strcpy(mode, "SEN");

          } else {

            success = false;

            errorCode = "INVALID_CMD";

            message = "Unknown action";
          }

          digitalWrite(PIN_RELAY, pumpStatus ? LOW : HIGH);

          unsigned long executedAt = millis();

          /* ===== OPTIMIZED ACK ===== */
          char modeShort;

          if (strcmp(mode,"SEN")==0) modeShort='S';
          else if (strcmp(mode,"CLD")==0) modeShort='C';
          else modeShort='R';   // READY
          int pumpShort  = pumpStatus ? 1 : 0;

          int flowShort = (int)(default_flow * 10); // 1.2 -> 12
          int ampShort  = (int)(default_amp * 10);  // 0.5 -> 5

          char ack[128];

          snprintf(ack,sizeof(ack),
          "A,%s,%s,%d,%s,%s,%d,%c,%d,%d,%d,%lu",
          NODE_SHORT,            // NODE_01 -> 01
          cmd_id,                // giữ nguyên
          success ? 1 : 0,
          errorCode.c_str(),     // giữ nguyên
          message.c_str(),       // giữ nguyên
          pumpShort,             // 1 / 0
          modeShort,             // S / C
          flowShort,             // flow * 10
          ampShort,              // amp * 10
          (int)last_trigger_soil,
          executedAt
          );

          Serial.print("[LORA][TX ACK] ");
          Serial.println(ack);

          delay(80);
          sendUplink(ack);

          lastAckTime = millis();   // wait before next send

          lastStatusSend = millis();
          lastSensorSend = millis();

          cmdProcessing = false;
txBlockedUntil = millis() + 3000;
        }
      }
    }

  }

  /* ===== AUTO MODE ===== */

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

  /* ===== REALTIME STATUS ===== */

  if (!cmdProcessing &&
  millis() > txBlockedUntil &&
      millis() - lastStatusSend > STATUS_INTERVAL + random(0,1000) &&
      millis() - lastAckTime > STATUS_INTERVAL) {

    lastStatusSend = millis();

    unsigned long uptime = (millis() - bootTime)/1000;

    char payload[80];

    snprintf(payload,sizeof(payload),
    "R,%s,%d,%s,%lu,%d,%d,%.0f",
    NODE_SHORT,
    pumpStatus ? 1:0,
    mode,
    uptime,
    (int)(default_amp*10),
    (int)(default_flow*10),
    last_trigger_soil
    );

    Serial.print("[LORA][STATUS] ");
    Serial.println(payload);

    sendUplink(payload);
  }

  /* ===== SENSOR DATA ===== */

  if (!cmdProcessing &&
  millis() > txBlockedUntil &&
      millis() - lastSensorSend > SENSOR_INTERVAL &&
      millis() - lastAckTime > SENSOR_INTERVAL) {

    lastSensorSend = millis();

    char payload[80];

    snprintf(payload,sizeof(payload),
    "S,%s,%.1f,%.1f,%d,%d",
    NODE_SHORT,
    ema_t,
    ema_h,
    cachedSoil,
    (int)ema_l
    );

    Serial.print("[LORA][SENSOR] ");
    Serial.println(payload);

    sendUplink(payload);
  }

    /* ===== SENSOR UPDATE ===== */

  processSensors();

  if (millis() - lastSoilRead > SOIL_INTERVAL)
  {
    cachedSoil = readSoil();
    lastSoilRead = millis();
  }
}