#include "config.h"
#include "mesh.h"
#include "lora_tx_rx.h"
#include "command_handler.h"

// SENSOR
void processSensors() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (!isnan(t)) ema_t = alpha * t + (1 - alpha) * ema_t;
  if (!isnan(h)) ema_h = alpha * h + (1 - alpha) * ema_h;

  float l = map(analogRead(PIN_LDR), 0, 4095, 0, 100);
  ema_l = alpha * l + (1 - alpha) * ema_l;
}

int readSoil() {
  return map(analogRead(PIN_SOIL), 0, 4095, 0, 100);
}

void setup() {
  Serial.begin(115200);

  dht.begin();
  pinMode(PIN_RELAY, OUTPUT);

  initLoRa();

  nodeState = NODE_DISCOVERY;
  discoveryStart = millis();
}

void loop() {

  // RX
  int size = LoRa.parsePacket();
  if (size) {
    char msg[128];
    int i = 0;

    while (LoRa.available()) {
      msg[i++] = (char)LoRa.read();
    }
    msg[i] = '\0';

    if (msg[0] == 'C') {
      executeCmd(msg);
    }
  }

  // SENSOR UPDATE
  processSensors();

  if (millis() - lastSoilRead > SOIL_INTERVAL) {
    cachedSoil = readSoil();
    lastSoilRead = millis();
  }

  // SEND SENSOR
  if (millis() - lastSensorSend > SENSOR_INTERVAL) {
    lastSensorSend = millis();

    char payload[80];
    snprintf(payload, sizeof(payload),
             "D,%s,LD,3,1,%.1f,%.1f,%d,%d",
             NODE_SHORT,
             ema_t,
             ema_h,
             cachedSoil,
             (int)ema_l);

    pushTx(payload, TX_NORMAL);
  }

  handleTxQueue();
}