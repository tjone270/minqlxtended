#ifndef DEMOS_H
#define DEMOS_H

#include "quake_common.h"

void Demo_Init(void);                            // register + cache cvars
void Demo_Capture(msg_t *msg, client_t *client); // called for every S2C message
void Demo_ClientDisconnect(int slot);            // finalise a client's demo
void Demo_CloseAll(void);                        // finalise every open demo

#endif /* DEMOS_H */
