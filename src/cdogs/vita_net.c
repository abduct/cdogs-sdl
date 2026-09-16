/*
	Vita network stack lifecycle for C-Dogs SDL.

	Order follows VitaSDK samples (net_http / socket_ping):
	  load SCE_SYSMODULE_NET -> sceNetInit -> sceNetCtlInit
	  sceNetCtlTerm -> sceNetTerm -> free pool -> unload module
*/
#include "vita_net.h"

#include <stdlib.h>
#include <string.h>

#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include "log.h"

/* VitaSDK network samples use a 1 MiB pool for SceNetInitParam.memory. */
#define VITA_NET_POOL_SIZE (1 * 1024 * 1024)

/*
 * Keep RX LOAD segment from ending with too little room before RW for
 * vita-elf-create SCE metadata (needs ~3KiB gap). Crossing into the next
 * page forces data to 0x...f0000 and restores a large gap.
 */
__attribute__((used, section(".rodata"))) static const char
	s_vitaSegGapPad[2048];

static bool s_moduleLoaded;
static bool s_netInited;
static bool s_netCtlInited;
static void *s_netMemory;

bool VitaNetInit(void)
{
	int res;

	if (s_netCtlInited)
	{
		return true;
	}

	LOG(LM_NET, LL_INFO, "Vita: loading SCE_SYSMODULE_NET...");
	res = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
	if (res < 0)
	{
		LOG(LM_NET, LL_ERROR, "Vita: sceSysmoduleLoadModule(NET) failed (0x%08X)",
			(unsigned)res);
		return false;
	}
	s_moduleLoaded = true;
	LOG(LM_NET, LL_INFO, "Vita: SCE_SYSMODULE_NET loaded");

	s_netMemory = malloc(VITA_NET_POOL_SIZE);
	if (s_netMemory == NULL)
	{
		LOG(LM_NET, LL_ERROR, "Vita: failed to allocate %d-byte net memory pool",
			VITA_NET_POOL_SIZE);
		VitaNetTerm();
		return false;
	}
	memset(s_netMemory, 0, VITA_NET_POOL_SIZE);

	SceNetInitParam param;
	param.memory = s_netMemory;
	param.size = VITA_NET_POOL_SIZE;
	param.flags = 0;

	LOG(LM_NET, LL_INFO, "Vita: sceNetInit (pool %d bytes)...", VITA_NET_POOL_SIZE);
	res = sceNetInit(&param);
	if (res < 0)
	{
		LOG(LM_NET, LL_ERROR, "Vita: sceNetInit failed (0x%08X)", (unsigned)res);
		VitaNetTerm();
		return false;
	}
	s_netInited = true;
	LOG(LM_NET, LL_INFO, "Vita: sceNetInit OK");

	LOG(LM_NET, LL_INFO, "Vita: sceNetCtlInit...");
	res = sceNetCtlInit();
	if (res < 0)
	{
		LOG(LM_NET, LL_ERROR, "Vita: sceNetCtlInit failed (0x%08X)", (unsigned)res);
		VitaNetTerm();
		return false;
	}
	s_netCtlInited = true;
	LOG(LM_NET, LL_INFO, "Vita: sceNetCtlInit OK");
	LOG(LM_NET, LL_INFO, "Vita networking ready");

	return true;
}

void VitaNetTerm(void)
{
	if (s_netCtlInited)
	{
		LOG(LM_NET, LL_INFO, "Vita: sceNetCtlTerm");
		sceNetCtlTerm();
		s_netCtlInited = false;
	}

	if (s_netInited)
	{
		LOG(LM_NET, LL_INFO, "Vita: sceNetTerm");
		sceNetTerm();
		s_netInited = false;
	}

	if (s_netMemory != NULL)
	{
		free(s_netMemory);
		s_netMemory = NULL;
	}

	if (s_moduleLoaded)
	{
		LOG(LM_NET, LL_INFO, "Vita: unloading SCE_SYSMODULE_NET");
		sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
		s_moduleLoaded = false;
	}
}
