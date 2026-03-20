#ifndef COMMAND_HANDLER_H
#define COMMAND_HANDLER_H

#include "config.h"
#include "lora_tx_rx.h"

void executeCmd(const char* pkt) {
  char target[4];
  int action;

  if (sscanf(pkt, "C,%*[^,],%*[^,],%*d,%*d,%*[^,],%3[^,],%d", target, &action) != 2)
    return;

  if (strcmp(target, NODE_SHORT) != 0) return;

  if (action == 1) {
    pumpStatus = true;
    strcpy(mode, "C");
  } else if (action == 0) {
    pumpStatus = false;
    strcpy(mode, "C");
  }

  digitalWrite(PIN_RELAY, pumpStatus ? LOW : HIGH);

  char ack[64];
  snprintf(ack, sizeof(ack),
           "A,%s,LD,3,1,cmd,%s,1",
           NODE_SHORT,
           NODE_SHORT);

  pushTx(ack, TX_ACK);
}

#endif