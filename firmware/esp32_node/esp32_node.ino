#include <LoRa.h>
#include <SPI.h>
#include <DHT.h>

/* =========================================================
   PIN CONFIG
   ========================================================= */

#define PIN_LORA_SS 5
#define PIN_LORA_RST 14
#define PIN_LORA_DIO0 26

#define PIN_DHT 32
#define PIN_SOIL 34
#define PIN_LDR 35
#define PIN_RELAY 22


/* =========================================================
   NODE IDENTIFICATION
   ========================================================= */

const char NODE_ID[] = "NODE_02";
const char NODE_SHORT[] = "02";


/* =========================================================
   INTERVAL CONFIG
   ========================================================= */

const unsigned long STATUS_INTERVAL = 10000;
const unsigned long SENSOR_INTERVAL = 25000;

const unsigned long HEARTBEAT_TIMEOUT = 75000;

const unsigned long SOIL_INTERVAL = 2000;

const unsigned long HELLO_INTERVAL = 10000;
const unsigned long DISCOVERY_WINDOW = 30000;

/* =========================================================
   LORA CONFIG
   ========================================================= */

const int LORA_RETRY_MAX = 5;


/* =========================================================
   COMMAND LOCK
   ========================================================= */

const unsigned long CMD_LOCK_TIME = 1500;


/* =========================================================
   MESH CONFIG
   ========================================================= */

#define MAX_MESH_NODE 5
#define MAX_HOP 3

struct MeshNode {

  char id[4];
  int rssi;
  unsigned long lastSeen;
};

struct NodeMetric {

  char id[4];
  int gwRssi;
  unsigned long lastUpdate;
};


/* =========================================================
   GLOBAL STATE
   ========================================================= */

DHT dht(PIN_DHT, DHT11);

bool loraReady = false;
bool pumpStatus = false;

char mode[4] = "R";

float default_amp = 0.5;
float default_flow = 1.2;

float last_trigger_soil = 0;

float ema_t = 0;
float ema_h = 0;
float ema_l = 0;

float alpha = 0.1;

unsigned long bootTime = 0;

unsigned long lastStatusSend = 0;
unsigned long lastSensorSend = 0;
unsigned long lastAckTime = 0;
unsigned long lastSoilRead = 0;

unsigned long txBlockedUntil = 0;

unsigned long lastGatewaySeen = 0;

#define PACKET_CACHE 64

uint32_t packetCache[PACKET_CACHE];
int packetIndex = 0;

enum NodeState {
  NODE_BOOT,
  NODE_DISCOVERY,
  NODE_NETWORK_READY
};

NodeState nodeState = NODE_BOOT;

unsigned long discoveryStart = 0;
unsigned long lastLeaderSeen = 0;

const unsigned long LEADER_TIMEOUT = 60000;
const unsigned long LEADER_HB_INTERVAL = 15000;

unsigned long lastLeaderHB = 0;

enum DiscoveryState {
  DISC_HELLO_GW,
  DISC_WAIT_R,
  DISC_HELLO_MESH,
  DISC_SEND_METRIC,
  DISC_DONE
};

DiscoveryState discState = DISC_HELLO_GW;
unsigned long helloSentTime = 0;

bool joinSent = false;
bool joinScheduled = false;
unsigned long joinTime = 0;

uint16_t packetSeq = 0;

/* =========================================================
   COMMAND PROCESS
   ========================================================= */

bool cmdProcessing = false;
unsigned long cmdLockUntil = 0;


/* =========================================================
   SENSOR CACHE
   ========================================================= */

int cachedSoil = 0;

/* =========================================================
   LEADER AGGREGATION CACHE
   ========================================================= */

#define LEADER_BUF 6
#define LEADER_TRIGGER 5

struct LeaderSensorPacket {
  char node[4];
  float t;
  float h;
  int soil;
  int light;
};

struct LeaderStatusPacket {
  char node[4];
  int pump;
  char mode;
  int soil;
};

LeaderSensorPacket sensorBuf[LEADER_BUF];
LeaderStatusPacket statusBuf[LEADER_BUF];

int sensorCount = 0;
int statusCount = 0;

unsigned long lastLeaderFlush = 0;
const unsigned long LEADER_FLUSH_INTERVAL = 10000;

/* =========================================================
   MESH STATE
   ========================================================= */

MeshNode meshTable[MAX_MESH_NODE];
int meshCount = 0;

NodeMetric nodeTable[5];
int nodeCount = 0;

char leaderId[4] = "";

bool isLeader = false;
bool leaderLocked = false;
bool networkFormed = false;

unsigned long lastHello = 0;

/* =========================================================
   LORA INIT
   ========================================================= */

void initLoRa() {

  LoRa.setPins(PIN_LORA_SS, PIN_LORA_RST, PIN_LORA_DIO0);

  for (int i = 1; i <= LORA_RETRY_MAX; i++) {

    Serial.printf("[LORA] Đang thử kết nối lần %d/%d...\n", i, LORA_RETRY_MAX);

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

      Serial.println("[LORA] Khởi tạo THÀNH CÔNG");

      return;
    }

    delay(1000);
  }

  Serial.println("[LORA][LỖI] Không tìm thấy module");

  loraReady = false;
}

bool validNode(const char* id) {

  if (strcmp(id, "01") == 0) return true;
  if (strcmp(id, "02") == 0) return true;
  if (strcmp(id, "03") == 0) return true;

  return false;
}

/* =========================================================
   SENSOR PROCESSING
   ========================================================= */

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

bool isRoutable(char t) {
  return (t == 'D' || t == 'S' || t == 'A' || t == 'C');
}

void flushSensorBatch() {

  if (sensorCount == 0) return;
  delay(random(10, 40));
  char packet[256];

  int offset = 0;

  offset += sprintf(packet, "BS,");

  for (int i = 0; i < sensorCount; i++) {

    offset += sprintf(packet + offset,
                      "%s,%.1f,%.1f,%d,%d;",
                      sensorBuf[i].node,
                      sensorBuf[i].t,
                      sensorBuf[i].h,
                      sensorBuf[i].soil,
                      sensorBuf[i].light);
  }

  Serial.println("[LEADER] SENSOR BATCH -> GW");

  sendUplink(packet);

  sensorCount = 0;
}

void flushStatusBatch() {

  if (statusCount == 0) return;
  delay(random(10, 40));
  char packet[256];

  int offset = 0;

  offset += sprintf(packet, "BT,");

  for (int i = 0; i < statusCount; i++) {

    int amp = (int)(default_amp * 10);
    int flow = (int)(default_flow * 10);

    offset += sprintf(packet + offset,
                      "%s,%d,%c,%d,%d,%d;",
                      statusBuf[i].node,
                      statusBuf[i].pump,
                      statusBuf[i].mode,
                      statusBuf[i].soil,
                      amp,
                      flow);
  }

  Serial.println("[LEADER] STATUS BATCH -> GW");

  sendUplink(packet);

  statusCount = 0;
}

/* =========================================================
   LORA SEND
   ========================================================= */

bool sendUplink(const char* data) {

  if (!loraReady) return false;

  LoRa.idle();

  LoRa.beginPacket();
  LoRa.print(data);
  LoRa.endPacket();
  LoRa.receive();

  return true;
}


/* =========================================================
   MESH FUNCTIONS
   ========================================================= */

void printMeshTable() {

  Serial.println("------ MESH TABLE ------");

  for (int i = 0; i < meshCount; i++) {

    Serial.printf("Node: %s | RSSI: %d | LastSeen: %lu ms\n",
                  meshTable[i].id,
                  meshTable[i].rssi,
                  millis() - meshTable[i].lastSeen);
  }

  Serial.println("------------------------");
}

void printMetricTable() {
  Serial.println("----- METRIC TABLE -----");

  for (int i = 0; i < nodeCount; i++) {
    Serial.printf("Node=%s gwRSSI=%d age=%lu\n",
                  nodeTable[i].id,
                  nodeTable[i].gwRssi,
                  millis() - nodeTable[i].lastUpdate);
  }

  Serial.println("------------------------");
}

void updateLastSeen(const char* node) {
  if (strcmp(node, "GW") == 0) return;
  if (strcmp(node, NODE_SHORT) == 0) return;

  for (int i = 0; i < meshCount; i++) {

    if (strcmp(meshTable[i].id, node) == 0) {

      meshTable[i].lastSeen = millis();
      return;
    }
  }

  /* nếu chưa có -> thêm */

  if (meshCount < MAX_MESH_NODE) {

    strcpy(meshTable[meshCount].id, node);
    meshTable[meshCount].rssi = -120;
    meshTable[meshCount].lastSeen = millis();

    meshCount++;

    Serial.printf("[MESH] node dynamically added: %s\n", node);
  }
}

void updateMesh(const char* node, int rssi) {
  for (int i = 0; i < meshCount; i++) {
    if (strcmp(meshTable[i].id, node) == 0) {
      meshTable[i].rssi = rssi;
      meshTable[i].lastSeen = millis();
      return;
    }
  }
  if (meshCount < MAX_MESH_NODE) {
    strcpy(meshTable[meshCount].id, node);
    meshTable[meshCount].rssi = rssi;
    meshTable[meshCount].lastSeen = millis();
    meshCount++;
  }
  Serial.printf("[MESH] discovered %s RSSI=%d\n", node, rssi);
  printMeshTable();
}

void resetMeshNetwork() {
  Serial.println("[MESH] Reset network state");

  meshCount = 0;
  nodeCount = 0;

  leaderId[0] = '\0';
  isLeader = false;
  leaderLocked = false;
  networkFormed = false;

  lastLeaderSeen = millis();
  lastGatewaySeen = millis();

  sensorCount = 0;
  statusCount = 0;
  joinSent = false;
  joinScheduled = false;

  packetIndex = 0;
  memset(packetCache, 0, sizeof(packetCache));

  nodeState = NODE_DISCOVERY;
  discState = DISC_HELLO_GW;
  lastHello = 0;
  discoveryStart = millis();

  Serial.println("[STATE] DISCOVERY MODE (rebuild mesh)");
}

/* =========================================================
   MESH HELLO
   ========================================================= */

void sendHelloGW() {

  char payload[32];

  snprintf(payload, sizeof(payload),
           "H,%s",
           NODE_SHORT);

  Serial.print("[DISCOVERY] HELLO->GW ");
  Serial.println(payload);

  sendUplink(payload);
}

void sendHelloMesh() {

  char payload[32];

  snprintf(payload, sizeof(payload),
           "HM,%s",
           NODE_SHORT);

  Serial.print("[DISCOVERY] HELLO_MESH ");
  Serial.println(payload);

  sendUplink(payload);
}

/* =========================================================
   METRIC TABLE
   ========================================================= */

void updateGatewayRSSI(const char* node, int rssi) {
  for (int i = 0; i < nodeCount; i++) {
    if (strcmp(nodeTable[i].id, node) == 0) {
      nodeTable[i].gwRssi = rssi;
      nodeTable[i].lastUpdate = millis();
      return;
    }
  }
  if (nodeCount < 5) {
    strcpy(nodeTable[nodeCount].id, node);
    nodeTable[nodeCount].gwRssi = rssi;
    nodeTable[nodeCount].lastUpdate = millis();
    nodeCount++;
  }
  printMetricTable();
}


void broadcastMetric() {

  int myRssi = -120;

  for (int i = 0; i < nodeCount; i++) {

    if (strcmp(nodeTable[i].id, NODE_SHORT) == 0) {

      myRssi = nodeTable[i].gwRssi;
      break;
    }
  }

  if (myRssi == -120) {

    Serial.println("[METRIC] cannot broadcast metric (no GW RSSI)");

    return;
  }

  char payload[32];

  snprintf(payload, sizeof(payload),
           "M,%s,%d",
           NODE_SHORT,
           myRssi);

  Serial.print("[METRIC][TX] ");
  Serial.println(payload);

  sendUplink(payload);
}


/* =========================================================
   LEADER ELECTION
   ========================================================= */

void electLeader() {

  if (leaderLocked) return;

  if (networkFormed) return;

  if (nodeCount <= 1) {

    strcpy(leaderId, NODE_SHORT);

    isLeader = true;
    leaderLocked = true;
    networkFormed = true;
    lastLeaderSeen = millis();

    nodeState = NODE_NETWORK_READY;
    Serial.println("[STATE] NETWORK READY");

    Serial.println("[LEADER] Single node mode");
    Serial.println("[LEADER] I am leader");

    return;
  }

  int best = -200;
  char bestNode[4] = "";

  for (int i = 0; i < nodeCount; i++) {

    if (millis() - nodeTable[i].lastUpdate > 30000)
      continue;

    if (nodeTable[i].gwRssi > best || (nodeTable[i].gwRssi == best && strcmp(nodeTable[i].id, bestNode) < 0)) {

      best = nodeTable[i].gwRssi;
      strcpy(bestNode, nodeTable[i].id);
      Serial.printf("[LEADER] candidate %s RSSI=%d\n",
                    nodeTable[i].id,
                    nodeTable[i].gwRssi);
    }
  }

  if (strlen(bestNode) == 0) return;

  strcpy(leaderId, bestNode);

  isLeader = strcmp(leaderId, NODE_SHORT) == 0;

  leaderLocked = true;
  networkFormed = true;

  nodeState = NODE_NETWORK_READY;

  if (isLeader) {

    char msg[32];

    snprintf(msg, sizeof(msg), "L,%s", leaderId);

    sendUplink(msg);

    Serial.println("[LEADER] Broadcast leader");
  }

  Serial.print("[LEADER] Selected: ");
  Serial.println(leaderId);

  Serial.print("[LEADER] I am leader: ");
  Serial.println(isLeader ? "YES" : "NO");
}

bool packetSeen(const char* src, uint16_t seq) {

  uint32_t id = ((uint32_t)seq << 16) | ((uint32_t)src[0] << 8) | src[1];

  for (int i = 0; i < PACKET_CACHE; i++) {

    if (packetCache[i] == id) return true;
  }

  packetCache[packetIndex++] = id;

  if (packetIndex >= PACKET_CACHE)
    packetIndex = 0;

  return false;
}

/* =========================================================
   SETUP
   ========================================================= */

void setup() {

  Serial.begin(115200);

  Serial.println("\n[NODE] Đang khởi động...");

  dht.begin();

  pinMode(PIN_RELAY, OUTPUT);

  digitalWrite(PIN_RELAY, LOW);

  Serial.println("[BOOT] Relay: TẮT | READY");

  bootTime = millis();
  lastGatewaySeen = millis();

  initLoRa();

  nodeState = NODE_DISCOVERY;
  discoveryStart = millis();

  Serial.println("[STATE] DISCOVERY MODE");
  Serial.println("[STATE] Waiting HELLO + RSSI metrics...");
}


/* =========================================================
   LOOP
   ========================================================= */

void loop() {

  /* ================= LORA RX ================= */
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

      char* start = msg;

      while (true) {

        char* sep = strchr(start, ';');

        if (sep != NULL) {
          *sep = '\0';
        }

        char* pkt = start;

        /* ================= DUPLICATE CHECK ================= */
        char src[4] = "";
        char dest[4] = "";
        uint16_t seq = 0;
        int hop = 0;

        bool needDupCheck =
          (pkt[0] == 'D' || pkt[0] == 'S' || pkt[0] == 'A' || pkt[0] == 'C');

        if (needDupCheck) {

          int ok = sscanf(pkt,
                          "%*c,%3[^,],%3[^,],%d,%hu",
                          src,
                          dest,
                          &hop,
                          &seq);

          if (ok == 4 && packetSeen(src, seq)) {
            Serial.println("[DROP] duplicate packet");
            goto next_packet;
          }
          if (!validNode(src) && strcmp(src, "GW") != 0) {
            Serial.println("[SECURITY] invalid SRC");
            goto next_packet;
          }
        } else {

          char type = pkt[0];

          /* ======================================================
         HEARTBEAT  (XỬ LÝ SỚM)
         ====================================================== */

          if (type == 'B') {

            char target[8];

            if (sscanf(pkt, "B,%7s", target) == 1) {

              if (strcmp(target, "ALL") == 0 || strcmp(target, NODE_SHORT) == 0) {

                lastGatewaySeen = millis();

                Serial.println("[HB] Gateway alive");
              }

              if (isLeader) {
                sendUplink(pkt);
              }

            } else {
              Serial.println("[HB] malformed");
            }
          }
          /* ===== CMD ===== */

          else if (pkt[0] == 'C') {

            if (cmdProcessing && millis() < cmdLockUntil) {

              Serial.println("[CMD] Locked");

              return;
            }

            cmdProcessing = true;

            cmdLockUntil = millis() + CMD_LOCK_TIME;
            txBlockedUntil = millis() + 3000;

            int soil_now = readSoil();

            char* v[6];
            int idx = 0;

            char copy[128];
            strcpy(copy, pkt);

            char* token = strtok(copy, ",");

            while (token && idx < 6) {

              v[idx++] = token;
              token = strtok(NULL, ",");
            }

            if (idx < 6) {
              cmdProcessing = false;
              return;
            }

            const char* target = v[2];
            const char* cmd_id = v[3];
            const char* action = v[4];

            if (strcmp(target, NODE_ID) == 0) {

              last_trigger_soil = soil_now;

              bool success = true;

              const char* errorCode = "No";
              const char* message = "OK";

              if (strcmp(action, "ON") == 0) {

                mode[0] = 'C';
                mode[1] = '\0';

                pumpStatus = true;

              } else if (strcmp(action, "OFF") == 0) {
                mode[0] = 'C';
                mode[1] = '\0';

                pumpStatus = false;

              } else if (strcmp(action, "AUTO") == 0) {

                mode[0] = 'S';
                mode[1] = '\0';

              } else {

                success = false;

                errorCode = "INVALID_CMD";

                message = "Unknown action";
              }

              digitalWrite(PIN_RELAY, pumpStatus ? LOW : HIGH);

              unsigned long executedAt = millis();

              char modeShort = mode[0];

              int pumpShort = pumpStatus ? 1 : 0;

              int flowShort = (int)(default_flow * 10);
              int ampShort = (int)(default_amp * 10);

              char ack[128];

              snprintf(ack, sizeof(ack),
                       "A,%s,GW,%d,%s,%d,%s,%s,%d,%c,%d,%d,%d,%lu",
                       NODE_SHORT,
                       MAX_HOP,
                       cmd_id,
                       success ? 1 : 0,
                       errorCode,
                       message,
                       pumpShort,
                       modeShort,
                       flowShort,
                       ampShort,
                       (int)last_trigger_soil,
                       executedAt);

              Serial.print("[LORA][TX ACK] ");
              Serial.println(ack);

              delay(80);
              sendUplink(ack);

              lastAckTime = millis();

              lastStatusSend = millis();
              lastSensorSend = millis();

              cmdProcessing = false;
              txBlockedUntil = millis() + 3000;
            }
          }
          /* ======================================================
         HELLO MESH
         ====================================================== */

          else if (strncmp(pkt, "HM,", 3) == 0) {

            char node[4];

            if (sscanf(pkt, "HM,%3[^,\r\n]", node) == 1) {

              if (strcmp(node, NODE_SHORT) != 0) {

                int rssi = LoRa.packetRssi();

                updateMesh(node, rssi);

              } else {
                Serial.println("[MESH] ignore self");
              }

            } else {
              Serial.println("[MESH] malformed HM");
            }
          }

          /* ======================================================
         GW RSSI METRIC
         ====================================================== */

          else if (type == 'R') {

            char node[4];
            int rssi;

            if (sscanf(pkt, "R,%3[^,],%d", node, &rssi) == 2) {

              Serial.printf("[METRIC][GW] node=%s rssi=%d\n", node, rssi);

              if (strcmp(node, NODE_SHORT) == 0) {

                updateGatewayRSSI(NODE_SHORT, rssi);

                if (nodeState == NODE_DISCOVERY && discState == DISC_WAIT_R) {

                  Serial.println("[DISCOVERY] GW RSSI received");

                  discState = DISC_HELLO_MESH;
                }

              } else {

                Serial.println("[METRIC] not for me");
              }

            } else {
              Serial.println("[METRIC] malformed R");
            }
          }

          /* ======================================================
         NODE METRIC
         ====================================================== */

          else if (type == 'M') {

            char node[4];
            int rssi;

            if (sscanf(pkt, "M,%3[^,],%d", node, &rssi) == 2) {

              if (strcmp(node, NODE_SHORT) != 0) {

                updateGatewayRSSI(node, rssi);

                updateLastSeen(node);

              } else {

                Serial.println("[MESH] ignore self");
              }

            } else {
              Serial.println("[METRIC NODE] malformed");
            }
          }

          /* ======================================================
         JOIN REPLY
         ====================================================== */

          else if (strncmp(pkt, "JR,", 3) == 0) {

            if (networkFormed || strlen(leaderId) > 0) {

              Serial.println("[JOIN] ignore JR (already in mesh)");

            } else {

              char leader[4];

              if (sscanf(pkt, "JR,%3[^,\r\n]", leader) == 1) {

                strcpy(leaderId, leader);

                networkFormed = true;
                leaderLocked = true;

                discState = DISC_DONE;
                nodeState = NODE_NETWORK_READY;

                lastLeaderSeen = millis();

                Serial.printf("[JOIN] connected to leader %s\n", leaderId);

              } else {

                Serial.println("[JOIN] malformed JR");
              }
            }
          }

          /* ======================================================
         LEADER ANNOUNCE
         ====================================================== */

          else if (strncmp(pkt, "L,", 2) == 0) {

            char leader[4];

            if (sscanf(pkt, "L,%3s", leader) == 1) {

              strcpy(leaderId, leader);

              networkFormed = true;
              leaderLocked = true;

              isLeader = strcmp(leaderId, NODE_SHORT) == 0;

              nodeState = NODE_NETWORK_READY;

              Serial.print("[LEADER] network leader = ");
              Serial.println(leaderId);

            } else {

              Serial.println("[LEADER] malformed");
            }
          }

          /* ======================================================
         LEADER HEARTBEAT
         ====================================================== */

          else if (strncmp(pkt, "HL,", 3) == 0) {

            char leader[4];

            if (sscanf(pkt, "HL,%3[^,\r\n]", leader) == 1) {

              if (strcmp(leader, leaderId) == 0) {

                lastLeaderSeen = millis();

                Serial.println("[HB] Leader alive");
              }

            } else {

              Serial.println("[HL] malformed");
            }
          }
          /* ======================================================
            JOIN REQUEST
            ====================================================== */

          else if (strncmp(pkt, "J,", 2) == 0) {

            char node[4];

            if (sscanf(pkt, "J,%3[^,\r\n]", node) == 1) {

              if (strcmp(node, NODE_SHORT) == 0) {
                Serial.println("[JOIN] ignore self");
                goto next_packet;
              }

              else if (!isLeader) {

                Serial.println("[JOIN] ignore (not leader)");

              }

              else {

                Serial.printf("[JOIN] request from %s\n", node);

                updateLastSeen(node);
                updateGatewayRSSI(node, -120);

                char reply[16];

                snprintf(reply,
                         sizeof(reply),
                         "JR,%s",
                         leaderId);

                sendUplink(reply);

                Serial.printf("[JOIN] accepted %s\n", node);
              }

            } else {

              Serial.println("[JOIN] malformed");
            }
          }

          /* ======================================================
        ROUTABLE PACKETS
        ====================================================== */

          else if (isRoutable(type)) {

            char src[4];
            char dest[4];
            int hop;
            char data[96];

            uint16_t seq;

            int ok = sscanf(pkt,
                            "%c,%3[^,],%3[^,],%d,%hu,%95[^\n]",
                            &type,
                            src,
                            dest,
                            &hop,
                            &seq,
                            data);

            if (ok < 4) {

              Serial.println("[ROUTE] malformed");

            } else {

              updateLastSeen(src);

              if (hop <= 1) {

                Serial.println("[DROP] hop=0");

              } else {

                hop--;

                if (hop == 0) {

                  Serial.println("[DROP] TTL expired");

                }

                else {

                  /* ======================================
           PACKET FOR THIS NODE
           ====================================== */

                  if (strcmp(dest, NODE_SHORT) == 0) {

                    Serial.println("[ROUTE] packet for me");

                  }

                  /* ======================================
           ROUTE TO GATEWAY
           ====================================== */

                  else if (strcmp(dest, "GW") == 0 && !isLeader && strcmp(leaderId, "") != 0) {
                    if (strcmp(src, NODE_SHORT) == 0) goto next_packet;
                    Serial.println("[ROUTE] forward to leader");

                    char fwd[128];

                    if (ok >= 6) {
                      snprintf(fwd,
                               sizeof(fwd),
                               "%c,%s,%s,%d,%u,%s",
                               type,
                               src,
                               dest,
                               hop,
                               seq,
                               data);

                    } else {
                      snprintf(fwd,
                               sizeof(fwd),
                               "%c,%s,%s,%d,%u",
                               type,
                               src,
                               dest,
                               hop,
                               seq);
                    }

                    sendUplink(fwd);
                  }

                  /* ======================================
           LEADER HANDLING
           ====================================== */

                  else if (isLeader) {

                    /* ===== SENSOR DATA ===== */

                    if (type == 'D') {

                      if (sensorCount < LEADER_BUF) {

                        sscanf(pkt,
                               "%*c,%3[^,],%*[^,],%*d,%*u,%f,%f,%d,%d",
                               sensorBuf[sensorCount].node,
                               &sensorBuf[sensorCount].t,
                               &sensorBuf[sensorCount].h,
                               &sensorBuf[sensorCount].soil,
                               &sensorBuf[sensorCount].light);

                        sensorCount++;

                        Serial.printf("[LEADER] sensor cached (%d)\n",
                                      sensorCount);

                        if (sensorCount >= LEADER_TRIGGER) {
                          flushSensorBatch();
                        }
                      }

                    }

                    /* ===== STATUS DATA ===== */

                    else if (type == 'S') {

                      if (statusCount < LEADER_BUF) {

                        sscanf(pkt,
                               "S,%3[^,],%*[^,],%*d,%*u,%d,%c,%*lu,%*d,%*d,%d",
                               statusBuf[statusCount].node,
                               &statusBuf[statusCount].pump,
                               &statusBuf[statusCount].mode,
                               &statusBuf[statusCount].soil);

                        statusCount++;

                        Serial.printf("[LEADER] status cached (%d)\n",
                                      statusCount);

                        if (statusCount >= LEADER_TRIGGER) {
                          flushStatusBatch();
                        }
                      }
                    }

                    else if (type == 'A') {
                      sendUplink(pkt);
                    }
                  }
                }
              }
            }
          }

          /* ======================================================
         UNKNOWN PACKET
         ====================================================== */

          else {

            Serial.println("[LORA] Unknown packet");
          }
        }
next_packet:
        if (sep == NULL)
          break;

        start = sep + 1;
      }
    }
  }

  if (isLeader && millis() - lastLeaderFlush > LEADER_FLUSH_INTERVAL) {

    flushSensorBatch();
    flushStatusBatch();

    lastLeaderFlush = millis();
  }

  if (!isLeader && networkFormed) {
    if (millis() - lastLeaderSeen > LEADER_TIMEOUT) {

      Serial.println("[MESH] Leader lost -> rebuilding mesh");
      resetMeshNetwork();

      lastHello = 0;
    }
  }

  for (int i = 0; i < meshCount; i++) {
    if (strcmp(meshTable[i].id, NODE_SHORT) == 0)
      continue;
    if (millis() - meshTable[i].lastSeen > 30000) {
      Serial.printf("[MESH] node %s timeout\n", meshTable[i].id);

      for (int j = i; j < meshCount - 1; j++)
        meshTable[j] = meshTable[j + 1];

      meshCount--;
      i--;
    }
  }

  /* ================= AUTO MODE ================= */

  if (strcmp(mode, "SEN") == 0) {

    if (cachedSoil < 30 && !pumpStatus) {

      pumpStatus = true;
      last_trigger_soil = cachedSoil;

    } else if (cachedSoil > 70 && pumpStatus) {

      pumpStatus = false;
      last_trigger_soil = cachedSoil;
    }
  }

  if (millis() - lastGatewaySeen > HEARTBEAT_TIMEOUT) {

    if (strcmp(mode, "SEN") != 0) {
      Serial.println("[AUTO MODE] Gateway lost");
      strcpy(mode, "SEN");
    }
  }

  digitalWrite(PIN_RELAY, pumpStatus ? LOW : HIGH);


  /* ================= STATUS SEND ================= */

  if (nodeState == NODE_NETWORK_READY && !cmdProcessing && millis() > txBlockedUntil && millis() - lastStatusSend > STATUS_INTERVAL + random(0, 1000) && millis() - lastAckTime > STATUS_INTERVAL) {

    lastStatusSend = millis();

    unsigned long uptime = (millis() - bootTime) / 1000;

    if (isLeader) {

      if (statusCount < LEADER_BUF) {

        strcpy(statusBuf[statusCount].node, NODE_SHORT);

        statusBuf[statusCount].pump = pumpStatus ? 1 : 0;

        statusBuf[statusCount].mode = mode[0];

        statusBuf[statusCount].soil = cachedSoil;

        statusCount++;

        Serial.printf("[LEADER] self status cached (%d)\n", statusCount);

        if (statusCount >= LEADER_TRIGGER) {
          flushStatusBatch();
        }
      }

    } else {
      char payload[80];
      packetSeq++;

      snprintf(payload, sizeof(payload),
               "S,%s,%s,%d,%u,%d,%c,%lu,%d,%d,%d",
               NODE_SHORT,
               leaderId,
               MAX_HOP,
               packetSeq,
               pumpStatus ? 1 : 0,
               mode,
               uptime,
               (int)(default_amp * 10),
               (int)(default_flow * 10),
               (int)last_trigger_soil);

      Serial.print("[LORA][STATUS] ");
      Serial.println(payload);

      sendUplink(payload);
    }
  }


  /* ================= SENSOR SEND ================= */

  if (nodeState == NODE_NETWORK_READY && !cmdProcessing && millis() > txBlockedUntil && millis() - lastSensorSend > SENSOR_INTERVAL + random(0, 3000) && millis() - lastAckTime > SENSOR_INTERVAL + random(0, 3000)) {

    lastSensorSend = millis();


    Serial.print("[ROUTE] Sensor route: ");
    Serial.print(NODE_SHORT);
    Serial.print(" -> ");
    Serial.println((isLeader || strlen(leaderId) == 0) ? "GW" : leaderId);

    const char* dest;

    if (isLeader)
      dest = "GW";
    else if (strlen(leaderId) > 0)
      dest = leaderId;
    else
      return;

    if (isLeader) {

      if (sensorCount < LEADER_BUF) {

        strcpy(sensorBuf[sensorCount].node, NODE_SHORT);

        sensorBuf[sensorCount].t = ema_t;
        sensorBuf[sensorCount].h = ema_h;
        sensorBuf[sensorCount].soil = cachedSoil;
        sensorBuf[sensorCount].light = (int)ema_l;

        sensorCount++;

        Serial.printf("[LEADER] self sensor cached (%d)\n", sensorCount);

        if (sensorCount >= LEADER_TRIGGER) {
          flushSensorBatch();
        }
      }

    } else {
      char payload[80];

      packetSeq++;

      snprintf(payload, sizeof(payload),
               "D,%s,%s,%d,%u,%.1f,%.1f,%d,%d",
               NODE_SHORT,
               leaderId,
               MAX_HOP,
               packetSeq,
               ema_t,
               ema_h,
               cachedSoil,
               (int)ema_l);

      sendUplink(payload);
    }
  }

  if (isLeader && networkFormed && millis() - lastLeaderHB > LEADER_HB_INTERVAL) {
    char hb[16];

    snprintf(hb, sizeof(hb), "HL,%s", NODE_SHORT);

    delay(random(50, 200));
    sendUplink(hb);

    Serial.println("[LEADER HB] sent");

    lastLeaderHB = millis();
  }

  /* ================= SENSOR UPDATE ================= */

  processSensors();

  if (millis() - lastSoilRead > SOIL_INTERVAL) {

    cachedSoil = readSoil();

    lastSoilRead = millis();
  }


  /* ================= HELLO SEND ================= */

  if (nodeState == NODE_DISCOVERY) {

    switch (discState) {

      case DISC_HELLO_GW:

        if (millis() - lastHello > HELLO_INTERVAL + random(0, 500)) {

          lastHello = millis();

          sendHelloGW();

          helloSentTime = millis();

          discState = DISC_WAIT_R;
        }

        break;

      case DISC_WAIT_R:

        /* wait gateway reply */

        if (millis() - helloSentTime > 5000) {

          Serial.println("[DISCOVERY] GW reply timeout");

          discState = DISC_HELLO_GW;
        }

        break;

      case DISC_HELLO_MESH:

        delay(random(50, 200));

        sendHelloMesh();

        Serial.println("[DISCOVERY] HELLO_MESH broadcast");

        printMeshTable();

        if (meshCount > 0) {
          Serial.println("[DISCOVERY] mesh nodes detected");
        } else {
          Serial.println("[DISCOVERY] no mesh found");
        }

        delay(random(50, 200));

        discState = DISC_SEND_METRIC;

        break;

      case DISC_SEND_METRIC:

        broadcastMetric();

        printMetricTable();

        discState = DISC_DONE;

        break;
      case DISC_DONE:

        if (!networkFormed && !joinScheduled) {

          joinScheduled = true;

          joinTime = millis() + random(3000, 8000);

          Serial.println("[DISCOVERY] mesh found -> scheduling JOIN");
        }

        if (joinScheduled && !joinSent && millis() > joinTime) {

          char payload[16];

          snprintf(payload, sizeof(payload),
                   "J,%s",
                   NODE_SHORT);

          Serial.println("[DISCOVERY] sending JOIN");

          sendUplink(payload);

          joinSent = true;
        }

        break;
    }
  }

  if (nodeState == NODE_DISCOVERY && millis() - discoveryStart > DISCOVERY_WINDOW) {
    Serial.println("[DISCOVERY] Window closed -> elect leader");
    electLeader();
  }
}