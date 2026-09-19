/*
	Viewport-sized static floor cache + wall ROW ATLAS.
	Walls are cached per logical tile row so DrawTiles can blit one row,
	then doors + mid Things, preserving original north→south painter order.
	Fog/LOS applied dynamically (not baked).
*/
#include "terrain_cache.h"

#ifdef CDOGS_TERRAIN_CACHE

#include <string.h>

#include "config.h"
#include "color.h"
#include "draw/draw.h"
#include "draw/draw_buffer.h"
#include "grafx.h"
#include "log.h"
#include "texture.h"
#include "tile_class.h"
#include "utils.h"
#define WALL_BLEED (-WALL_OFFSET_Y)
/* Band tall enough for WALL_OFFSET_Y overhang + typical wall pics (≤48px). */
#define WALL_ROW_BAND_H (WALL_BLEED + 48)

typedef struct
{
	SDL_Texture *floorTex;
	SDL_Texture *wallTex; /* packed logical-row bands */
	int texW;
	int texH;	  /* floor height */
	int wallTexH; /* Y_TILES * WALL_ROW_BAND_H */
	int rowBandH;
	int xStart;
	int yStart;
	int sizeX;
	int sizeY;
	bool valid;
	bool dirty;
} TerrainCacheState;

static TerrainCacheState s_tc;

void TerrainCacheInit(void)
{
	memset(&s_tc, 0, sizeof s_tc);
	s_tc.dirty = true;
	s_tc.rowBandH = WALL_ROW_BAND_H;
}

void TerrainCacheTerminate(void)
{
	if (s_tc.floorTex != NULL)
	{
		SDL_DestroyTexture(s_tc.floorTex);
		s_tc.floorTex = NULL;
	}
	if (s_tc.wallTex != NULL)
	{
		SDL_DestroyTexture(s_tc.wallTex);
		s_tc.wallTex = NULL;
	}
	memset(&s_tc, 0, sizeof s_tc);
	s_tc.rowBandH = WALL_ROW_BAND_H;
}

void TerrainCacheInvalidate(void)
{
	s_tc.dirty = true;
	s_tc.valid = false;
}

typedef enum
{
	CACHE_LOS_NORMAL = 0,
	CACHE_LOS_FOG,
	CACHE_LOS_NONE
} CacheTileLOS;

static CacheTileLOS CacheLOS(const Tile *tile, const bool useFog)
{
	if (!tile->isVisited)
	{
		return CACHE_LOS_NONE;
	}
	if (tile->outOfSight)
	{
		return useFog ? CACHE_LOS_FOG : CACHE_LOS_NONE;
	}
	return CACHE_LOS_NORMAL;
}

static bool EnsureTextures(const int w, const int floorH, const int wallH)
{
	SDL_Renderer *r = gGraphicsDevice.gameWindow.renderer;
	if (r == NULL)
	{
		return false;
	}
	if (s_tc.floorTex != NULL && s_tc.texW == w && s_tc.texH == floorH &&
		s_tc.wallTex != NULL && s_tc.wallTexH == wallH &&
		s_tc.rowBandH == WALL_ROW_BAND_H)
	{
		return true;
	}
	if (s_tc.floorTex != NULL)
	{
		SDL_DestroyTexture(s_tc.floorTex);
		s_tc.floorTex = NULL;
	}
	if (s_tc.wallTex != NULL)
	{
		SDL_DestroyTexture(s_tc.wallTex);
		s_tc.wallTex = NULL;
	}
	s_tc.floorTex = TextureCreate(
		r, SDL_TEXTUREACCESS_TARGET, svec2i(w, floorH), SDL_BLENDMODE_BLEND,
		255);
	s_tc.wallTex = TextureCreate(
		r, SDL_TEXTUREACCESS_TARGET, svec2i(w, wallH), SDL_BLENDMODE_BLEND,
		255);
	if (s_tc.floorTex == NULL || s_tc.wallTex == NULL)
	{
		LOG(LM_GFX, LL_ERROR, "terrain cache: texture create failed");
		TerrainCacheTerminate();
		s_tc.dirty = true;
		return false;
	}
	s_tc.texW = w;
	s_tc.texH = floorH;
	s_tc.wallTexH = wallH;
	s_tc.rowBandH = WALL_ROW_BAND_H;
	s_tc.dirty = true;
	return true;
}

static bool NeedsRebuild(const DrawBuffer *b)
{
	return !s_tc.valid || s_tc.dirty || s_tc.xStart != b->xStart ||
		   s_tc.yStart != b->yStart || s_tc.sizeX != b->Size.x;
}

static void Rebuild(DrawBuffer *b)
{
	SDL_Renderer *r = gGraphicsDevice.gameWindow.renderer;
	const int w = b->Size.x * TILE_WIDTH;
	const int floorH = Y_TILES * TILE_HEIGHT;
	const int wallH = Y_TILES * WALL_ROW_BAND_H;
	if (!EnsureTextures(w, floorH, wallH))
	{
		s_tc.valid = false;
		return;
	}

	TextureFlush();
	SDL_Texture *prev = SDL_GetRenderTarget(r);

	/* Floor layer — screen-space layout. */
	if (SDL_SetRenderTarget(r, s_tc.floorTex) != 0)
	{
		LOG(LM_GFX, LL_ERROR, "terrain cache floor target: %s", SDL_GetError());
		s_tc.valid = false;
		return;
	}
	SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
	SDL_RenderClear(r);

	const Tile **tile = DrawBufferGetFirstTile(b);
	struct vec2i pos;
	int x, y;
	for (y = 0, pos.y = 0; y < Y_TILES; y++, pos.y += TILE_HEIGHT)
	{
		for (x = 0, pos.x = 0; x < b->Size.x; x++, tile++, pos.x += TILE_WIDTH)
		{
			if (*tile == NULL)
			{
				continue;
			}
			const Tile *t = *tile;
			if (t->Class != NULL && t->Class->Pic != NULL &&
				t->Class->Pic->Data != NULL &&
				t->Class->Type != TILE_CLASS_WALL)
			{
				PicRender(
					t->Class->Pic, r, pos, colorWhite, 0, svec2_one(),
					SDL_FLIP_NONE, Rect2iZero());
			}
		}
		tile += X_TILES - b->Size.x;
	}
	TextureFlush();
	/*
	 * Wall ROW ATLAS: each logical tile row owns a band of height
	 * WALL_ROW_BAND_H. Only that row's TILE_CLASS_WALL Pics are drawn into
	 * the band (with WALL_OFFSET_Y), so bands do not mix logical rows even
	 * when sprites overhang in screen space.
	 */
	if (SDL_SetRenderTarget(r, s_tc.wallTex) != 0)
	{
		LOG(LM_GFX, LL_ERROR, "terrain cache wall target: %s", SDL_GetError());
		SDL_SetRenderTarget(r, prev);
		s_tc.valid = false;
		return;
	}
	SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
	SDL_RenderClear(r);

	tile = DrawBufferGetFirstTile(b);
	for (y = 0; y < Y_TILES; y++)
	{
		const int bandY0 = y * WALL_ROW_BAND_H;
		const int tileBaseY = bandY0 + WALL_BLEED;
		for (x = 0, pos.x = 0; x < b->Size.x; x++, tile++, pos.x += TILE_WIDTH)
		{
			if (*tile == NULL)
			{
				continue;
			}
			const Tile *t = *tile;
			if (t->Class != NULL && t->Class->Type == TILE_CLASS_WALL &&
				t->Class->Pic != NULL && t->Class->Pic->Data != NULL)
			{
				PicRender(
					t->Class->Pic, r,
					svec2i(pos.x, tileBaseY + WALL_OFFSET_Y), colorWhite, 0,
					svec2_one(), SDL_FLIP_NONE, Rect2iZero());
			}
		}
		tile += X_TILES - b->Size.x;
	}
	TextureFlush();
	SDL_SetRenderTarget(r, prev);

	s_tc.xStart = b->xStart;
	s_tc.yStart = b->yStart;
	s_tc.sizeX = b->Size.x;
	s_tc.sizeY = Y_TILES;
	s_tc.valid = true;
	s_tc.dirty = false;
}

static void ApplyFloorLOSOverlays(DrawBuffer *b, const struct vec2i offset)
{
	const bool useFog = ConfigGetBool(&gConfig, "Game.Fog");
	SDL_Renderer *r = gGraphicsDevice.gameWindow.renderer;
	const Tile **tile = DrawBufferGetFirstTile(b);
	struct vec2i pos;
	int x, y;
	for (y = 0, pos.y = b->dy + offset.y; y < Y_TILES;
		 y++, pos.y += TILE_HEIGHT)
	{
		for (x = 0, pos.x = b->dx + offset.x; x < b->Size.x;
			 x++, tile++, pos.x += TILE_WIDTH)
		{
			if (*tile == NULL)
			{
				continue;
			}
			const Tile *t = *tile;
			if (t->Class != NULL && t->Class->Type == TILE_CLASS_WALL)
			{
				continue;
			}
			const CacheTileLOS los = CacheLOS(t, useFog);
			TextureFlush();
			if (los == CACHE_LOS_NONE)
			{
				SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
				SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
				const SDL_Rect rect = {pos.x, pos.y, TILE_WIDTH, TILE_HEIGHT};
				SDL_RenderFillRect(r, &rect);
			}
			else if (los == CACHE_LOS_FOG)
			{
				SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_MOD);
				SDL_SetRenderDrawColor(
					r, colorFog.r, colorFog.g, colorFog.b, 255);
				const SDL_Rect rect = {pos.x, pos.y, TILE_WIDTH, TILE_HEIGHT};
				SDL_RenderFillRect(r, &rect);
				SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
			}
		}
		tile += X_TILES - b->Size.x;
	}
}

static void ApplyWallRowLOSOverlays(
	DrawBuffer *b, const struct vec2i offset, const int row,
	const int tileScreenY)
{
	const bool useFog = ConfigGetBool(&gConfig, "Game.Fog");
	SDL_Renderer *r = gGraphicsDevice.gameWindow.renderer;
	const Tile **tile = DrawBufferGetFirstTile(b);
	tile += row * X_TILES;
	struct vec2i pos;
	pos.y = tileScreenY;
	int x;
	for (x = 0, pos.x = b->dx + offset.x; x < b->Size.x;
		 x++, tile++, pos.x += TILE_WIDTH)
	{
		if (*tile == NULL)
		{
			continue;
		}
		const Tile *t = *tile;
		if (t->Class == NULL || t->Class->Type != TILE_CLASS_WALL)
		{
			continue;
		}
		const CacheTileLOS los = CacheLOS(t, useFog);
		const struct vec2i rpos = svec2i_add(pos, svec2i(0, WALL_OFFSET_Y));
		const struct vec2i rsize =
			svec2i(TILE_WIDTH, TILE_HEIGHT - WALL_OFFSET_Y);
		TextureFlush();
		if (los == CACHE_LOS_NONE)
		{
			SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
			SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
			const SDL_Rect rect = {rpos.x, rpos.y, rsize.x, rsize.y};
			SDL_RenderFillRect(r, &rect);
		}
		else if (los == CACHE_LOS_FOG)
		{
			SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_MOD);
			SDL_SetRenderDrawColor(r, colorFog.r, colorFog.g, colorFog.b, 255);
			const SDL_Rect rect = {rpos.x, rpos.y, rsize.x, rsize.y};
			SDL_RenderFillRect(r, &rect);
			SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
		}
	}
}

bool TerrainCacheDrawFloors(DrawBuffer *b, const struct vec2i offset)
{
	if (b == NULL)
	{
		return false;
	}
	if (NeedsRebuild(b))
	{
		Rebuild(b);
	}
	if (!s_tc.valid || s_tc.floorTex == NULL)
	{
		return false;
	}

	const struct vec2i pos = svec2i(offset.x + b->dx, offset.y + b->dy);
	TextureRender(
		s_tc.floorTex, gGraphicsDevice.gameWindow.renderer, Rect2iZero(),
		Rect2iNew(pos, svec2i(s_tc.texW, s_tc.texH)), colorWhite, 0,
		SDL_FLIP_NONE);
	ApplyFloorLOSOverlays(b, offset);
	return true;
}

bool TerrainCachePrepareWalls(DrawBuffer *b)
{
	if (b == NULL)
	{
		return false;
	}
	if (NeedsRebuild(b))
	{
		Rebuild(b);
	}
	return s_tc.valid && s_tc.wallTex != NULL;
}

bool TerrainCacheBlitWallRow(
	DrawBuffer *b, const struct vec2i offset, const int row,
	const int tileScreenY)
{
	if (!s_tc.valid || s_tc.wallTex == NULL || row < 0 || row >= s_tc.sizeY)
	{
		return false;
	}

	const Rect2i src = Rect2iNew(
		svec2i(0, row * s_tc.rowBandH), svec2i(s_tc.texW, s_tc.rowBandH));
	const struct vec2i destPos =
		svec2i(offset.x + b->dx, tileScreenY - WALL_BLEED);
	TextureRender(
		s_tc.wallTex, gGraphicsDevice.gameWindow.renderer, src,
		Rect2iNew(destPos, svec2i(s_tc.texW, s_tc.rowBandH)), colorWhite, 0,
		SDL_FLIP_NONE);
	ApplyWallRowLOSOverlays(b, offset, row, tileScreenY);
	return true;
}

#endif /* CDOGS_TERRAIN_CACHE */
