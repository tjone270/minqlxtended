// rcon_send.h - a send timeout on the engine's zmq rcon socket. See rcon_send.c.

#ifndef RCON_SEND_H
#define RCON_SEND_H

#include "server/maps_parser.h"

// Finds the rcon socket global and libzmq's zmq_setsockopt inside qzeroded. Called once
// from SearchFunctions. A miss only warns: the server then runs with the engine's own
// unbounded send, as it always has.
void RconSend_Resolve(module_info_t* module);

// Game thread only, once a frame. Applies sv_rconSendTimeout to the rcon socket whenever the
// socket or the cvar changed since it last did; otherwise a couple of compares.
void RconSend_Frame(void);

#endif /* RCON_SEND_H */
