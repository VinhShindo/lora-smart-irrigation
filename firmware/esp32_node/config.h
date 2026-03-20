#ifndef CONFIG_H
#define CONFIG_H

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

// NODE IDENTIFICATION
const char NODE_ID[] = "NODE_03";
const char NODE_SHORT[] = "03";

// INTERVAL CONFIG
const unsigned long STATUS_INTERVAL = 10000;
const unsigned long SENSOR_INTERVAL = 25000;
const unsigned long HEARTBEAT_TIMEOUT = 75000;
const unsigned long SOIL_INTERVAL = 2000;
const unsigned long HELLO_INTERVAL = 10000;
const unsigned long DISCOVERY_WINDOW = 30000;

// LORA CONFIG
const int LORA_RETRY_MAX = 5;

// COMMAND LOCK
const unsigned long CMD_LOCK_TIME = 1500;

// MESH CONFIG
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

// GLOBAL STATE
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

// SENSOR CACHE
int cachedSoil = 0;

#endif