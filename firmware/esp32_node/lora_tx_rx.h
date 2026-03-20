#ifndef LORA_TX_RX_H
#define LORA_TX_RX_H

#include "config.h"

enum TxType {
  TX_NORMAL,
  TX_CMD,
  TX_ACK
};

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

// ===== INIT =====
void initLoRa() {
  LoRa.setPins(PIN_LORA_SS, PIN_LORA_RST, PIN_LORA_DIO0);

  for (int i = 1; i <= LORA_RETRY_MAX; i++) {
    if (LoRa.begin(433E6)) {
      LoRa.setSyncWord(0xA5);
      LoRa.enableCrc();
      LoRa.receive();
      loraReady = true;
      return;
    }
    delay(1000);
  }
  loraReady = false;
}

// ===== TX QUEUE =====
bool pushTx(const char* data, TxType type) {
  if (!loraReady) return false;

  int next = (normTail + 1) % NORMAL_QUEUE_SIZE;
  if (next == normHead) {
    normHead = (normHead + 1) % NORMAL_QUEUE_SIZE;
  }

  strncpy(normalQueue[normTail].data, data, sizeof(normalQueue[normTail].data) - 1);
  normalQueue[normTail].type = type;
  normTail = next;

  return true;
}

void handleTxQueue() {
  if (!loraReady) return;

  if (normHead == normTail) return;

  TxItem item = normalQueue[normHead];
  normHead = (normHead + 1) % NORMAL_QUEUE_SIZE;

  LoRa.beginPacket();
  LoRa.print(item.data);
  LoRa.endPacket();
  LoRa.receive();
}

#endif