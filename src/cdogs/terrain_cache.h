/*
	Viewport-sized static floor cache + wall ROW ATLAS.
	Caches unfogged terrain; applies LOS/fog/visited dynamically each frame.
	Doors and Things remain live; walls composite per logical tile row.
*/
#pragma once

#include "draw/draw_buffer.h"
#include "vector.h"

#ifdef CDOGS_TERRAIN_CACHE

void TerrainCacheInit(void);
void TerrainCacheTerminate(void);
void TerrainCacheInvalidate(void);

/* Draw cached floors; apply LOS overlays. True => skip per-tile floors. */
bool TerrainCacheDrawFloors(DrawBuffer *b, const struct vec2i offset);

/* Ensure wall-row atlas is rebuilt/valid for this buffer. */
bool TerrainCachePrepareWalls(DrawBuffer *b);

/*
 * Blit one logical wall row from the row atlas at the given tile-row screen Y,
 * then apply LOS overlays for walls in that row only.
 */
bool TerrainCacheBlitWallRow(
	DrawBuffer *b, const struct vec2i offset, const int row,
	const int tileScreenY);

#else

#define TerrainCacheInit() ((void)0)
#define TerrainCacheTerminate() ((void)0)
#define TerrainCacheInvalidate() ((void)0)
#define TerrainCacheDrawFloors(b, offset) (false)
#define TerrainCachePrepareWalls(b) (false)
#define TerrainCacheBlitWallRow(b, offset, row, tileScreenY) (false)

#endif
