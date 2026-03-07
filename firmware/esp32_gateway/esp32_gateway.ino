#include <LoRa.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <map>
#include <set>
#include "time.h"

#define LORA_SS    5
#define LORA_RST   14
#define LORA_DIO0  26

#define LORA_RETRY_MAX 5
#define NODE_TIMEOUT 15000

const char* WIFI_SSID = "Shindo";
const char* WIFI_PASS = "vinh1230987";
const char* MQTT_SRV  = "192.168.0.105";
const char* API_URL   = "http://192.168.0.105:5000/api/batch";

WiFiClient espClient;
PubSubClient mqtt(espClient);

bool loraReady = false;
unsigned long lastLoRaRetry = 0;

#define MAX_BUF 50
String buffer[MAX_BUF];
int head = 0, count = 0;  
SemaphoreHandle_t mutex;

bool waitingAck = false;
unsigned long ackTimeout = 0;
char lastCmdId[32] = "";  

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 3600;  // GMT+7 (VN)
const int   daylightOffset_sec = 0;

bool ntpReady = false;

struct NodeState {
  unsigned long lastSeen;
  int rssi;
  bool online;
};

std::map<String, NodeState> nodeRegistry;

const char* whitelist[] = {
  "NODE_01",
  "NODE_02",
  "NODE_03"
};

#define CMD_QUEUE_SIZE 10

struct CmdItem {
  char node[16];
  char cmd[16];
  char cid[32];
};

QueueHandle_t cmdQueue;
SemaphoreHandle_t loraMutex;

/* ================= HEARTBEAT STATE (NEW) ================= */
unsigned long lastHeartbeatSent = 0;
const unsigned long HEARTBEAT_INTERVAL = 25000;

String getTimeISO() {
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo, 2000)) {
    return "";
  }

  char buffer[27];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S+07:00", &timeinfo);
  return String(buffer);
}

bool isWhitelisted(const char* node) {
  for (int i = 0; i < 3; i++) {
    if (strcmp(node, whitelist[i]) == 0) return true;
  }
  return false;
}

void initLoRa() {
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  for (int i = 1; i <= LORA_RETRY_MAX; i++) {
    Serial.printf("[LORA] Init attempt %d/%d...\n", i, LORA_RETRY_MAX);

    if (xSemaphoreTake(loraMutex, portMAX_DELAY)) {

      if (LoRa.begin(433E6)) {

        // ===== CẤU HÌNH RADIO =====
        LoRa.setSyncWord(0xA5);

        LoRa.setSpreadingFactor(9);      // SF7 nhanh nhưng dễ rớt ACK
                                            // SF9 cân bằng ổn định
        LoRa.setSignalBandwidth(125E3);  // 125kHz chuẩn
        LoRa.setCodingRate4(5);          // 4/5 (mặc định nhưng set lại cho chắc)
        LoRa.setTxPower(17);             // SX1278 max ~17dBm
        LoRa.setPreambleLength(8);
        LoRa.enableCrc();

        LoRa.receive();

        loraReady = true;
        Serial.println("[LORA] Initialized OK (Configured)");

        xSemaphoreGive(loraMutex);
        return;
      }

      xSemaphoreGive(loraMutex);
    }

    delay(1000);
  }

  Serial.println("[LORA][FAIL] Running without LoRa (will retry later)");
  loraReady = false;
}

void mqttReconnect() {
  static unsigned long lastTry = 0;

  if (mqtt.connected()) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastTry < 5000) return;

  lastTry = millis();
  Serial.print("[MQTT] Connecting...");

  bool ok = mqtt.connect("GARDEN_GATEWAY", nullptr, nullptr, nullptr, 0, false, nullptr, false);
  if (ok) {
    mqtt.subscribe("garden/control/+/cmd");
    Serial.println(" OK");
  } else {
    Serial.printf(" FAIL rc=%d\n", mqtt.state());
  }
}

void wifiReconnect() {

  static unsigned long lastTry = 0;

  if (WiFi.status() == WL_CONNECTED) return;

  if (millis() - lastTry < 5000) return;

  lastTry = millis();

  Serial.println("[WIFI] Reconnecting...");

  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void mqttCallback(char* topic, byte* payload, unsigned int len) {
  Serial.printf("[MQTT][RX CMD] topic=%s\n", topic);
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, payload, len) != DeserializationError::Ok) {
    Serial.println("[MQTT][CMD] JSON parse fail");
    return;
  }

  CmdItem item;
  strncpy(item.node, doc["node_id"], sizeof(item.node));
  strncpy(item.cmd,  doc["action"], sizeof(item.cmd));
  strncpy(item.cid,  doc["cmd_id"], sizeof(item.cid));

  Serial.printf("[MQTT][CMD DATA] node=%s cmd=%s cid=%s\n",
                item.node, item.cmd, item.cid);

  if (!isWhitelisted(item.node)) {
    Serial.println("[SECURITY] CMD rejected");
    return;
  }

  if (xQueueSend(cmdQueue, &item, pdMS_TO_TICKS(50)) != pdPASS) {
    lastHeartbeatSent = millis();
    Serial.println("[CMD QUEUE] FULL - drop CMD");
  } else {
    Serial.println("[CMD QUEUE] Enqueued");
  }
}

void cmdTask(void* p) {
  CmdItem item;

  for (;;) {
    if (xQueueReceive(cmdQueue, &item, portMAX_DELAY) == pdTRUE) {
      if (!loraReady) {
        Serial.println("[CMD TASK] LoRa not ready - skip");
        continue;
      }

      // Không gửi nếu đang chờ ACK
      if (waitingAck) {
          Serial.println("[CMD TASK] Waiting ACK - skip new CMD");
          continue;  // BẮT BUỘC
      }

      char loraCmd[128];

      snprintf(loraCmd, sizeof(loraCmd),
      "CMD,%s,%s,%s",
      item.node,
      item.cid,
      item.cmd);

      if (xSemaphoreTake(loraMutex, portMAX_DELAY)) {

        LoRa.idle();
        delay(2);

        LoRa.beginPacket();
        LoRa.print(loraCmd);
        LoRa.endPacket(true);
        LoRa.receive();

        xSemaphoreGive(loraMutex);
      }

      // KÍCH HOẠT CHỜ ACK
      waitingAck = true;
      ackTimeout = millis() + 8000;
      strncpy(lastCmdId, item.cid, sizeof(lastCmdId));

      lastHeartbeatSent = millis();
      Serial.print("[CMD→LORA] ");
      Serial.println(loraCmd);
    }
  }
}

void core0Task(void* p) {
  mqtt.setServer(MQTT_SRV, 1883);
  mqtt.setCallback(mqttCallback);

  Serial.println("[CORE0] Real-time task started");

  for (;;) {
    static unsigned long lastNtpTry = 0;
    if (WiFi.status() == WL_CONNECTED && !ntpReady &&
        millis() - lastNtpTry > 10000) {

        lastNtpTry = millis();

        Serial.println("[NTP] Retry sync");

        configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

        struct tm timeinfo;

        if (getLocalTime(&timeinfo, 3000)) {
            ntpReady = true;
            Serial.println("[NTP] Sync OK");
        } else {
            Serial.println("[NTP] Sync fail");
        }
    }
    /* ======================================================
     * P0 – LoRa RX (ƯU TIÊN CAO NHẤT)
     * ====================================================== */
    if (loraReady && xSemaphoreTake(loraMutex, pdMS_TO_TICKS(10))) {

      int packetSize = LoRa.parsePacket();
      if (packetSize) {
        int rssi = LoRa.packetRssi();
        Serial.printf("[LORA][RX RAW] RSSI=%d LEN=%d\n", rssi, packetSize);

        char buf[256];
        int i = 0;
        while (LoRa.available() && i < 255) {
          buf[i++] = (char)LoRa.read();
        }
        buf[i] = '\0';
        // String raw = buf;

        xSemaphoreGive(loraMutex);

        /* ---------- ACK ---------- */
        if (strncmp(buf, "ACK", 3) == 0) {
          char* v[12];
          // int idx = 0, start = 0;
          int idx = 0;
          char* token = strtok(buf, ",");

          while (token != NULL && idx < 12) {
              v[idx++] = token;
              token = strtok(NULL, ",");
          }

          if (idx < 8) {
            Serial.println("[ACK ERROR] malformed");
            continue;
          }

          StaticJsonDocument<512> ack;

          ack["type"]        = "ACK";
          ack["node_id"]     = v[1];
          ack["cmd_id"]      = v[2];
          if (waitingAck) {
              if (strcmp(v[2], lastCmdId) == 0) {
                  waitingAck = false;
                  Serial.println("[ACK MATCHED] OK");
              } else {
                  Serial.println("[ACK MISMATCH]");
              }
          }
          ack["success"]     = (strcmp(v[3], "1") == 0);
          ack["error_code"]  = v[4];
          ack["message"]     = v[5];
          ack["pump"]        = v[6];
          ack["mode"]        = v[7];
          ack["flow"] = (idx > 8) ? atof(v[8]) : 0;
          ack["amp"] = (idx > 9) ? atof(v[9]) : 0;
          ack["last_soil"] = (idx > 10) ? atof(v[10]) : 0;
          ack["executed_at"] = (idx > 11) ? v[11] : "0";
          ack["gateway_time"] = getTimeISO();
          ack["rssi"]        = LoRa.packetRssi();
          ack["gateway_id"]  = "ESP32_GATEWAY_01";

          char out[512];
          serializeJson(ack, out);

          if (mqtt.connected()) {
              bool ok = mqtt.publish("garden/control/ack", out);
              Serial.println(ok ? "[MQTT][TX ACK] OK"
                                : "[MQTT][TX ACK] FAIL");
          } else {
              Serial.println("[MQTT] Not connected - ACK dropped");
          }

          // Serial.println(ok ? "[MQTT][TX ACK] OK"
          //                   : "[MQTT][TX ACK] FAIL");

          continue;
        }

        /* ---------- SENSOR ---------- */
        char* v[12];
        // int idx = 0, start = 0;
        int idx = 0;
        char* token = strtok(buf, ",");

        while (token != NULL && idx < 12) {
            v[idx++] = token;
            token = strtok(NULL, ",");
        }

        if (idx < 11) {
          Serial.println("[ERROR] SENSOR malformed");
          continue;
        }

        const char* node = v[0];
        if (!isWhitelisted(node)) {
          Serial.println("[SECURITY] SENSOR rejected (not in whitelist)");
          continue;
        }

        bool existed = nodeRegistry.count(node);
        bool wasOffline = existed && !nodeRegistry[node].online;

        nodeRegistry[node].lastSeen = millis();
        nodeRegistry[node].rssi = rssi;
        nodeRegistry[node].online = true;

        if (!existed || wasOffline) {
          Serial.print("[NODE ONLINE] ");
          Serial.println(node);
        }

        StaticJsonDocument<512> status;
        status["type"] = "STATUS";
        status["node_id"] = node;
        status["pump"] = (strcmp(v[5], "1") == 0) ? "ON" : "OFF";
        status["mode"] = v[6];
        status["rssi"] = rssi;
        status["uptime"] = atoi(v[7]);
        status["amp"] = atof(v[8]);
        status["flow"] = atof(v[9]);
        status["last_soil"] = atof(v[10]);
        status["current_status"] = "ONLINE";
        String t = getTimeISO();
        status["gateway_time"] = t.length() ? t : "1970-01-01T00:00:00";

        char msg[512];
        serializeJson(status, msg);
        bool ok = mqtt.publish("garden/status", msg, false);
        if (ok) {
            Serial.print("[MQTT][TX STATUS] ");
            Serial.println(msg);
        } else {
            Serial.println("[MQTT][TX STATUS] FAIL");
        }

        xSemaphoreTake(mutex, portMAX_DELAY);
        String measuredAt = getTimeISO();
        char line[128];

        snprintf(line, sizeof(line),
        "%s,%s,%s,%s,%s,%s",
        v[0], v[1], v[2], v[3], v[4], measuredAt.c_str());

        buffer[head] = line;
        head = (head + 1) % MAX_BUF;
        if (count < MAX_BUF) count++;
        xSemaphoreGive(mutex);

        Serial.printf("[BUFFER] Stored (%d/%d)\n", count, MAX_BUF);

        continue; // RX xong → quay lại ngay
      }

      xSemaphoreGive(loraMutex);
    }

    /* ======================================================
     * P1 – MQTT (CHỈ CHẠY KHI KHÔNG CÓ RX)
     * ====================================================== */
    wifiReconnect();
    mqttReconnect();
    mqtt.loop();

    /* ======================================================
     * P2 – HEARTBEAT (ƯU TIÊN THẤP NHẤT)
     * ====================================================== */
    if (loraReady &&
      !waitingAck &&
      millis() - lastHeartbeatSent > HEARTBEAT_INTERVAL &&
      xSemaphoreTake(loraMutex, 0)) {

      LoRa.idle();
      delay(2);
      LoRa.beginPacket();
      LoRa.print("HB,GW_01");
      LoRa.endPacket();
      // delay(5);
      LoRa.receive();

      lastHeartbeatSent = millis();
      xSemaphoreGive(loraMutex);
    }

    /* ======================================================
     * LoRa retry init (background)
     * ====================================================== */
    if (!loraReady && millis() - lastLoRaRetry > 10000) {
      Serial.println("[LORA] Retry init...");
      lastLoRaRetry = millis();
      initLoRa();
    }
    // ACK TIMEOUT CHECK
    if (waitingAck && millis() > ackTimeout) {
      Serial.println("[ACK] Timeout - clear waiting");
      waitingAck = false;
    }
    vTaskDelay(1 / portTICK_PERIOD_MS); // yield CPU
  }
}

void core1Task(void* p) {
  Serial.println("[CORE1] Background task started");
  static uint32_t batch_id = 0;

  for (;;) {
    if (count >= 10 && WiFi.status() == WL_CONNECTED) {
      // if (!ntpReady) {
      //     Serial.println("[TIME] NTP not ready - skip batch");
      //     vTaskDelay(2000 / portTICK_PERIOD_MS);
      //     continue;
      // }
      String nowISO = getTimeISO();

      if (nowISO == "") {
          nowISO = "1970-01-01T00:00:00";
      }
      xSemaphoreTake(mutex, portMAX_DELAY);
      int tail = (head - count + MAX_BUF) % MAX_BUF;

      DynamicJsonDocument doc(4096);
      doc["gateway_id"] = "ESP32_GATEWAY_01";
      doc["batch_id"] = ++batch_id;
      doc["sent_at"] = nowISO.length() ? nowISO : "1970-01-01T00:00:00";

      JsonArray arr = doc.createNestedArray("measurements");

      for (int i = 0; i < 10; i++) {
        char id[16];
        float t, h, s, l;
        char measuredAt[32];

        sscanf(buffer[(tail + i) % MAX_BUF].c_str(),
          "%15[^,],%f,%f,%f,%f,%31[^,\n]",
          id, &t, &h, &s, &l, measuredAt);

        JsonObject o = arr.createNestedObject();
        o["node_id"] = id;
        o["temp"] = t;
        o["humi"] = h;
        o["soil"] = s;
        o["light"] = l;
        o["measured_at"] = measuredAt;
      }
      
      String out;
      serializeJson(doc, out);
      xSemaphoreGive(mutex);

      Serial.println("[HTTP] POST attempt:");
      Serial.println(out);

      HTTPClient http;
      http.setTimeout(5000);
      http.begin(API_URL);
      http.addHeader("Content-Type", "application/json");
      int code = http.POST(out);
      http.end();

      if (code >= 200 && code < 300) {
        xSemaphoreTake(mutex, portMAX_DELAY);
        count -= 10;
        xSemaphoreGive(mutex);
        Serial.println("[HTTP] SUCCESS - batch committed");
      } else {
        Serial.printf("[HTTP] FAIL (code=%d) - retry later\n", code);
      }
    }

    unsigned long now = millis();
    for (auto &it : nodeRegistry) {
      if (it.second.online && now - it.second.lastSeen > NODE_TIMEOUT) {
        it.second.online = false;

        StaticJsonDocument<256> off;
        off["type"] = "STATUS"; 
        off["node_id"] = it.first;
        off["current_status"] = "OFFLINE";
        off["rssi"] = it.second.rssi;
        off["uptime"] = 0;
        off["gateway_time"] = getTimeISO();

        String msg;
        serializeJson(off, msg);
        if (mqtt.connected()) {
            mqtt.publish("garden/status", msg.c_str(), false);
        }

        Serial.println("[NODE OFFLINE] " + it.first);
      }
    }

    vTaskDelay(5000 / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
  mqtt.setBufferSize(1024);

  Serial.println("\n=== ESP32 GARDEN GATEWAY ===");

  mutex = xSemaphoreCreateMutex();
  loraMutex = xSemaphoreCreateMutex();
  if (!loraMutex) {
    Serial.println("[FATAL] LoRa mutex create failed");
    esp_restart();
  }

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WIFI] Connecting");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");

      if (millis() - start > 20000) {
          Serial.println("\n[WIFI] TIMEOUT");
          break;
      }
  }
  if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n[WIFI] OK");
      Serial.println(WiFi.localIP());
  } else {
      Serial.println("[WIFI] FAIL");
  }
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  if (WiFi.status() == WL_CONNECTED) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    Serial.print("[NTP] Waiting sync");

    for (int i = 0; i < 10; i++) {
        struct tm timeinfo;
        if (getLocalTime(&timeinfo)) {
            ntpReady = true;
            Serial.println(" OK");
            break;
        }
        Serial.print(".");
        delay(1000);
    }

    if (!ntpReady) {
        Serial.println(" FAIL");
    }
  }

  initLoRa();

  cmdQueue = xQueueCreate(CMD_QUEUE_SIZE, sizeof(CmdItem));
  if (!cmdQueue) {
    Serial.println("[FATAL] CMD Queue create failed");
    esp_restart();
  }

  xTaskCreatePinnedToCore(cmdTask, "CMD", 4096, NULL, 6, NULL, 0);
  xTaskCreatePinnedToCore(core0Task, "CORE0", 8192, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(core1Task, "CORE1", 8192, NULL, 1, NULL, 1);
}

void loop() {
  vTaskDelete(NULL);
}
