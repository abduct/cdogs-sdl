/*
	Vita network stack lifecycle for C-Dogs SDL.
	Isolates SCE_SYSMODULE_NET / sceNet* / sceNetCtl* from generic platforms.
*/
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bring up Vita networking (sysmodule + sceNetInit + sceNetCtlInit).
 * Safe to call once. On failure, logs and returns false; the game may
 * continue offline — do not treat as fatal.
 */
bool VitaNetInit(void);

/**
 * Tear down Vita networking. Safe if init failed or was never called;
 * only reverses steps that succeeded.
 */
void VitaNetTerm(void);

#ifdef __cplusplus
}
#endif
