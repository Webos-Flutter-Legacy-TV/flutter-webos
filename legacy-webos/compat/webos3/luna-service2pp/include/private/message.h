// Stand-in for luna-service2's private message.h (legacy-webos, webOS 3
// build): only what libluna-service2++'s subscription.cpp uses. Both functions
// are exported by the firmware's libluna-service2 / libPmLogLib.
#pragma once

#include <PmLogLib.h>
#include <luna-service2/lunaservice.h>

#ifdef __cplusplus
extern "C" {
#endif
bool LSMessageIsConnected(LSMessage *msg);
#ifdef __cplusplus
}
#endif

#define MSGID_LS_CANT_CANCEL_METH "LS_CANC_METH" /** Can't cancel method */
