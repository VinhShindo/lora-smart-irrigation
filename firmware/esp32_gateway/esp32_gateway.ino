#include <LoRa.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <map>
#include <set>

#define LORA_SS    5
#define LORA_RST   14
#define LORA_DIO0  26

#define LORA_RETRY_MAX 5
#define NODE_TIMEOUT 15000

const char* WIFI_SSID = "Shindo";
const char* WIFI_PASS = "vinh1230987";
const char* MQTT_SRV  = "192.168.0.101";
const char* API_URL   = "http://192.168.0.101:5000/api/batch";

WiFiClient espClient;
PubSubClient mqtt(espClient);

bool loraReady = false;
unsigned long lastLoRaRetry = 0;

#define MAX_BUF 50
String buffer[MAX_BUF];
int head = 0, count = 0;
SemaphoreHandle_t mutex;

struct NodeState {
  unsigned long lastSeen;
  int rssi;
  bool online;
};

std::map<String, NodeState> nodeRegistry;

std::set<String> whitelist = {
  "NODE_01",
  "NODE_02",
  "NODE_03"
};

#define CMD_QUEUE_SIZE 10

struct CmdItem {
  String node;
  String cmd;
  String cid;
};

QueueHandle_t cmdQueue;
SemaphoreHandle_t loraMutex;

/* ================= HEARTBEAT STATE (NEW) ================= */
unsigned long lastHeartbeatSent = 0;
const unsigned long HEARTBEAT_INTERVAL = 25000;

void initLoRa() {
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  for (int i = 1; i <= LORA_RETRY_MAX; i++) {
    Serial.printf("[LORA] Init attempt %d/%d...\n", i, LORA_RETRY_MAX);

    if (xSemaphoreTake(loraMutex, portMAX_DELAY)) {
      if (LoRa.begin(433E6)) {
        LoRa.setSyncWord(0xA5);
        LoRa.receive();
        loraReady = true;
        Serial.println("[LORA] Initialized OK");
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


void mqttCallback(char* topic, byte* payload, unsigned int len) {
  Serial.printf("[MQTT][RX CMD] topic=%s\n", topic);
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, payload, len) != DeserializationError::Ok) {
    Serial.println("[MQTT][CMD] JSON parse fail");
    return;
  }

  CmdItem item;
  item.node = doc["node_id"].as<String>();
  item.cmd  = doc["action"].as<String>();
  item.cid  = doc["cmd_id"].as<String>();

  Serial.printf("[MQTT][CMD DATA] node=%s cmd=%s cid=%s\n",
                item.node.c_str(), item.cmd.c_str(), item.cid.c_str());

  if (!whitelist.count(item.node)) {
    Serial.println("[SECURITY] CMD rejected");
    return;
  }

  if (xQueueSend(cmdQueue, &item, 0) != pdPASS) {
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

      String loraCmd = "CMD," + item.node + "," + item.cid + "," + item.cmd;

      if (xSemaphoreTake(loraMutex, portMAX_DELAY)) {
        LoRa.idle();
        delay(2);
        LoRa.beginPacket();
        LoRa.print(loraCmd);
        LoRa.endPacket();
        delay(5);
        LoRa.receive();
        xSemaphoreGive(loraMutex);
      }

      lastHeartbeatSent = millis();
      Serial.println("[CMD→LORA] " + loraCmd);
    }
  }
}

void core0Task(void* p) {
  mqtt.setServer(MQTT_SRV, 1883);
  mqtt.setCallback(mqttCallback);

  Serial.println("[CORE0] Real-time task started");

  for (;;) {

    /* ======================================================
     * P0 – LoRa RX (ƯU TIÊN CAO NHẤT)
     * ====================================================== */
    if (loraReady && xSemaphoreTake(loraMutex, 0)) {

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
        String raw = buf;

        xSemaphoreGive(loraMutex);

        /* ---------- ACK ---------- */
        if (raw.startsWith("ACK")) {
          Serial.println("[LORA][ACK RX] " + raw);

          String v[6];
          int idx = 0, start = 0;
          for (int i = 0; i < raw.length() && idx < 5; i++) {
            if (raw[i] == ',') {
              v[idx++] = raw.substring(start, i);
              start = i + 1;
            }
          }
          v[idx] = raw.substring(start);

          StaticJsonDocument<256> ack;
          ack["node_id"]   = v[1];
          ack["cmd_id"]    = v[2];
          ack["pump"]      = v[3];
          ack["mode"]      = v[4];
          ack["last_soil"] = v[5].toFloat();

          String out;
          serializeJson(ack, out);
          bool ok = mqtt.publish("garden/control/ack", out.c_str());
          Serial.println(ok ? "[MQTT][TX ACK] OK" : "[MQTT][TX ACK] FAIL");

          continue; // quay lại vòng lặp, không làm việc khác
        }

        /* ---------- SENSOR ---------- */
        String v[12];
        int idx = 0, start = 0;
        for (int i = 0; i < raw.length() && idx < 11; i++) {
          if (raw[i] == ',') {
            v[idx++] = raw.substring(start, i);
            start = i + 1;
          }
        }
        v[idx] = raw.substring(start);

        if (idx < 11) {
          Serial.println("[ERROR] SENSOR malformed");
          continue;
        }

        String node = v[0];
        if (!whitelist.count(node)) {
          Serial.println("[SECURITY] SENSOR rejected (not in whitelist)");
          continue;
        }

        bool existed = nodeRegistry.count(node);
        bool wasOffline = existed && !nodeRegistry[node].online;

        nodeRegistry[node].lastSeen = millis();
        nodeRegistry[node].rssi = rssi;
        nodeRegistry[node].online = true;

        if (!existed || wasOffline) {
          Serial.println("[NODE ONLINE] " + node);
        }

        StaticJsonDocument<512> status;
        status["node_id"] = node;
        status["pump"] = (v[5] == "1") ? "ON" : "OFF";
        status["mode"] = v[6];
        status["rssi"] = rssi;
        status["amp"] = v[9].toFloat();
        status["flow"] = v[10].toFloat();
        status["last_soil"] = v[11].toFloat();
        status["current_status"] = "ONLINE";

        String msg;
        serializeJson(status, msg);
        bool ok = mqtt.publish("garden/status", msg.c_str());
        Serial.println(ok ? "[MQTT][TX STATUS] " + msg
                          : "[MQTT][TX STATUS] FAIL");

        xSemaphoreTake(mutex, portMAX_DELAY);
        buffer[head] = v[0] + "," + v[1] + "," + v[2] + "," + v[3] + "," + v[4];
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
    mqttReconnect();
    mqtt.loop();

    /* ======================================================
     * P2 – HEARTBEAT (ƯU TIÊN THẤP NHẤT)
     * ====================================================== */
    if (loraReady &&
        millis() - lastHeartbeatSent > HEARTBEAT_INTERVAL &&
        xSemaphoreTake(loraMutex, 0)) {

      LoRa.idle();
      delay(2);
      LoRa.beginPacket();
      LoRa.print("HB,GW_01");
      LoRa.endPacket();
      delay(5);
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

    vTaskDelay(1 / portTICK_PERIOD_MS); // yield CPU
  }
}

void core1Task(void* p) {
  Serial.println("[CORE1] Background task started");
  static uint32_t batch_id = 0;

  for (;;) {
    if (count >= 10 && WiFi.status() == WL_CONNECTED) {
      xSemaphoreTake(mutex, portMAX_DELAY);
      int tail = (head - count + MAX_BUF) % MAX_BUF;

      DynamicJsonDocument doc(4096);
      doc["gateway_id"] = "ESP32_GATEWAY_01";
      doc["batch_id"] = ++batch_id;
      doc["sent_at"] = millis();

      JsonArray arr = doc.createNestedArray("measurements");

      for (int i = 0; i < 10; i++) {
        char id[16];
        float t, h, s, l;
        sscanf(buffer[(tail + i) % MAX_BUF].c_str(),
               "%[^,],%f,%f,%f,%f", id, &t, &h, &s, &l);

        JsonObject o = arr.createNestedObject();
        o["node_id"] = id;
        o["temp"] = t;
        o["humi"] = h;
        o["soil"] = s;
        o["light"] = l;
      }

      String out;
      serializeJson(doc, out);
      xSemaphoreGive(mutex);

      Serial.println("[HTTP] POST attempt:\n" + out);

      HTTPClient http;
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
        off["node_id"] = it.first;
        off["current_status"] = "OFFLINE";
        off["rssi"] = it.second.rssi;

        String msg;
        serializeJson(off, msg);
        mqtt.publish("garden/status", msg.c_str());

        Serial.println("[NODE OFFLINE] " + it.first);
      }
    }

    vTaskDelay(5000 / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n=== ESP32 GARDEN GATEWAY ===");

  mutex = xSemaphoreCreateMutex();
  loraMutex = xSemaphoreCreateMutex();
  if (!loraMutex) {
    Serial.println("[FATAL] LoRa mutex create failed");
    esp_restart();
  }

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) delay(200);
  Serial.println(WiFi.status() == WL_CONNECTED ? "[WIFI] OK" : "[WIFI] FAIL");

  initLoRa();

  cmdQueue = xQueueCreate(CMD_QUEUE_SIZE, sizeof(CmdItem));
  if (!cmdQueue) {
    Serial.println("[FATAL] CMD Queue create failed");
    esp_restart();
  }

  xTaskCreatePinnedToCore(cmdTask, "CMD", 4096, NULL, 5, NULL, 0);
  xTaskCreatePinnedToCore(core0Task, "CORE0", 8192, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(core1Task, "CORE1", 8192, NULL, 1, NULL, 1);
}

void loop() {
  vTaskDelete(NULL);
}
