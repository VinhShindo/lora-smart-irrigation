#include <LoRa.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <map>
#include <set>
#include "time.h"
#include <WebServer.h>
#include <Preferences.h>

// LORA
#define LORA_SS 5
#define LORA_RST 14
#define LORA_DIO0 26
#define LORA_RETRY_MAX 5
#define NODE_TIMEOUT 45000

// NETWORK
const char* WIFI_SSID = "Shindo";
const char* WIFI_PASS = "vinh1230987";
const char* MQTT_SRV = "192.168.0.105";
const char* API_URL = "http://192.168.0.105:5000/api/batch";

WiFiClient espClient;
PubSubClient mqtt(espClient);

// --- BIẾN CẤU HÌNH WIFI MỚI ---
WebServer server(80);
Preferences preferences;

const char* DEFAULT_SSID = "Garden_Gateway_Config";
const char* DEFAULT_PASS = "12345678";

// Biến lưu thông tin WiFi tạm thời từ Preferences
String st_ssid = "";
String st_pass = "";

// HTML cho trang cấu hình
const char html_page[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head><meta name="viewport" content="width=device-width, initial-scale=1"><title>Gateway Config</title></head>
<body>
  <h2>Cấu hình WiFi cho Gateway</h2>
  <form action="/save" method="POST">
    SSID: <br><input type="text" name="ssid"><br>
    Password: <br><input type="password" name="pass"><br><br>
    <input type="submit" value="Lưu và Khởi động lại">
  </form>
</body></html>
)rawliteral";

// LORA STATE
bool loraReady = false;
unsigned long lastLoRaRetry = 0;

// SENSOR BUFFER
#define MAX_BUF 50
char buffer[MAX_BUF][128];
int head = 0, count = 0;
SemaphoreHandle_t mutex;

// ACK STATE
bool waitingAck = false;
unsigned long ackTimeout = 0;
char lastCmdId[32] = "";
uint16_t lastSeqSent = 0;

// NTP
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 7 * 3600;
const int daylightOffset_sec = 0;
bool ntpReady = false;

// NODE REGISTRY
struct NodeState {
  unsigned long lastSeen;
  int rssi;
  bool online;
};
std::map<String, NodeState> nodeRegistry;

// SECURITY
const char* whitelist[] = { "NODE_01", "NODE_02", "NODE_03" };

// CMD QUEUE
#define CMD_QUEUE_SIZE 10
struct CmdItem {
  char node[16];
  char cmd[16];
  char cid[32];
};
QueueHandle_t cmdQueue;
SemaphoreHandle_t loraMutex;

// HEARTBEAT
unsigned long lastHeartbeatSent = 0;
const unsigned long HEARTBEAT_INTERVAL = 25000;

bool apModeActive = false;

void handleSave() {
  String n_ssid = server.arg("ssid");
  String n_pass = server.arg("pass");
  if (n_ssid.length() > 0) {
    preferences.begin("wifi-config", false);
    preferences.putString("ssid", n_ssid);
    preferences.putString("pass", n_pass);
    preferences.end();
    server.send(200, "text/html", "<h1>Da luu! Gateway dang khoi dong lai...</h1>");
    delay(2000);
    apModeActive = false;
    st_ssid = n_ssid;
    st_pass = n_pass;
    ESP.restart();
  } else {
    server.send(400, "text/html", "<h1>SSID khong hop le!</h1>");
  }
}

// Hàm khởi chạy trang Web cấu hình (Chế độ AP)
void startConfigPortal() {
  if (apModeActive) return;
  apModeActive = true;
  mqtt.disconnect();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(DEFAULT_SSID, DEFAULT_PASS);
  Serial.println("[WIFI] AP MODE");
  Serial.println(WiFi.softAPIP());
  server.on("/", []() {
    server.send(200, "text/html", html_page);
  });
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
}

// TIME
String getTimeISO() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 2000)) return "";
  char buf[27];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+07:00", &timeinfo);
  return String(buf);
}

// WHITELIST
bool isWhitelisted(const char* node) {
  for (int i = 0; i < 3; i++)
    if (strcmp(node, whitelist[i]) == 0) return true;
  return false;
}

bool validShortNode(const char* node) {
  if (strcmp(node, "01") == 0) return true;
  if (strcmp(node, "02") == 0) return true;
  if (strcmp(node, "03") == 0) return true;
  return false;
}

// INIT LORA
void initLoRa() {
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  for (int i = 1; i <= LORA_RETRY_MAX; i++) {
    Serial.printf("[LORA] Init attempt %d/%d\n", i, LORA_RETRY_MAX);
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
        Serial.println("[LORA] OK");
        xSemaphoreGive(loraMutex);
        return;
      }
      xSemaphoreGive(loraMutex);
    }
    delay(1000);
  }
  Serial.println("[LORA] FAIL");
  loraReady = false;
}

// WIFI RECONNECT
void wifiReconnect() {
  if (apModeActive) return;
  static unsigned long lastTry = 0;
  static unsigned long firstFail = 0;
  if (st_ssid.length() == 0) return;
  if (WiFi.status() == WL_CONNECTED) {
    if (apModeActive) {
      Serial.println("[WIFI] Connected → disable AP");
      WiFi.softAPdisconnect(true);
      apModeActive = false;
    }
    firstFail = 0;
    return;
  }
  ntpReady = false;
  if (firstFail == 0) firstFail = millis();
  if (millis() - firstFail > 60000) {
    Serial.println("[WIFI] Lost too long → start config portal");
    startConfigPortal();
    return;
  }
  if (millis() - lastTry < 10000) return;
  lastTry = millis();
  WiFi.begin(st_ssid.c_str(), st_pass.c_str());
}

// MQTT RECONNECT
void mqttReconnect() {
  if (apModeActive) return;
  static unsigned long lastTry = 0;
  if (mqtt.connected()) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastTry < 5000) return;
  lastTry = millis();
  Serial.print("[MQTT] Connecting...");
  if (mqtt.connect("GARDEN_GATEWAY")) {
    mqtt.subscribe("garden/control/+/cmd");
    Serial.println("OK");
  } else Serial.printf("FAIL %d\n", mqtt.state());
}

// SEND LORA
void sendLoRa(const char* msg) {
  if (!loraReady) return;
  if (xSemaphoreTake(loraMutex, portMAX_DELAY)) {
    LoRa.idle();
    delay(2);
    LoRa.beginPacket();
    LoRa.print(msg);
    LoRa.endPacket();
    delay(2);
    LoRa.receive();
    xSemaphoreGive(loraMutex);
  }
}

// MQTT CALLBACK
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return;
  CmdItem item;
  strncpy(item.node, doc["node_id"], 15);
  strncpy(item.cmd, doc["action"], 15);
  strncpy(item.cid, doc["cmd_id"], 31);
  Serial.printf(
    "[MQTT][CMD DATA] node=%s cmd=%s cid=%s\n",
    item.node,
    item.cmd,
    item.cid);
  if (!isWhitelisted(item.node)) {
    Serial.println("[SECURITY] reject");
    return;
  }
  if (xQueueSend(cmdQueue, &item, pdMS_TO_TICKS(50)) != pdPASS)
    Serial.println("[CMD QUEUE] FULL");
}

// CHECK PACKAGE FOR GW
bool isPacketForGateway(const char* p) {
  if (strncmp(p, "H,", 2) == 0) return true;
  if (strncmp(p, "BS,", 3) == 0) return true;
  if (strncmp(p, "BT,", 3) == 0) return true;
  if (strncmp(p, "A,", 2) == 0) return true;
  return false;
}

// CMD TASK
void cmdTask(void* p) {
  Serial.println("[CORE] CMD TASK started");

  CmdItem item;
  for (;;) {
    if (xQueueReceive(cmdQueue, &item, portMAX_DELAY) == pdTRUE) {
      if (!loraReady) continue;
      if (waitingAck) {
        if (millis() < ackTimeout) {
          vTaskDelay(50 / portTICK_PERIOD_MS);
          continue;
        } else {
          Serial.println("[ACK] old timeout → allow next CMD");
          waitingAck = false;
        }
      }
      char nodeShort[4];
      sscanf(item.node, "NODE_%3s", nodeShort);
      int action = !strcmp(item.cmd, "ON") ? 1 : !strcmp(item.cmd, "OFF") ? 0
                                                                          : 2;
      uint16_t seq = millis() & 0xFFFF;
      lastSeqSent = seq;
      char loraCmd[128];
      snprintf(loraCmd, sizeof(loraCmd), "C,GW,LD,3,%u,%s,%s,%d", seq, item.cid, nodeShort, action);
      if (xSemaphoreTake(loraMutex, portMAX_DELAY)) {
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
      ackTimeout = millis() + 3500;
      strncpy(lastCmdId, item.cid, sizeof(lastCmdId));
      Serial.print("[CMD→LORA] ");
      Serial.println(loraCmd);
    }
  }
}

void webTask(void* p) {
  for (;;) {
    if (apModeActive) {
      server.handleClient();
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// LORA RX TASK
void loraRxTask(void* p) {
  Serial.println("[CORE] LORA TASK started");

  for (;;) {

    // NTP retry
    static unsigned long lastNtpTry = 0;
    if (WiFi.status() == WL_CONNECTED && !ntpReady && millis() - lastNtpTry > 10000) {
      lastNtpTry = millis();
      Serial.println("[NTP] Retry sync");
      configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
      struct tm timeinfo;
      if (getLocalTime(&timeinfo, 3000)) {
        ntpReady = true;
        Serial.println("[NTP] OK");
      } else {
        Serial.println("[NTP] Sync fail");
      }
    }
    // HEARTBEAT (SMART PRIORITY)
    if (loraReady && millis() - lastHeartbeatSent > HEARTBEAT_INTERVAL) {
      if (xSemaphoreTake(loraMutex, pdMS_TO_TICKS(100))) {
        LoRa.idle();
        delay(3);
        while (LoRa.available()) LoRa.read();
        delay(2);
        LoRa.beginPacket();
        LoRa.print("B,L");
        LoRa.endPacket();
        LoRa.receive();
        Serial.println("[GW][TX] HEARTBEAT");
        lastHeartbeatSent = millis();
        xSemaphoreGive(loraMutex);
      }
    }

    if (loraReady && xSemaphoreTake(loraMutex, pdMS_TO_TICKS(10))) {
      int packetSize = LoRa.parsePacket();
      if (!packetSize) {
        xSemaphoreGive(loraMutex);
        vTaskDelay(2);
        continue;
      }
      int rssi = LoRa.packetRssi();
      char buf[256];
      int i = 0;
      while (LoRa.available() && i < 255) buf[i++] = LoRa.read();
      buf[i] = '\0';
      xSemaphoreGive(loraMutex);
      if (i < 3) continue;
      if (strncmp(buf, "C,", 2) == 0) {
        char type[2], src[8];
        if (sscanf(buf, "%1[^,],%7[^,]", type, src) == 2) {
          if (strcmp(src, "GW") != 0) {
            Serial.println("[MESH] Forward CMD (raw)");
            sendLoRa(buf);
          }
        }
        continue;
      }
      if (!isPacketForGateway(buf)) {
        continue;
      }
      Serial.print("[LORA][RX] ");
      Serial.println(buf);

      // HELLO
      if (strncmp(buf, "H,", 2) == 0) {
        char node[4];
        if (sscanf(buf, "H,%3[^,\r\n]", node) == 1) {
          if (!validShortNode(node)) continue;
          Serial.printf("[HELLO] node=%s RSSI=%d\n", node, rssi);
          char reply[32];
          snprintf(reply, sizeof(reply), "R,%s,%d", node, rssi);
          delay(100);
          sendLoRa(reply);
          char nodeId[12];
          snprintf(nodeId, sizeof(nodeId), "NODE_%s", node);
          nodeRegistry[nodeId].lastSeen = millis();
          nodeRegistry[nodeId].rssi = rssi;
          nodeRegistry[nodeId].online = true;
        }
        continue;
      }

      // SENSOR BATCH
      if (strncmp(buf, "BS,", 3) == 0) {
        Serial.println("[GW] SENSOR BATCH RX");
        char* record = buf + 3;
        char* entry = strtok(record, "|");
        while (entry) {
          char node[4];
          float t, h, soil, light;
          if (sscanf(entry, "%3[^,],%f,%f,%f,%f", node, &t, &h, &soil, &light) == 5) {
            if (!validShortNode(node)) {
              entry = strtok(NULL, ";");
              continue;
            }
            char nodeId[12];
            snprintf(nodeId, sizeof(nodeId), "NODE_%s", node);
            xSemaphoreTake(mutex, portMAX_DELAY);
            String ts = getTimeISO();
            char line[128];
            snprintf(line, sizeof(line), "%s,%.1f,%.1f,%f,%f,%s", nodeId, t, h, soil, light, ts.c_str());
            strncpy(buffer[head], line, sizeof(buffer[head]) - 1);
            head = (head + 1) % MAX_BUF;
            if (count < MAX_BUF) count++;
            xSemaphoreGive(mutex);
            nodeRegistry[nodeId].lastSeen = millis();
            nodeRegistry[nodeId].rssi = rssi;
            nodeRegistry[nodeId].online = true;
          }
          entry = strtok(NULL, ";");
        }
        continue;
      }

      // STATUS BATCH
      if (strncmp(buf, "BT,", 3) == 0) {
        Serial.println("[GW] STATUS BATCH RX");
        char* record = buf + 3;
        char* entry = strtok(record, "|");
        while (entry) {
          char node[4];
          int pump;
          char mode;
          int soil;
          float amp;
          float flow;
          int nodeRssi = -200;
          if (sscanf(entry, "%3[^,],%d,%c,%d,%f,%f,%d",
                     node, &pump, &mode, &soil, &amp, &flow, &nodeRssi)
              == 7) {
            if (!validShortNode(node)) {
              Serial.println("[SECURITY] invalid node in BT");
              entry = strtok(NULL, ";");
              continue;
            }
            char nodeId[12];
            snprintf(nodeId, sizeof(nodeId), "NODE_%s", node);
            StaticJsonDocument<256> status;
            status["type"] = "STATUS";
            status["node_id"] = nodeId;
            status["pump"] = pump ? "ON" : "OFF";
            char modeStr[2] = { mode, '\0' };
            status["mode"] = modeStr;
            status["last_soil"] = soil;
            status["amp"] = amp;
            status["flow"] = flow;
            int finalRssi = nodeRssi;
            if (nodeRssi == -200) {
              finalRssi = rssi;
            }
            status["rssi"] = finalRssi;
            status["current_status"] = "ONLINE";
            status["gateway_time"] = getTimeISO();
            char msg[512];
            serializeJson(status, msg);
            if (mqtt.connected()) {
              mqtt.publish("garden/status", msg);
            }
            nodeRegistry[nodeId].lastSeen = millis();
            nodeRegistry[nodeId].rssi = finalRssi;
            nodeRegistry[nodeId].online = true;
            Serial.printf("[BATCH STATUS] %s\n", node);
          }
          entry = strtok(NULL, ";");
        }
        continue;
      }

      // ACK
      if (strncmp(buf, "A,", 2) == 0) {
        char* v[16];
        int idx = 0;
        char tmp[256];
        strncpy(tmp, buf, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        char* token = strtok(tmp, ",");

        while (token && idx < 16) {
          v[idx++] = token;
          token = strtok(NULL, ",");
        }
        if (idx < 9) continue;

        if (strcmp(v[1], "LD") != 0) {
          Serial.println("[SECURITY] ACK not from leader");
          continue;
        }
        if (strcmp(v[2], "GW") != 0) {
          Serial.println("[SECURITY] ACK not for gateway");
          continue;
        }
        if (!validShortNode(v[6])) {
          Serial.println("[SECURITY] invalid node_id");
          continue;
        }
        uint16_t ackSeq = atoi(v[4]);
        if (waitingAck && strcmp(v[5], lastCmdId) == 0 && ackSeq == lastSeqSent) {
          waitingAck = false;
          ackTimeout = 0;
          Serial.println("[ACK MATCH - VALID]");
        } else {
          Serial.println("[SECURITY] ACK replay or mismatch");
          continue;
        }
        char nodeId[16];
        snprintf(nodeId, sizeof(nodeId), "NODE_%s", v[6]);
        StaticJsonDocument<512> ack;

        ack["type"] = "ACK";
        ack["node_id"] = nodeId;
        ack["seq"] = v[4];
        ack["cmd_id"] = v[5];
        ack["success"] = atoi(v[7]) == 1;

        if (idx > 8) {
          char err = v[8][0];
          if (err == 'N') ack["error"] = "NONE";
          else if (err == 'I') ack["error"] = "INVALID";
          else ack["error"] = "UNKNOWN";
        }
        if (idx > 9) ack["pump"] = atoi(v[9]);
        if (idx > 10) ack["mode"] = v[10];
        if (idx > 11) ack["flow"] = atof(v[11]);
        if (idx > 12) ack["amp"] = atof(v[12]);
        if (idx > 13) ack["last_soil"] = atoi(v[13]);
        if (idx > 14) ack["node_exec_ms"] = atol(v[14]);

        ack["executed_at"] = getTimeISO();
        ack["gateway_time"] = getTimeISO();
        ack["rssi"] = rssi;
        char out[512];
        serializeJson(ack, out);
        if (mqtt.connected()) {
          mqtt.publish("garden/control/ack", out);
        }
        continue;
      }
      Serial.println("[LORA] UNKNOWN PACKET");
    }

    if (!loraReady && millis() - lastLoRaRetry > 10000) {
      Serial.println("[LORA] Retry init...");
      lastLoRaRetry = millis();
      initLoRa();
    }

    if (waitingAck && millis() > ackTimeout) {
      Serial.println("[ACK] Timeout → release queue");
      waitingAck = false;
      // lastCmdId[0] = '\0';
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// MQTT TASK
void mqttTask(void* p) {
  Serial.println("[CORE] MQTT TASK started");

  mqtt.setServer(MQTT_SRV, 1883);
  mqtt.setCallback(mqttCallback);
  for (;;) {
    wifiReconnect();
    mqttReconnect();
    mqtt.loop();
    vTaskDelay(1);
  }
}

// HTTP TASK
void httpTask(void* p) {
  Serial.println("[CORE] HTTP TASK started");

  static uint32_t batch_id = 0;
  for (;;) {
    if (count >= 10 && WiFi.status() == WL_CONNECTED) {
      String nowISO = getTimeISO();
      if (nowISO == "") nowISO = "1970-01-01T00:00:00";
      xSemaphoreTake(mutex, portMAX_DELAY);
      int tail = (head - count + MAX_BUF) % MAX_BUF;
      DynamicJsonDocument doc(4096);
      doc["gateway_id"] = "GW_01";
      doc["batch_id"] = ++batch_id;
      doc["sent_at"] = nowISO.length() ? nowISO : "1970-01-01T00:00:00";
      JsonArray arr = doc.createNestedArray("measurements");
      for (int i = 0; i < 10; i++) {
        char id[16];
        float t, h, s, l;
        char ts[32];
        sscanf(buffer[(tail + i) % MAX_BUF], "%15[^,],%f,%f,%f,%f,%31[^,\n]", id, &t, &h, &s, &l, ts);
        JsonObject o = arr.createNestedObject();
        o["node_id"] = id;
        o["t"] = t;
        o["h"] = h;
        o["s"] = s;
        o["l"] = l;
        o["ts"] = ts;
      }
      char out[4096];
      serializeJson(doc, out);
      xSemaphoreGive(mutex);
      Serial.println("[HTTP] POST Server");

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
      } else Serial.printf("[HTTP] FAIL %d\n", code);
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
        if (mqtt.connected()) mqtt.publish("garden/status", msg.c_str(), false);
        Serial.println("[NODE OFFLINE] " + it.first);
      }
    }

    vTaskDelay(5000 / portTICK_PERIOD_MS);
  }
}

// SETUP
void setup() {
  Serial.begin(115200);
  mqtt.setBufferSize(1024);
  Serial.println("\n=== ESP32 GARDEN GATEWAY ===");
  // 1. Lấy WiFi từ bộ nhớ
  preferences.begin("wifi-config", true);
  st_ssid = preferences.getString("ssid", "");
  st_pass = preferences.getString("pass", "");
  preferences.end();

  // 2. Thử kết nối WiFi
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  if (st_ssid.length() > 0) {
    WiFi.begin(st_ssid.c_str(), st_pass.c_str());
    Serial.print("[WIFI] Đang kết nối tới: ");
    Serial.println(st_ssid);

    unsigned long start = millis();
    bool connected = false;
    while (millis() - start < 5000) {  // Đợi 20 giây
      if (WiFi.status() == WL_CONNECTED) {
        connected = true;
        break;
      }
      delay(500);
      Serial.print(".");
    }

    if (connected) {
      Serial.println("\n[WIFI] Kết nối thành công!");
    } else {
      Serial.println("\n[WIFI] Thất bại! Chuyển sang chế độ Cấu hình...");
      startConfigPortal();  // Dừng tại đây chờ người dùng nhập WiFi
    }
  } else {
    Serial.println("[WIFI] Không tìm thấy WiFi đã lưu. Khởi động Portal...");
    startConfigPortal();
  }
  mutex = xSemaphoreCreateMutex();
  loraMutex = xSemaphoreCreateMutex();
  if (!loraMutex) {
    Serial.println("[FATAL] LoRa mutex create failed");
    esp_restart();
  }
  // WiFi.mode(WIFI_STA);
  // WiFi.setSleep(false);
  // WiFi.begin(WIFI_SSID, WIFI_PASS);
  // Serial.print("[WIFI] Connecting");
  // unsigned long start = millis();
  // while (WiFi.status() != WL_CONNECTED) {
  //   delay(500);
  //   Serial.print(".");
  //   if (millis() - start > 20000) {
  //     Serial.println("\n[WIFI] TIMEOUT");
  //     break;
  //   }
  // }

  // if (WiFi.status() == WL_CONNECTED) {
  //   Serial.println("\n[WIFI] OK");
  //   Serial.println(WiFi.localIP());
  // } else {
  //   Serial.println("[WIFI] FAIL");
  // }
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
  mqtt.setKeepAlive(30);
  cmdQueue = xQueueCreate(CMD_QUEUE_SIZE, sizeof(CmdItem));
  if (!cmdQueue) {
    Serial.println("[FATAL] CMD Queue create failed");
    esp_restart();
  }
  xTaskCreatePinnedToCore(cmdTask, "cmdTx", 4096, NULL, 7, NULL, 0);
  xTaskCreatePinnedToCore(loraRxTask, "loraRx", 4096, NULL, 6, NULL, 0);
  xTaskCreatePinnedToCore(mqttTask, "mqtt", 4096, NULL, 5, NULL, 1);
  xTaskCreatePinnedToCore(httpTask, "http", 8192, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(webTask, "web", 4096, NULL, 1, NULL, 1);
}

void loop() {
  vTaskDelete(NULL);
}