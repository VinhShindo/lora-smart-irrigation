#include <LoRa.h>
#include <SPI.h>
#include <DHT.h>

// PIN CONFIG
#define PIN_LORA_SS 5
#define PIN_LORA_RST 14
#define PIN_LORA_DIO0 26

#define PIN_DHT 32
#define PIN_SOIL 34
#define PIN_LDR 35
#define PIN_RELAY 22

//  NODE IDENTIFICATION
const char NODE_ID[] = "NODE_03";
const char NODE_SHORT[] = "03";

//  INTERVAL CONFIG
const unsigned long STATUS_INTERVAL = 10000;
const unsigned long SENSOR_INTERVAL = 25000;
const unsigned long HEARTBEAT_TIMEOUT = 75000;
const unsigned long SOIL_INTERVAL = 2000;
const unsigned long HELLO_INTERVAL = 10000;
const unsigned long DISCOVERY_WINDOW = 30000;

//  LORA CONFIG
const int LORA_RETRY_MAX = 5;

//  COMMAND LOCK
const unsigned long CMD_LOCK_TIME = 1500;

//  MESH CONFIG
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

//  GLOBAL STATE
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
unsigned long lastGatewaySeen = 0;
#define PACKET_CACHE 128
struct PacketCacheItem {
  uint32_t id;
  uint32_t time;
};
PacketCacheItem packetCache[PACKET_CACHE];
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

enum TxType {
  TX_NORMAL,
  TX_CMD,
  TX_ACK
};

// SENSOR CACHE
int cachedSoil = 0;

// LEADER AGGREGATION CACHE
#define LEADER_BUF 6

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

//  MESH STATE
MeshNode meshTable[MAX_MESH_NODE];
int meshCount = 0;
NodeMetric nodeTable[5];
int nodeCount = 0;
char leaderId[4] = "";
bool isLeader = false;
bool leaderLocked = false;
bool networkFormed = false;
unsigned long lastHello = 0;

struct TxItem {
  char data[160];
  TxType type;
};

#define CMD_QUEUE_SIZE 10
#define NORMAL_QUEUE_SIZE 20

TxItem cmdQueue[CMD_QUEUE_SIZE];
TxItem normalQueue[NORMAL_QUEUE_SIZE];

int cmdHead = 0, cmdTail = 0;
int normHead = 0, normTail = 0;

#define CMD_EXEC_CACHE 30

struct CmdExecItem {
  uint16_t seq;
  char cmd_id[32];
  char target[4];
  char src[4];
  unsigned long time;
};

CmdExecItem cmdExecCache[CMD_EXEC_CACHE];
int cmdExecIndex = 0;

bool ackWindowActive = false;
unsigned long ackWindowStart = 0;
const unsigned long ACK_WINDOW_TIME = 1200;  // ms

//  LORA INIT
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

// SENSOR PROCESSING
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

bool dropOldestNormal() {
  if (normHead == normTail) return false;  // empty

  Serial.println("[NORMQ] DROP oldest");
  normHead = (normHead + 1) % NORMAL_QUEUE_SIZE;
  return true;
}

bool pushTx(const char* data, TxType type) {
  if (!loraReady) return false;
  // if (ackWindowActive && isLeader && type != TX_ACK) {
  //   return false;
  // }
  if (type == TX_CMD || type == TX_ACK) {
    int next = (cmdTail + 1) % CMD_QUEUE_SIZE;
    if (next == cmdHead) {
      Serial.println("[CMDQ] FULL -> try free space");
      if (dropOldestNormal()) {
        Serial.println("[CMDQ] freed by dropping NORMAL");
      } else {
        Serial.println("[CMDQ] overwrite oldest CMD");
        cmdHead = (cmdHead + 1) % CMD_QUEUE_SIZE;
      }
    }
    strncpy(cmdQueue[cmdTail].data, data, sizeof(cmdQueue[cmdTail].data) - 1);
    cmdQueue[cmdTail].data[sizeof(cmdQueue[cmdTail].data) - 1] = '\0';
    cmdQueue[cmdTail].type = type;
    cmdTail = (cmdTail + 1) % CMD_QUEUE_SIZE;
    return true;
  } else {
    int next = (normTail + 1) % NORMAL_QUEUE_SIZE;
    if (next == normHead) {
      Serial.println("[NORMQ] FULL -> overwrite oldest");
      normHead = (normHead + 1) % NORMAL_QUEUE_SIZE;
    }
    strncpy(normalQueue[normTail].data, data, sizeof(normalQueue[normTail].data) - 1);
    normalQueue[normTail].data[sizeof(normalQueue[normTail].data) - 1] = '\0';
    normalQueue[normTail].type = type;
    normTail = (normTail + 1) % NORMAL_QUEUE_SIZE;
    return true;
  }
}

void handleTxQueue() {
  if (!loraReady) return;
  if (ackWindowActive && isLeader) {
    return;
  }
  static unsigned long lastTx = 0;
  if (millis() - lastTx < 20) return;
  TxItem item;
  if (cmdHead != cmdTail) {
    item = cmdQueue[cmdHead];
    cmdHead = (cmdHead + 1) % CMD_QUEUE_SIZE;
  } else {
    if (normHead == normTail) return;
    item = normalQueue[normHead];
    normHead = (normHead + 1) % NORMAL_QUEUE_SIZE;
  }
  LoRa.idle();
  delay(3);
  LoRa.beginPacket();
  LoRa.print(item.data);
  LoRa.endPacket();
  if (item.type == TX_CMD && isLeader) {
    ackWindowActive = true;
    ackWindowStart = millis();
    Serial.println("[ACK WINDOW] OPEN");
  }
  delay(8);
  LoRa.receive();
  lastTx = millis();
}

void flushSensorBatch() {
  if (sensorCount == 0) return;

  char packet[320];
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
  pushTx(packet, TX_NORMAL);
  Serial.print("[TX][BATCH_SENSOR] ");
  Serial.print(NODE_SHORT);
  Serial.print(" -> GW | ");
  Serial.println(packet);
  sensorCount = 0;
  lastLeaderFlush = millis();
}

void flushStatusBatch() {
  if (statusCount == 0) return;

  char packet[320];
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
  pushTx(packet, TX_NORMAL);
  Serial.print("[TX][BATCH_STATUS] ");
  Serial.print(NODE_SHORT);
  Serial.print(" -> GW | ");
  Serial.println(packet);
  statusCount = 0;
  lastLeaderFlush = millis();
}

//  MESH FUNCTIONS
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

int getActiveNodeCount() {
  int count = 0;
  for (int i = 0; i < meshCount; i++) {
    if (millis() - meshTable[i].lastSeen < 30000) {
      count++;
    }
  }
  count++;
  return count;
}

bool isSensorBatchComplete() {
  int expected = max(1, getActiveNodeCount());
  bool leaderFound = false;
  for (int i = 0; i < sensorCount; i++) {
    if (strcmp(sensorBuf[i].node, NODE_SHORT) == 0) {
      leaderFound = true;
      break;
    }
  }
  if (!leaderFound) return false;
  return sensorCount >= expected;
}

bool isStatusBatchComplete() {
  int expected = max(1, getActiveNodeCount());
  bool leaderFound = false;
  for (int i = 0; i < statusCount; i++) {
    if (strcmp(statusBuf[i].node, NODE_SHORT) == 0) {
      leaderFound = true;
      break;
    }
  }
  if (!leaderFound) return false;
  return statusCount >= expected;
}

//  MESH HELLO
void sendHelloGW() {
  char payload[32];
  snprintf(payload, sizeof(payload),
           "H,%s",
           NODE_SHORT);
  pushTx(payload, TX_NORMAL);
  Serial.print("[DISCOVERY] HELLO->GW ");
  Serial.println(payload);
}

void sendHelloMesh() {
  char payload[32];
  snprintf(payload, sizeof(payload),
           "HM,%s",
           NODE_SHORT);
  pushTx(payload, TX_NORMAL);
  Serial.print("[DISCOVERY] HELLO_MESH ");
  Serial.println(payload);
}

//  METRIC TABLE
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
  pushTx(payload, TX_NORMAL);
  Serial.print("[METRIC][TX] ");
  Serial.println(payload);
}

// LEADER ELECTION
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
    pushTx(msg, TX_NORMAL);
    Serial.println("[LEADER] Broadcast leader");
  }
  Serial.print("[LEADER] Selected: ");
  Serial.println(leaderId);
  Serial.print("[LEADER] I am leader: ");
  Serial.println(isLeader ? "YES" : "NO");
}

uint32_t hashIdentity(const char* pkt) {
  char type = pkt[0];
  uint32_t h = 2166136261UL;  // FNV offset
  auto hashAdd = [&](const char* s) {
    while (*s) {
      h ^= (uint8_t)*s++;
      h *= 16777619;
    }
  };
  char src[4], dest[4], cmd_id[32], node_id[4];
  uint16_t seq;
  if (type == 'D' || type == 'S') {
    if (sscanf(pkt, "%*c,%3[^,],%*[^,],%*d,%hu", src, &seq) == 2) {
      hashAdd(src);
      h ^= seq;
    }
  } else if (type == 'C') {
    if (sscanf(pkt, "%*c,%*[^,],%*[^,],%*d,%hu,%31[^,],%3[^,]", &seq, cmd_id, node_id) == 3) {
      h ^= seq;
      hashAdd(cmd_id);
      hashAdd(node_id);
    }
  } else if (type == 'A') {
    char src[4], dest[4];
    if (sscanf(pkt, "A,%3[^,],%3[^,],%*d,%hu,%31[^,],%3[^,]",
               src, dest, &seq, cmd_id, node_id)
        == 5) {
      hashAdd(src);
      hashAdd(dest);
      h ^= seq;
      hashAdd(cmd_id);
      hashAdd(node_id);
    }
  } else {
    hashAdd(pkt);
  }
  return h;
}

bool packetSeen(const char* pkt) {
  if (pkt == NULL || pkt[0] == '\0') return true;
  char type = pkt[0];
  if (type == 'H' || type == 'R' || type == 'M' || type == 'B') {
    return false;
  }
  uint32_t id = hashIdentity(pkt);
  uint32_t now = millis();
  for (int i = 0; i < PACKET_CACHE; i++) {
    if (packetCache[i].id == id && (now - packetCache[i].time) < 700) {
      return true;
    }
  }
  packetCache[packetIndex].id = id;
  packetCache[packetIndex].time = now;
  packetIndex++;
  if (packetIndex >= PACKET_CACHE) packetIndex = 0;
  return false;
}

bool leaderOnlyPacket(const char* pkt) {
  if (strncmp(pkt, "BS,", 3) == 0) return true;
  if (strncmp(pkt, "BT,", 3) == 0) return true;
  return false;
}

/* ================= CMD HANDLER ================= */
/* ===== Leader forward CMD to node ===== */
void forwardCmdToNode(const char* pkt) {
  char src[4], dest[4], target[4], cmd_id[32];
  int hop, action;
  uint16_t seq;
  int ok = sscanf(pkt,
                  "C,%3[^,],%3[^,],%d,%hu,%31[^,],%3[^,],%d",
                  src, dest, &hop, &seq, cmd_id, target, &action);
  if (ok != 7) return;
  if (!isLeader) return;
  hop--;
  if (hop <= 0) return;
  char fwd[128];
  snprintf(
    fwd,
    sizeof(fwd),
    "C,%s,%s,%d,%u,%s,%s,%d",
    "LD",    // LD
    target,  // node
    hop,
    seq,
    cmd_id,
    target,
    action);

  pushTx(fwd, TX_CMD);
  ackWindowActive = true;
  ackWindowStart = millis();
  Serial.print("[CMD][LEADER->NODE] ");
  Serial.println(fwd);
}

/* ===== Node execute CMD ===== */
void executeCmd(const char* pkt) {
  char src[4], dest[4], target[4], cmd_id[32];
  int hop, action;
  uint16_t seq;
  int ok = sscanf(pkt,
                  "C,%3[^,],%3[^,],%d,%hu,%31[^,],%3[^,],%d",
                  src, dest, &hop, &seq, cmd_id, target, &action);

  if (ok != 7) {
    Serial.println("[CMD] malformed");
    return;
  }
  if (cmdExecuted(seq, cmd_id, target, src)) {
    Serial.println("[CMD] duplicate EXECUTE blocked");
    return;
  }
  if (strcmp(target, NODE_SHORT) != 0) {
    return;
  }
  int soil_now = readSoil();
  last_trigger_soil = soil_now;
  bool success = true;
  if (action == 1) {
    strcpy(mode, "C");
    pumpStatus = true;
  } else if (action == 0) {
    strcpy(mode, "C");
    pumpStatus = false;
  } else if (action == 2) {
    strcpy(mode, "SEN");
  } else {
    success = false;
  }
  digitalWrite(PIN_RELAY, pumpStatus ? LOW : HIGH);
  char ack[160];
  const char* ackSrc = NODE_SHORT;
  const char* ackDest = "LD";
  if (isLeader) {
    ackDest = "GW";
    ackSrc = "LD";
  }
  snprintf(
    ack,
    sizeof(ack),
    "A,%s,%s,%d,%u,%s,%s,%d,%c,%d,%c,%d,%d,%d",
    ackSrc,   // src
    ackDest,  // dest
    MAX_HOP,
    seq,
    cmd_id,
    NODE_SHORT,  // node_id
    success ? 1 : 0,
    success ? 'N' : 'I',
    pumpStatus ? 1 : 0,
    mode[0],
    (int)(default_flow * 10),
    (int)(default_amp * 10),
    soil_now);
  if (!isLeader) {
    if (ackWindowActive) {
      delay(random(50, 120));
    }
  }
  pushTx(ack, TX_ACK);
  Serial.print("[CMD][ACK->LEADER] ");
  Serial.println(ack);
  lastAckTime = millis();
  for (int i = 0; i < statusCount; i++) {
    if (strcmp(statusBuf[i].node, NODE_SHORT) == 0) {
      statusBuf[i].pump = pumpStatus;
      statusBuf[i].mode = mode[0];
      statusBuf[i].soil = soil_now;
      return;
    }
  }
}

/* ===== Leader forward ACK to GW ===== */
void handleAckAtLeader(const char* pkt) {
  if (!isLeader) return;
  char src[4], dest[4], cmd_id[32], node_id[4];
  int hop, success, pump, flow, amp, soil;
  char statusChar, modeChar;
  uint16_t seq;
  int ok = sscanf(pkt,
                  "A,%3[^,],%3[^,],%d,%hu,%31[^,],%3[^,],%d,%c,%d,%c,%d,%d,%d",
                  src, dest, &hop, &seq,
                  cmd_id, node_id,
                  &success,
                  &statusChar,
                  &pump,
                  &modeChar,
                  &flow,
                  &amp,
                  &soil);
  if (ok < 13) {
    Serial.println("[ACK] malformed");
    return;
  }
  updateLastSeen(node_id);
  updateLastSeen(src);
  if (strcmp(dest, "LD") != 0) return;
  hop--;
  if (hop <= 0) return;
  char fwd[180];
  snprintf(fwd, sizeof(fwd),
           "A,%s,%s,%d,%u,%s,%s,%d,%c,%d,%c,%d,%d,%d",
           "LD",  // src
           "GW",  // dest
           hop,
           seq,
           cmd_id,
           node_id,
           success,
           statusChar,
           pump,
           modeChar,
           flow,
           amp,
           soil);
  pushTx(fwd, TX_ACK);
  Serial.print("[ACK][LEADER->GW] ");
  Serial.println(fwd);
}

bool cmdExecuted(uint16_t seq, const char* cmd_id, const char* target, const char* src) {
  unsigned long now = millis();

  for (int i = 0; i < CMD_EXEC_CACHE; i++) {
    if (cmdExecCache[i].seq == seq && strcmp(cmdExecCache[i].cmd_id, cmd_id) == 0 && strcmp(cmdExecCache[i].target, target) == 0 && strcmp(cmdExecCache[i].src, src) == 0 && (now - cmdExecCache[i].time) < 10000) {
      return true;
    }
  }
  cmdExecCache[cmdExecIndex].seq = seq;
  strcpy(cmdExecCache[cmdExecIndex].cmd_id, cmd_id);
  strcpy(cmdExecCache[cmdExecIndex].target, target);
  strcpy(cmdExecCache[cmdExecIndex].src, src);
  cmdExecCache[cmdExecIndex].time = now;

  cmdExecIndex = (cmdExecIndex + 1) % CMD_EXEC_CACHE;
  return false;
}
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
      Serial.print("[LORA][RECEIVE] ");
      Serial.println(msg);

      char* start = msg;
      while (true) {

        char* sep = strchr(start, ';');
        if (sep != NULL) {
          *sep = '\0';
        }
        char* pkt = start;
        char type = pkt[0];
        if (ackWindowActive && isLeader) {
          if (type != 'A') {
            goto next_packet;
          }
        }
        if (packetSeen(pkt)) {
          Serial.println("[DROP] duplicate packet");
          goto next_packet;
        }
        /* ===== LEADER ONLY PACKET FILTER ===== */
        if (!isLeader && leaderOnlyPacket(pkt)) {
          goto next_packet;
        }

        /* ===== ACK PRIORITY ===== */
        if (type == 'A') {
          char src[4], dest[4], cmd_id[32], node_id[4];
          int hop, success, pump, flow, amp, soil;
          char statusChar, modeChar;
          uint16_t seq;
          int ok = sscanf(pkt,
                          "A,%3[^,],%3[^,],%d,%hu,%31[^,],%3[^,],%d,%c,%d,%c,%d,%d,%d",
                          src, dest, &hop, &seq,
                          cmd_id, node_id,
                          &success,
                          &statusChar,
                          &pump,
                          &modeChar,
                          &flow,
                          &amp,
                          &soil);
          if (ok < 13) {
            Serial.println("[ACK] malformed");
            goto next_packet;
          }
          lastAckTime = millis();
          if (isLeader && ackWindowActive) {
            ackWindowActive = false;
            Serial.println("[ACK WINDOW] CLOSED (ACK RECEIVED)");
          }
          if (hop <= 1) {
            Serial.println("[ACK] hop exhausted");
            goto next_packet;
          }
          if (strcmp(src, "LD") == 0) {
            lastLeaderSeen = millis();
          }
          if (isLeader) {
            handleAckAtLeader(pkt);
            goto next_packet;
          }
          if (strcmp(dest, NODE_SHORT) != 0 || (strcmp(src, "LD") == 0 && strcmp(dest, "GW") == 0)) {
            hop--;
            char fwd[180];
            snprintf(fwd, sizeof(fwd),
                     "A,%s,%s,%d,%u,%s,%s,%d,%c,%d,%c,%d,%d,%d",
                     src,
                     dest,
                     hop,
                     seq,
                     cmd_id,
                     node_id,
                     success,
                     statusChar,
                     pump,
                     modeChar,
                     flow,
                     amp,
                     soil);
            pushTx(fwd, TX_ACK);
            Serial.println("[ACK] forward mesh");
          }

          goto next_packet;
        }
        /* ===== CMD ===== */
        if (pkt[0] == 'C') {
          char src[4], dest[4], target[4], cmd_id[32];
          int hop, action;
          uint16_t seq;
          int ok = sscanf(pkt,
                          "C,%3[^,],%3[^,],%d,%hu,%31[^,],%3[^,],%d",
                          src, dest, &hop, &seq, cmd_id, target, &action);
          if (ok != 7) {
            Serial.println("[CMD] malformed");
            goto next_packet;
          }
          if (strcmp(src, "GW") == 0) {
            lastGatewaySeen = millis();
          }
          // ===== Node nhận CMD gửi tới leader =====
          if (!isLeader && strcmp(dest, "LD") == 0) {
            if (strcmp(src, "GW") == 0) {
              Serial.println("[CMD] drop GW->LD at node");
              goto next_packet;
            }
            if (hop <= 1) {
              Serial.println("[CMD] hop exhausted");
              goto next_packet;
            }
            delay(random(10, 30));
            hop--;
            char fwd[160];
            snprintf(fwd, sizeof(fwd),
                     "C,%s,%s,%d,%u,%s,%s,%d",
                     src, dest, hop, seq, cmd_id, target, action);
            pushTx(fwd, TX_CMD);
            Serial.println("[CMD] forward -> leader");
            goto next_packet;
          }
          /* ===== Leader receive from GW ===== */
          if (strcmp(dest, "LD") == 0 && isLeader) {
            Serial.println("[CMD] GW -> LEADER");
            lastGatewaySeen = millis();
            /* ===== CMD for leader itself ===== */
            if (strcmp(target, NODE_SHORT) == 0) {
              Serial.println("[CMD] Leader executing self command");
              executeCmd(pkt);
            } else {
              Serial.println("[CMD] Leader -> node");
              forwardCmdToNode(pkt);
            }
            goto next_packet;
          }
          // ===== Node chỉ execute nếu CMD từ Leader =====
          if (!isLeader && strcmp(target, NODE_SHORT) == 0) {
            if (strcmp(src, "LD") != 0) {
              Serial.println("[CMD] ignore (not from leader)");
              goto next_packet;
            }
            Serial.println("[CMD] NODE EXECUTE");
            lastLeaderSeen = millis();
            executeCmd(pkt);
            goto next_packet;
          }
          /* ===== Forward inside mesh ===== */
          if (hop > 1 && strcmp(src, "LD") != 0) {
            hop--;
            char fwd[160];
            char* rest = strchr(pkt, ',');
            rest = strchr(rest + 1, ',');
            rest = strchr(rest + 1, ',');
            rest = strchr(rest + 1, ',');
            snprintf(
              fwd,
              sizeof(fwd),
              "C,%s,%s,%d,%s",
              src,
              dest,
              hop,
              rest + 1);
            pushTx(fwd, TX_CMD);
            Serial.println("[CMD] forward mesh");
          }
          goto next_packet;
        }

        // HEARTBEAT  (XỬ LÝ SỚM)
        else if (type == 'B') {
          char dest[4];
          if (sscanf(pkt, "B,%3s", dest) != 1) {
            Serial.println("[HB] malformed");
            goto next_packet;
          }
          if (strcmp(dest, "L") == 0) {
            if (isLeader) {
              Serial.println("[HB] GW heartbeat -> leader");
              lastGatewaySeen = millis();
              char hb[16];
              snprintf(hb, sizeof(hb), "B,A");
              pushTx(hb, TX_NORMAL);
              Serial.println("[HB] Leader broadcast mesh");
            }
          } else if (strcmp(dest, "A") == 0) {
            lastGatewaySeen = millis();
            Serial.println("[HB] Mesh heartbeat received");
          }
        }
        // HELLO MESH
        else if (strncmp(pkt, "HM,", 3) == 0) {
          char node[4];
          if (sscanf(pkt, "HM,%3[^,\r\n]", node) == 1) {
            if (strcmp(node, NODE_SHORT) != 0) {
              int rssi = LoRa.packetRssi();
              updateMesh(node, rssi);
            }
          }
        }
        // GW RSSI METRIC
        else if (type == 'R') {
          char node[4];
          int rssi;
          if (sscanf(pkt, "R,%3[^,],%d", node, &rssi) == 2) {
            Serial.printf("[METRIC][GW] node=%s rssi=%d\n", node, rssi);
            if (strcmp(node, NODE_SHORT) == 0) {
              lastGatewaySeen = millis();
              updateGatewayRSSI(NODE_SHORT, rssi);
              if (nodeState == NODE_DISCOVERY && discState == DISC_WAIT_R) {
                Serial.println("[DISCOVERY] GW RSSI received");
                discState = DISC_HELLO_MESH;
              }
            }
          }
        }
        //  NODE METRIC
        else if (type == 'M') {
          char node[4];
          int rssi;
          if (sscanf(pkt, "M,%3[^,],%d", node, &rssi) == 2) {
            if (strcmp(node, NODE_SHORT) != 0) {
              updateGatewayRSSI(node, rssi);
              updateLastSeen(node);
            }
          }
        }
        //  JOIN REPLY
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
            }
          }
        }
        //  LEADER ANNOUNCE
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
          }
        }
        // LEADER HEARTBEAT
        else if (strncmp(pkt, "HL,", 3) == 0) {
          char leader[4];
          if (sscanf(pkt, "HL,%3[^,\r\n]", leader) == 1) {
            if (strcmp(leader, leaderId) == 0) {
              lastLeaderSeen = millis();
              updateLastSeen(leaderId);
              Serial.println("[HB] Leader alive");
            }
          }
        }

        // JOIN REQUEST
        else if (strncmp(pkt, "J,", 2) == 0) {
          char node[4];
          if (sscanf(pkt, "J,%3[^,\r\n]", node) == 1) {
            if (strcmp(node, NODE_SHORT) == 0) {
              goto next_packet;
            } else if (!isLeader) {
              Serial.println("[JOIN] ignore (not leader)");
            } else {
              updateLastSeen(node);
              updateGatewayRSSI(node, -120);
              char reply[16];
              snprintf(reply,
                       sizeof(reply),
                       "JR,%s",
                       leaderId);
              pushTx(reply, TX_NORMAL);
              Serial.printf("[JOIN] accepted %s\n", node);
            }
          }
        }
        //  else if (type != 'D' && type != 'S') {
        //   Serial.println("[LORA] Unknown packet");
        // }

        /* ================= ROUTABLE PACKETS ================= */
        if (type == 'D' || type == 'S') {
          char routeSrc[4];
          char routeDest[4];
          int routeHop;
          char data[96];
          uint16_t seq;
          int ok = sscanf(pkt,
                          "%c,%3[^,],%3[^,],%d,%hu,%95[^\n]",
                          &type,
                          routeSrc,
                          routeDest,
                          &routeHop,
                          &seq,
                          data);
          if (ok < 5) {
            goto next_packet;
          }
          updateLastSeen(routeSrc);
          if (routeHop <= 0) {
            goto next_packet;
          }
          /* ===== Forward packet trong mesh ===== */
          if (!isLeader && routeHop > 0 && strcmp(routeDest, NODE_SHORT) != 0) {
            routeHop--;
            if (routeHop <= 0) {
              goto next_packet;
            }
            char fwd[128];
            snprintf(
              fwd,
              sizeof(fwd),
              "%c,%s,%s,%d,%u,%s",
              type,
              routeSrc,
              routeDest,
              routeHop,
              seq,
              data);
            pushTx(fwd, TX_NORMAL);
            // Serial.println("[ROUTE] forwarding packet");
          }
          /* ===== LEADER RECEIVE ===== */
          if (isLeader && strcmp(routeSrc, NODE_SHORT) != 0 && (strcmp(routeDest, leaderId) == 0 || strcmp(routeDest, NODE_SHORT) == 0 || strcmp(routeDest, "GW") == 0)) {
            if (type == 'D') {
              char nodeId[4];
              float t, h;
              int soil, light;
              if (sscanf(pkt,
                         "D,%3[^,],%*[^,],%*d,%*u,%f,%f,%d,%d",
                         nodeId, &t, &h, &soil, &light)
                  == 5) {
                updateLastSeen(nodeId);
                bool found = false;
                for (int i = 0; i < sensorCount; i++) {
                  if (strcmp(sensorBuf[i].node, nodeId) == 0) {
                    sensorBuf[i].t = t;
                    sensorBuf[i].h = h;
                    sensorBuf[i].soil = soil;
                    sensorBuf[i].light = light;
                    found = true;
                    // Serial.printf("[LEADER] sensor UPDATE %s\n", nodeId);
                    break;
                  }
                }
                if (!found && sensorCount < LEADER_BUF) {
                  strcpy(sensorBuf[sensorCount].node, nodeId);
                  sensorBuf[sensorCount].t = t;
                  sensorBuf[sensorCount].h = h;
                  sensorBuf[sensorCount].soil = soil;
                  sensorBuf[sensorCount].light = light;
                  sensorCount++;
                  // Serial.printf("[LEADER] sensor NEW %s (%d)\n", nodeId, sensorCount);
                }
              }
            } else if (type == 'S') {
              char nodeId[4];
              int pump, soil;
              char modeChar;
              if (sscanf(pkt,
                         "S,%3[^,],%*[^,],%*d,%*u,%d,%c,%*lu,%*d,%*d,%d",
                         nodeId, &pump, &modeChar, &soil)
                  == 4) {
                updateLastSeen(nodeId);
                bool found = false;
                for (int i = 0; i < statusCount; i++) {
                  if (strcmp(statusBuf[i].node, nodeId) == 0) {
                    statusBuf[i].pump = pump;
                    statusBuf[i].mode = modeChar;
                    statusBuf[i].soil = soil;
                    found = true;
                    // Serial.printf("[LEADER] status UPDATE %s\n", nodeId);
                    break;
                  }
                }
                if (!found && statusCount < LEADER_BUF) {
                  strcpy(statusBuf[statusCount].node, nodeId);
                  statusBuf[statusCount].pump = pump;
                  statusBuf[statusCount].mode = modeChar;
                  statusBuf[statusCount].soil = soil;
                  statusCount++;
                  // Serial.printf("[LEADER] status NEW %s (%d)\n", nodeId, statusCount);
                }
              }
            }
          }
          goto next_packet;
        }
next_packet:
        if (sep == NULL)
          break;
        start = sep + 1;
      }
    }
    if (ackWindowActive && millis() - ackWindowStart > ACK_WINDOW_TIME) {
      ackWindowActive = false;
      Serial.println("[ACK WINDOW] CLOSED");
    }

    if (isLeader && cmdHead == cmdTail) {
      if (isSensorBatchComplete()) {
        // Serial.println("[FLUSH] Sensor batch FULL -> send");
        flushSensorBatch();
      }
      if (isStatusBatchComplete()) {
        // Serial.println("[FLUSH] Status batch FULL -> send");
        flushStatusBatch();
      }
      if (millis() - lastLeaderFlush > LEADER_FLUSH_INTERVAL) {
        // Serial.println("[FLUSH] Timeout -> force send");
        flushSensorBatch();
        flushStatusBatch();
      }
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
    if (mode[0] == 'S') {
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
    if (nodeState == NODE_NETWORK_READY && cmdHead == cmdTail && millis() - lastStatusSend > STATUS_INTERVAL + random(0, 1000) && millis() - lastAckTime > STATUS_INTERVAL) {
      lastStatusSend = millis();
      unsigned long uptime = (millis() - bootTime) / 1000;
      if (isLeader) {
        bool found = false;
        for (int i = 0; i < statusCount; i++) {
          if (strcmp(statusBuf[i].node, NODE_SHORT) == 0) {
            statusBuf[i].pump = pumpStatus ? 1 : 0;
            statusBuf[i].mode = mode[0];
            statusBuf[i].soil = cachedSoil;
            found = true;
            break;
          }
        }
        if (!found && statusCount < LEADER_BUF) {
          strcpy(statusBuf[statusCount].node, NODE_SHORT);
          statusBuf[statusCount].pump = pumpStatus ? 1 : 0;
          statusBuf[statusCount].mode = mode[0];
          statusBuf[statusCount].soil = cachedSoil;
          statusCount++;
        }
      } else {
        char payload[80];
        packetSeq++;
        if (packetSeq == 0) packetSeq = 1;
        snprintf(payload, sizeof(payload),
                 "S,%s,%s,%d,%u,%d,%c,%lu,%d,%d,%d",
                 NODE_SHORT,
                 leaderId,
                 MAX_HOP,
                 packetSeq,
                 pumpStatus ? 1 : 0,
                 mode[0],
                 uptime,
                 (int)(default_amp * 10),
                 (int)(default_flow * 10),
                 (int)last_trigger_soil);
        pushTx(payload, TX_NORMAL);
        Serial.print("[SEND][STATUS] ");
        Serial.print(NODE_SHORT);
        Serial.print(" -> ");
        Serial.print(leaderId);
        Serial.print(" | ");
        Serial.println(payload);
      }
    }

    /* ================= SENSOR SEND ================= */
    if (nodeState == NODE_NETWORK_READY && cmdHead == cmdTail && millis() - lastSensorSend > SENSOR_INTERVAL + random(0, 3000) && millis() - lastAckTime > SENSOR_INTERVAL + random(0, 3000)) {
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
        bool found = false;
        for (int i = 0; i < sensorCount; i++) {
          if (strcmp(sensorBuf[i].node, NODE_SHORT) == 0) {
            sensorBuf[i].t = ema_t;
            sensorBuf[i].h = ema_h;
            sensorBuf[i].soil = cachedSoil;
            sensorBuf[i].light = (int)ema_l;
            found = true;
            break;
          }
        }
        if (!found && sensorCount < LEADER_BUF) {
          strcpy(sensorBuf[sensorCount].node, NODE_SHORT);
          sensorBuf[sensorCount].t = ema_t;
          sensorBuf[sensorCount].h = ema_h;
          sensorBuf[sensorCount].soil = cachedSoil;
          sensorBuf[sensorCount].light = (int)ema_l;
          sensorCount++;
        }
      } else {
        char payload[80];
        packetSeq++;
        if (packetSeq == 0) packetSeq = 1;
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
        pushTx(payload, TX_NORMAL);
        Serial.print("[SEND][SENSOR] ");
        Serial.print(NODE_SHORT);
        Serial.print(" -> ");
        Serial.print(leaderId);
        Serial.print(" | ");
        Serial.println(payload);
      }
    }

    if (isLeader && networkFormed && millis() - lastLeaderHB > LEADER_HB_INTERVAL) {
      char hb[16];
      snprintf(hb, sizeof(hb), "HL,%s", NODE_SHORT);
      pushTx(hb, TX_NORMAL);
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
          if (millis() - helloSentTime > 5000) {
            Serial.println("[DISCOVERY] GW reply timeout");
            discState = DISC_HELLO_GW;
          }
          break;
        case DISC_HELLO_MESH:
          sendHelloMesh();
          Serial.println("[DISCOVERY] HELLO_MESH broadcast");
          printMeshTable();
          if (meshCount > 0) {
            Serial.println("[DISCOVERY] mesh nodes detected");
          } else {
            Serial.println("[DISCOVERY] no mesh found");
          }
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
            pushTx(payload, TX_NORMAL);
            Serial.println("[DISCOVERY] sending JOIN");
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
  handleTxQueue();
}