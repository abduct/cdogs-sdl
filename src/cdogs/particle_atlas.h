/*
	Runtime particle texture atlas. Pics under particles/ (and custom
	equivalents) share one or more SDL_Textures with per-Pic source rects.
	Owned by this module; Pics set ownsTex=false and must not destroy Tex.
*/
#pragma once

#include "pic_manager.h"

#ifdef CDOGS_PARTICLE_ATLAS

void ParticleAtlasBuild(PicManager *pm);
void ParticleAtlasDestroy(void);

#else

#define ParticleAtlasBuild(pm) ((void)0)
#define ParticleAtlasDestroy() ((void)0)

#endif
