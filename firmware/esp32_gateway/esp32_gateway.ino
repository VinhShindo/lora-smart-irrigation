#include <LoRa.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <map>
#include <set>
#include "time.h"

/* =========================================================
   LORA PIN CONFIG
   ========================================================= */
#define LORA_SS 5
#define LORA_RST 14
#define LORA_DIO0 26

#define LORA_RETRY_MAX 5
#define NODE_TIMEOUT 30000

/* =========================================================
   NETWORK CONFIG
   ========================================================= */
const char* WIFI_SSID = "Shindo";
const char* WIFI_PASS = "vinh1230987";
const char* MQTT_SRV = "192.168.0.105";
const char* API_URL = "http://192.168.0.105:5000/api/batch";

WiFiClient espClient;
PubSubClient mqtt(espClient);

/* =========================================================
   LORA STATE
   ========================================================= */
bool loraReady = false;
unsigned long lastLoRaRetry = 0;

/* =========================================================
   SENSOR BUFFER
   ========================================================= */
#define MAX_BUF 50

String buffer[MAX_BUF];
int head = 0;
int count = 0;

SemaphoreHandle_t mutex;

/* =========================================================
   ACK STATE
   ========================================================= */
bool waitingAck = false;
unsigned long ackTimeout = 0;
char lastCmdId[32] = "";

/* =========================================================
   NTP CONFIG
   ========================================================= */
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 7 * 3600;
const int daylightOffset_sec = 0;

bool ntpReady = false;

/* =========================================================
   MESH STATE
   ========================================================= */
String meshLeader = "";
std::set<String> seenPackets;

/* =========================================================
   NODE REGISTRY
   ========================================================= */
struct NodeState {
  unsigned long lastSeen;
  int rssi;
  bool online;
};

std::map<String, NodeState> nodeRegistry;

/* =========================================================
   SECURITY
   ========================================================= */
const char* whitelist[] = {
  "NODE_01",
  "NODE_02",
  "NODE_03"
};

/* =========================================================
   COMMAND QUEUE
   ========================================================= */
#define CMD_QUEUE_SIZE 10

struct CmdItem {
  char node[16];
  char cmd[16];
  char cid[32];
};

QueueHandle_t cmdQueue;
SemaphoreHandle_t loraMutex;

/* =========================================================
   HEARTBEAT STATE
   ========================================================= */
unsigned long lastHeartbeatSent = 0;
const unsigned long HEARTBEAT_INTERVAL = 25000;


/* =========================================================
   TIME UTIL
   ========================================================= */
String getTimeISO() {
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo, 2000)) {
    return "";
  }

  char buffer[27];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S+07:00", &timeinfo);

  return String(buffer);
}


/* =========================================================
   WHITELIST CHECK
   ========================================================= */
bool isWhitelisted(const char* node) {
  for (int i = 0; i < 3; i++) {
    if (strcmp(node, whitelist[i]) == 0) {
      return true;
    }
  }

  return false;
}


/* =========================================================
   INIT LORA
   ========================================================= */
void initLoRa() {
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  for (int i = 1; i <= LORA_RETRY_MAX; i++) {

    Serial.printf("[LORA] Init attempt %d/%d...\n", i, LORA_RETRY_MAX);

    if (xSemaphoreTake(loraMutex, portMAX_DELAY)) {

      if (LoRa.begin(433E6)) {

        LoRa.setSyncWord(0xA5);
        LoRa.setSpreadingFactor(9);
        LoRa.setSignalBandwidth(125E3);
        LoRa.setCodingRate4(5);
        LoRa.setTxPower(17);
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


/* =========================================================
   MQTT RECONNECT
   ========================================================= */
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


/* =========================================================
   SEND LORA
   ========================================================= */
void sendLoRa(const char* msg) {
  if (!loraReady) return;
  if (xSemaphoreTake(loraMutex, portMAX_DELAY)) {

    LoRa.idle();

    LoRa.beginPacket();
    LoRa.print(msg);
    LoRa.endPacket();

    LoRa.receive();

    xSemaphoreGive(loraMutex);
  }
}


/* =========================================================
   WIFI RECONNECT
   ========================================================= */
void wifiReconnect() {
  static unsigned long lastTry = 0;

  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastTry < 5000) return;

  lastTry = millis();

  Serial.println("[WIFI] Reconnecting...");

  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}


/* =========================================================
   MQTT CALLBACK
   ========================================================= */
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  Serial.printf("[MQTT][RX CMD] topic=%s\n", topic);

  StaticJsonDocument<256> doc;

  if (deserializeJson(doc, payload, len) != DeserializationError::Ok) {
    Serial.println("[MQTT][CMD] JSON parse fail");
    return;
  }

  CmdItem item;

  strncpy(item.node, doc["node_id"], sizeof(item.node) - 1);
  item.node[sizeof(item.node) - 1] = '\0';

  strncpy(item.cmd, doc["action"], sizeof(item.cmd) - 1);
  item.cmd[sizeof(item.cmd) - 1] = '\0';

  strncpy(item.cid, doc["cmd_id"], sizeof(item.cid) - 1);
  item.cid[sizeof(item.cid) - 1] = '\0';

  Serial.printf(
    "[MQTT][CMD DATA] node=%s cmd=%s cid=%s\n",
    item.node,
    item.cmd,
    item.cid);

  if (!isWhitelisted(item.node)) {
    Serial.println("[SECURITY] CMD rejected");
    return;
  }

  if (xQueueSend(cmdQueue, &item, pdMS_TO_TICKS(50)) != pdPASS) {

    Serial.println("[CMD QUEUE] FULL - drop CMD");

  } else {

    Serial.println("[CMD QUEUE] Enqueued");
  }
}


/* =========================================================
   CMD TASK
   ========================================================= */
void cmdTask(void* p) {
  CmdItem item;

  for (;;) {

    if (xQueueReceive(cmdQueue, &item, portMAX_DELAY) == pdTRUE) {

      if (!loraReady) {
        Serial.println("[CMD TASK] LoRa not ready - skip");
        continue;
      }

      if (waitingAck) {
        Serial.println("[CMD TASK] Waiting ACK - skip new CMD");
        continue;
      }

      char loraCmd[128];

      snprintf(
        loraCmd,
        sizeof(loraCmd),
        "C,GW,%s,3,%s,%s",
        item.node,
        item.cid,
        item.cmd);

      if (xSemaphoreTake(loraMutex, portMAX_DELAY)) {

        Serial.println("[CMD] Preempt radio");

        LoRa.idle();

        delay(5);

        while (LoRa.available()) LoRa.read();

        delay(5);

        if (LoRa.parsePacket()) {

          Serial.println("[CMD] Channel busy");

          xSemaphoreGive(loraMutex);

          vTaskDelay(50 / portTICK_PERIOD_MS);

          continue;
        }

        LoRa.beginPacket();
        LoRa.print(loraCmd);
        LoRa.endPacket();

        LoRa.receive();

        lastHeartbeatSent = millis();

        xSemaphoreGive(loraMutex);
      }

      waitingAck = true;
      ackTimeout = millis() + 8000;

      strncpy(lastCmdId, item.cid, sizeof(lastCmdId));

      Serial.print("[CMD→LORA] ");
      Serial.println(loraCmd);
    }
  }
}


/* =========================================================
   CORE0 TASK
   ========================================================= */
void core0Task(void* p) {
  mqtt.setServer(MQTT_SRV, 1883);
  mqtt.setCallback(mqttCallback);

  Serial.println("[CORE0] Real-time task started");

  for (;;) {

    static unsigned long lastNtpTry = 0;

    if (WiFi.status() == WL_CONNECTED && !ntpReady && millis() - lastNtpTry > 10000) {

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
           P0 – LORA RX
           ====================================================== */

    if (loraReady && xSemaphoreTake(loraMutex, pdMS_TO_TICKS(10))) {

      int packetSize = LoRa.parsePacket();

      if (packetSize) {

        int rssi = LoRa.packetRssi();

        Serial.printf("[LORA][RX RAW] RSSI=%d LEN=%d\n", rssi, packetSize);

        char buf[512];
        int i = 0;

        while (LoRa.available() && i < 511) {
          buf[i++] = (char)LoRa.read();
        }

        buf[i] = '\0';

        String hash = String(buf);

        if (seenPackets.count(hash)) {
          Serial.println("[DROP DUP]");
          xSemaphoreGive(loraMutex);
          continue;
        }

        seenPackets.insert(hash);

        if (seenPackets.size() > 50) {
          seenPackets.clear();
        }

        xSemaphoreGive(loraMutex);

        Serial.print("[LORA][DATA] ");
        Serial.println(buf);

        /* ======================================================
                   CSV SPLIT
                   ====================================================== */

        char* v[16];
        int idx = 0;

        char* token = strtok(buf, ",");

        while (token && idx < 16) {
          v[idx++] = token;
          token = strtok(NULL, ",");
        }

        if (idx < 2) {
          Serial.println("[LORA] malformed");
          continue;
        }

        char type = v[0][0];

        /* ======================================================
          BATCH SENSOR
        ====================================================== */

        if (strncmp(buf, "BATCHSEN,", 9) == 0) {

          Serial.println("[GW] SENSOR BATCH RX");

          char* record = buf + 9;

          char* entry = strtok(record, ";");

          while (entry) {

            char node[4];
            float t, h;
            int soil, light;

            if (sscanf(entry, "D,%3[^,],%f,%f,%d,%d",
                       node, &t, &h, &soil, &light)
                == 5) {

              String nodeId = "NODE_" + String(node);

              xSemaphoreTake(mutex, portMAX_DELAY);

              String measuredAt = getTimeISO();

              char line[128];

              snprintf(line, sizeof(line),
                       "%s,%.1f,%.1f,%d,%d,%s",
                       nodeId.c_str(),
                       t, h, soil, light,
                       measuredAt.c_str());

              buffer[head] = line;

              head = (head + 1) % MAX_BUF;

              if (count < MAX_BUF) count++;

              xSemaphoreGive(mutex);

              Serial.printf("[BATCH SENSOR] %s stored\n", node);
            }

            entry = strtok(NULL, ";");
          }

          continue;
        }

        /* ======================================================
            BATCH STATUS
          ====================================================== */

        if (strncmp(buf, "BATCHSTA,", 9) == 0) {

          Serial.println("[GW] STATUS BATCH RX");

          char* record = buf + 9;

          char* entry = strtok(record, ";");

          while (entry) {

            char node[4];
            int pump;
            char mode[4];
            int soil;

            if (sscanf(entry,
                       "S,%3[^,],%d,%3[^,],%d",
                       node, &pump, mode, &soil)
                == 4) {

              String nodeId = "NODE_" + String(node);

              StaticJsonDocument<256> status;

              status["type"] = "STATUS";
              status["node_id"] = nodeId;
              status["pump"] = pump ? "ON" : "OFF";
              status["mode"] = mode;
              status["last_soil"] = soil;
              status["rssi"] = 0;
              status["current_status"] = "ONLINE";
              status["gateway_time"] = getTimeISO();

              char msg[256];
              serializeJson(status, msg);

              mqtt.publish("garden/status", msg);
            }

            entry = strtok(NULL, ";");
          }

          continue;
        }

        /* ======================================================
                   HELLO
                   ====================================================== */

        if (type == 'H') {

          char node[4];
          int rssi = LoRa.packetRssi();

          if (sscanf(buf, "H,%3s", node) == 1) {
            Serial.printf("[HELLO] from node=%s RSSI=%d\n", node, rssi);

            char reply[32];
            snprintf(reply, sizeof(reply), "R,%s,%d", node, rssi);

            Serial.print("[HELLO REPLY] ");
            Serial.println(reply);
            sendLoRa(reply);
          } else {
            Serial.println("[HELLO] malformed");
          }
        }

        /* ======================================================
                   ACK
                   ====================================================== */

        if (type == 'A') {

          if (idx < 12) {
            Serial.println("[ACK] malformed");
            continue;
          }

          const char* nodeShort = v[1];

          String nodeId = "NODE_" + String(nodeShort);

          StaticJsonDocument<512> ack;

          ack["type"] = "ACK";
          ack["node_id"] = nodeId;
          ack["cmd_id"] = v[4];

          if (waitingAck) {

            if (strcmp(v[4], lastCmdId) == 0) {

              waitingAck = false;
              ackTimeout = 0;
              lastHeartbeatSent = millis();

              Serial.println("[ACK MATCHED]");

            } else {

              Serial.println("[ACK CMD_ID MISMATCH]");
            }
          }

          ack["success"] = (strcmp(v[5], "1") == 0);

          ack["error_code"] = v[6];
          ack["message"] = v[7];

          ack["pump"] = (strcmp(v[8], "1") == 0) ? "ON" : "OFF";

          if (strcmp(v[9], "S") == 0)
            ack["mode"] = "SEN";
          else if (strcmp(v[9], "C") == 0)
            ack["mode"] = "CLD";
          else if (strcmp(v[9], "R") == 0)
            ack["mode"] = "READY";
          else
            ack["mode"] = "UNKNOWN";

          ack["flow"] = (idx > 10) ? atof(v[10]) / 10.0 : 0;
          ack["amp"] = (idx > 11) ? atof(v[11]) / 10.0 : 0;
          ack["last_soil"] = (idx > 12) ? atoi(v[12]) : 0;
          ack["executed_at"] = (idx > 13) ? v[13] : "0";

          ack["rssi"] = rssi;
          ack["gateway_time"] = getTimeISO();
          ack["gateway_id"] = "ESP32_GATEWAY_01";

          char out[512];

          serializeJson(ack, out);

          if (mqtt.connected()) {

            bool ok = mqtt.publish("garden/control/ack", out);

            Serial.println(ok ? "[MQTT][TX ACK] OK"
                              : "[MQTT][TX ACK] FAIL");
          }

          continue;
        }
        if (type == 'M') {

          char node[4];
          int rssi;

          sscanf(v[1], "%3s", node);
          rssi = atoi(v[2]);

          Serial.printf("[MESH METRIC] node=%s rssi=%d\n", node, rssi);

          continue;
        }
        /* ======================================================
                   STATUS
                   ====================================================== */

        if (type == 'S') {

          if (idx < 8) {
            Serial.println("[STATUS] malformed");
            continue;
          }

          String nodeId = "NODE_" + String(v[1]);

          bool existed = nodeRegistry.count(nodeId);
          bool wasOffline = existed && !nodeRegistry[nodeId].online;

          nodeRegistry[nodeId].lastSeen = millis();
          nodeRegistry[nodeId].rssi = rssi;
          nodeRegistry[nodeId].online = true;

          if (!existed || wasOffline) {

            Serial.print("[NODE ONLINE] ");
            Serial.println(nodeId);
          }

          StaticJsonDocument<512> status;

          status["type"] = "STATUS";
          status["node_id"] = nodeId;

          status["pump"] = (strcmp(v[2], "1") == 0) ? "ON" : "OFF";
          status["mode"] = v[3];

          status["uptime"] = atoi(v[4]);

          status["amp"] = atof(v[5]) / 10.0;
          status["flow"] = atof(v[6]) / 10.0;

          status["last_soil"] = atoi(v[7]);

          status["rssi"] = rssi;
          status["current_status"] = "ONLINE";

          status["gateway_time"] = getTimeISO();

          char msg[512];

          serializeJson(status, msg);

          bool ok = mqtt.publish("garden/status", msg);

          if (ok) {

            Serial.print("[MQTT][TX STATUS] ");
            Serial.println(msg);

          } else {

            Serial.println("[MQTT][TX STATUS] FAIL");
          }

          continue;
        }

        /* ======================================================
                   SENSOR
                   ====================================================== */

        if (type == 'D') {

          if (idx < 8) {
            Serial.println("[SENSOR] malformed");
            continue;
          }

          String nodeId = "NODE_" + String(v[1]);

          Serial.printf("[GW SENSOR] from %s via mesh\n", v[1]);

          xSemaphoreTake(mutex, portMAX_DELAY);

          String measuredAt = getTimeISO();

          char line[128];

          snprintf(
            line,
            sizeof(line),
            "%s,%s,%s,%s,%s,%s",
            nodeId.c_str(),
            v[4],
            v[5],
            v[6],
            v[7],
            measuredAt.c_str());

          buffer[head] = line;

          head = (head + 1) % MAX_BUF;

          if (count < MAX_BUF) count++;

          xSemaphoreGive(mutex);

          Serial.printf("[BUFFER] SENSOR stored (%d/%d)\n", count, MAX_BUF);

          continue;
        }
      }

      xSemaphoreGive(loraMutex);
    }

    wifiReconnect();
    mqttReconnect();
    mqtt.loop();

    /* ======================================================
           HEARTBEAT
           ====================================================== */

    if (loraReady && !waitingAck && millis() - lastHeartbeatSent > HEARTBEAT_INTERVAL && xSemaphoreTake(loraMutex, 0)) {

      LoRa.idle();
      delay(2);

      LoRa.beginPacket();

      if (loraReady) {

        char hb[32];

        snprintf(hb, sizeof(hb), "B,%s", "ALL");

        LoRa.print(hb);
      }

      LoRa.endPacket();

      LoRa.receive();

      lastHeartbeatSent = millis();

      xSemaphoreGive(loraMutex);
    }

    if (!loraReady && millis() - lastLoRaRetry > 10000) {

      Serial.println("[LORA] Retry init...");

      lastLoRaRetry = millis();

      initLoRa();
    }

    if (waitingAck && millis() > ackTimeout) {

      Serial.println("[ACK] Timeout - clear waiting");

      waitingAck = false;
    }

    vTaskDelay(1 / portTICK_PERIOD_MS);
  }
}


/* =========================================================
   CORE1 TASK
   ========================================================= */
void core1Task(void* p) {
  Serial.println("[CORE1] Background task started");

  static uint32_t batch_id = 0;

  for (;;) {

    if (count >= 10 && WiFi.status() == WL_CONNECTED) {

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

        sscanf(
          buffer[(tail + i) % MAX_BUF].c_str(),
          "%15[^,],%f,%f,%f,%f,%31[^,\n]",
          id,
          &t,
          &h,
          &s,
          &l,
          measuredAt);

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

    for (auto& it : nodeRegistry) {

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


/* =========================================================
   SETUP
   ========================================================= */
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


/* =========================================================
   LOOP
   ========================================================= */
void loop() {
  vTaskDelete(NULL);
}