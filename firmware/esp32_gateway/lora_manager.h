#ifndef LORA_MANAGER_H
#define LORA_MANAGER_H

#include <LoRa.h>
#include "config.h"

void initLoRa();
void sendLoRa(const char* msg);
void loraRxTask(void* p);
void cmdTask(void* p);

// ===== IMPLEMENT =====

void initLoRa() {
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  for (int i = 0; i < LORA_RETRY_MAX; i++) {
    if (LoRa.begin(433E6)) {
      LoRa.receive();
      loraReady = true;
      return;
    }
    delay(1000);
  }
  loraReady = false;
}

void sendLoRa(const char* msg) {
  if (!loraReady) return;

  LoRa.beginPacket();
  LoRa.print(msg);
  LoRa.endPacket();
  LoRa.receive();
}

void cmdTask(void* p) {
  CmdItem item;

  for (;;) {
    if (xQueueReceive(cmdQueue, &item, portMAX_DELAY)) {

      char msg[128];
      sprintf(msg, "C,GW,LD,3,%lu,%s", millis(), item.node);

      sendLoRa(msg);

      waitingAck = true;
      ackTimeout = millis() + 3000;
    }
  }
}

void loraRxTask(void* p) {
  for (;;) {
    int size = LoRa.parsePacket();
    if (!size) continue;

    char buf[256];
    int i = 0;
    while (LoRa.available()) buf[i++] = LoRa.read();
    buf[i] = 0;

    Serial.println(buf);

    if (strncmp(buf, "A,", 2) == 0) {
      waitingAck = false;
    }
  }
}

#endif