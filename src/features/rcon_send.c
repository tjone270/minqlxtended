// rcon_send.c - a send timeout on the engine's zmq rcon socket.
//
// The engine sends every console line to every rcon client it knows: Com_Printf calls a
// function in id_zmq.cpp (PTRN_RCON_BROADCAST) that walks its client list and, per client,
// sends the identity frame and then the text through czmq on a ROUTER socket with
// ZMQ_ROUTER_MANDATORY set. When the identity send fails it drops the client and prints
// "zmq RCON client disconnected". That send is blocking and the socket has no send timeout,
// so if libzmq answers EAGAIN for a client (its pipe is full) the game thread waits until
// that pipe drains. For a client that went away, it never does: the server stops dead with
// the process alive, and only a restart brings it back. Seen on 2026-10-25 with a client
// that disconnected while the game was printing; the stack was My_Com_Printf -> Com_Printf
// -> the broadcast -> zmq_msg_send -> poll(timeout=-1), the client's TCP connection already
// gone and its identity the only one left in the engine's list.
//
// ZMQ_SNDTIMEO on the socket bounds that wait. After it the send fails with EAGAIN, and the
// engine's own path takes over: the client is dropped from the list, the disconnect line is
// printed, and the next line goes to the clients still there. Nothing of the engine is
// patched; only one socket option changes.
//
// Neither czmq nor libzmq has symbols in qzeroded. The socket comes from the global the
// broadcast loads first (a zsock_t*, whose libzmq handle sits after the czmq tag), and
// zmq_setsockopt from the call inside czmq's zsock_set_router_mandatory. Both are only read
// here, never hooked. The pointer stays static on purpose: pyzmq's libzmq is loaded into the
// same process and exports zmq_setsockopt, and a global of ours by that name could shadow it.
//
// The option is applied from the game thread, the one thread that uses the socket, and again
// whenever the engine replaces the socket or the cvar changes.

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "common.h"
#include "engine/patterns.h"
#include "engine/quake_common.h"
#include "rcon_send.h"

// libzmq's option number, from zmq.h (unchanged since libzmq 3).
#define RCON_ZMQ_SNDTIMEO 28
// czmq's tag at the top of every zsock_t, followed by the libzmq socket handle.
#define RCON_ZSOCK_TAG 0x0004cafe

typedef struct {
    uint32_t tag;
    void* handle;
} rcon_zsock_head_t;

typedef int (*rcon_zmq_setsockopt_ptr)(void* socket, int option, const void* value, size_t len);

static rcon_zsock_head_t** rcon_zsock;  // the engine's global; NULL while unresolved
static rcon_zmq_setsockopt_ptr rcon_zmq_setsockopt;

static cvar_t* sv_rconSendTimeout;
static void* applied_handle;
static int applied_count = -1;

void RconSend_Resolve(module_info_t* module) {
    rcon_zsock          = NULL;
    rcon_zmq_setsockopt = NULL;

    pint broadcast = (pint)PatternSearchModule(module, PTRN_RCON_BROADCAST, MASK_RCON_BROADCAST);
    pint setter    = (pint)PatternSearchModule(module, PTRN_ZSOCK_SET_ROUTER_MANDATORY, MASK_ZSOCK_SET_ROUTER_MANDATORY);
    if (!broadcast || !setter) {
        DebugPrint("WARNING: Unable to find the zmq rcon socket. sv_rconSendTimeout will have no effect.\n");
        return;
    }

    pint relp_zsock = broadcast + RELOFFSET_RELP_RCON_ZSOCK;
    pint zsock      = *(int32_t*)relp_zsock + relp_zsock + 4;
    pint relp_set   = setter + RELOFFSET_RELP_ZMQ_SETSOCKOPT;
    pint set        = *(int32_t*)relp_set + relp_set + 4;
    if (!InImage(zsock) || !InImage(set)) {
        DebugPrint("WARNING: The zmq rcon socket resolved outside the image. sv_rconSendTimeout will have no effect.\n");
        return;
    }

    rcon_zsock          = (rcon_zsock_head_t**)zsock;
    rcon_zmq_setsockopt = (rcon_zmq_setsockopt_ptr)set;
    DebugPrint("rcon zsock: %p, zmq_setsockopt: %p\n", (void*)rcon_zsock, (void*)rcon_zmq_setsockopt);
}

void RconSend_Frame(void) {
    if (!rcon_zsock) {
        return;
    }
    // Milliseconds the game thread may wait on one rcon client before the engine drops it.
    // -1 is the engine's own behaviour (wait for ever), 0 drops a client the moment its
    // queue is full.
    if (!sv_rconSendTimeout) {
        sv_rconSendTimeout = Cvar_Get("sv_rconSendTimeout", "250", CVAR_ARCHIVE);
    }

    // NULL with zmq_rcon_enable 0, or between the engine closing the socket and opening the next.
    rcon_zsock_head_t* zsock = *rcon_zsock;
    void* handle             = (zsock && zsock->tag == RCON_ZSOCK_TAG) ? zsock->handle : NULL;
    if (!handle) {
        applied_handle = NULL;
        return;
    }
    if (handle == applied_handle && sv_rconSendTimeout->modificationCount == applied_count) {
        return;
    }

    int timeout = sv_rconSendTimeout->integer < -1 ? -1 : sv_rconSendTimeout->integer;
    if (rcon_zmq_setsockopt(handle, RCON_ZMQ_SNDTIMEO, &timeout, sizeof(timeout)) == 0) {
        DebugPrint("rcon: send timeout %d ms\n", timeout);
    } else {
        DebugPrint("WARNING: rcon: setting the send timeout failed: %s\n", strerror(errno));
    }
    // Recorded either way, so a failure is reported once and not every frame.
    applied_handle = handle;
    applied_count  = sv_rconSendTimeout->modificationCount;
}
