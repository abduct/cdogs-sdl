/*
	C-Dogs SDL
	A port of the legendary (and fun) action/arcade cdogs.
	Copyright (C) 1995 Ronny Wester
	Copyright (C) 2003 Jeremy Chin
	Copyright (C) 2003 Lucas Martin-King

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

	This file incorporates work covered by the following copyright and
	permission notice:

	Copyright (c) 2013-2014, 2016, 2018-2020, 2024 Cong Xu
	All rights reserved.

	Redistribution and use in source and binary forms, with or without
	modification, are permitted provided that the following conditions are met:

	Redistributions of source code must retain the above copyright notice, this
	list of conditions and the following disclaimer.
	Redistributions in binary form must reproduce the above copyright notice,
	this list of conditions and the following disclaimer in the documentation
	and/or other materials provided with the distribution.

	THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
	AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
	IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
	ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
	LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
	CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
	SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
	INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
	CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
	ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
	POSSIBILITY OF SUCH DAMAGE.
*/
#include "automap.h"
#include "vita_profile.h"

#include <stdio.h>
#include <string.h>

#include "actors.h"
#include "config.h"
#include "draw/draw.h"
#include "draw/draw_actor.h"
#include "draw/drawtools.h"
#include "font.h"
#include "gamedata.h"
#include "map.h"
#include "mission.h"
#include "objs.h"
#include "pic_manager.h"
#include "pickup.h"

#define MAP_SCALE_DEFAULT 2
#define MASK_ALPHA 128;

color_t colorWall = {72, 152, 72, 255};
color_t colorFloor = {12, 92, 12, 255};
color_t colorRoom = {24, 112, 24, 255};
color_t colorExit = {255, 255, 255, 255};

/* 1 while inside AutomapDrawRegion (HUD radar); counters only then. */
static int s_automapHudRadar;

static void DisplayPlayer(
	SDL_Renderer *renderer, const TActor *player, struct vec2i pos,
	const int scale)
{
	const struct vec2i playerPos = Vec2ToTile(player->thing.Pos);
	pos = svec2i_add(pos, svec2i_scale(playerPos, (float)scale));
	if (scale >= 2)
	{
		DrawHead(renderer, ActorGetCharacter(player), DIRECTION_DOWN, pos);
	}
	else
	{
		DrawPoint(pos, colorWhite);
	}
}

static void DisplayObjective(
	Thing *t, int objectiveIndex, struct vec2i pos, int scale, int flags)
{
	const struct vec2i objectivePos = Vec2ToTile(t->Pos);
	const Objective *o =
		CArrayGet(&gMission.missionData->Objectives, objectiveIndex);
	color_t color = o->color;
	pos = svec2i_add(pos, svec2i_scale(objectivePos, (float)scale));
	if (flags & AUTOMAP_FLAGS_MASK)
	{
		color.a = MASK_ALPHA;
	}
	if (scale >= 2)
	{
		DrawCross(&gGraphicsDevice, pos, color);
	}
	else
	{
		DrawPoint(pos, color);
	}
}

static void DisplayExits(
	const Map *m, const struct vec2i pos, const int scale, const int flags)
{
	CA_FOREACH(const Exit, e, m->exits)
	if (e->Hidden)
		continue;
	if (s_automapHudRadar)
	{
		VitaProfileDrawCount(VITA_DRAW_CNT_RADAR_EXIT_MARKERS, 1);
	}
	const struct vec2i exitPos =
		svec2i_add(svec2i_scale(e->R.Pos, (float)scale), pos);
	const struct vec2i exitSize = svec2i_scale(e->R.Size, (float)scale);

	color_t color = colorExit;
	if (flags & AUTOMAP_FLAGS_MASK)
	{
		color.a = MASK_ALPHA;
	}
	DrawRectangle(&gGraphicsDevice, exitPos, exitSize, color, false);
	CA_FOREACH_END()
}

static void DisplaySummary(void)
{
	char sScore[20];
	struct vec2i pos;
	pos.y = gGraphicsDevice.cachedConfig.Res.y - 5 - FontH();

	CA_FOREACH(const Objective, o, gMission.missionData->Objectives)
	if (ObjectiveIsRequired(o) || o->done > 0)
	{
		color_t textColor = colorWhite;
		pos.x = 5;
		// Objective color dot
		DrawRectangle(
			&gGraphicsDevice, svec2i(pos.x, pos.y + 3), svec2i(2, 2), o->color,
			false);

		pos.x += 5;

		sprintf(sScore, "(%d)", o->done);

		if (!ObjectiveIsRequired(o))
		{
			textColor = colorPurple;
		}
		else if (ObjectiveIsComplete(o))
		{
			textColor = colorRed;
		}
		pos = FontStrMask(o->Description, pos, textColor);
		pos.x += 5;
		FontStrMask(sScore, pos, textColor);
		pos.y -= (FontH() + 1);
	}
	CA_FOREACH_END()
}

static color_t DoorColor(const int x, const int y)
{
	const int l = MapGetDoorKeycardFlag(&gMap, svec2i(x, y));
	return KeyColor(l);
}

void DrawDot(Thing *t, color_t color, struct vec2i pos, int scale)
{
	const struct vec2i dotPos = Vec2ToTile(t->Pos);
	pos = svec2i_add(pos, svec2i_scale(dotPos, (float)scale));
	DrawRectangle(&gGraphicsDevice, pos, svec2i(scale, scale), color, false);
}

/*
	DrawMap tile → screen pixel (existing transform):

	  mapPos = center + centerOn * (-scale)
	  for each tile (x, y) and sub-pixel (j, i) in [0, scale):
	    drawPos = (mapPos.x + x * scale + j, mapPos.y + y * scale + i)

	HUD radar (AutomapDrawRegion) uses scale == 1, so:
	  drawPos = (center.x - centerOn.x + x, center.y - centerOn.y + y)

	With center = clipPos + clipSize/2 (integer division), a scale-1 point is
	inside the SDL clip [clipPos, clipPos+clipSize) iff:
	  centerOn.x - clipSize.x/2  <= x <  centerOn.x + (clipSize.x - clipSize.x/2)
	  (same for y)

	tileX0/tileY0/tileX1/tileY1 select the half-open map rectangle to visit.
	Callers that must draw the whole map pass [0, map->Size).
*/
static void DrawMap(
	Map *map, struct vec2i center, struct vec2i centerOn, struct vec2i size,
	int scale, int flags, const int tileX0, const int tileY0, const int tileX1,
	const int tileY1)
{
	int x, y;
	struct vec2i mapPos =
		svec2i_add(center, svec2i_scale(centerOn, (float)-scale));
	for (y = tileY0; y < tileY1; y++)
	{
		int i;
		for (i = 0; i < scale; i++)
		{
			for (x = tileX0; x < tileX1; x++)
			{
				if (s_automapHudRadar)
				{
					VitaProfileDrawCount(VITA_DRAW_CNT_RADAR_TILES_ITERATED, 1);
				}
				Tile *tile = MapGetTile(map, svec2i(x, y));
				if (tile->Class->Pic != NULL &&
					(tile->isVisited || (flags & AUTOMAP_FLAGS_SHOWALL)))
				{
					if (s_automapHudRadar)
					{
						VitaProfileDrawCount(VITA_DRAW_CNT_RADAR_TILES_VISITED, 1);
					}
					int j;
					for (j = 0; j < scale; j++)
					{
						struct vec2i drawPos = svec2i(
							mapPos.x + x * scale + j,
							mapPos.y + y * scale + i);
						color_t color = colorTransparent;
						switch (tile->Class->Type)
						{
						case TILE_CLASS_WALL:
							color = colorWall;
							break;
						case TILE_CLASS_DOOR:
							color = DoorColor(x, y);
							break;
						case TILE_CLASS_FLOOR:
							color =
								tile->Class->IsRoom ? colorRoom : colorFloor;
							break;
						default:
							CASSERT(false, "Unknown tile class type");
							break;
						}
						if (!ColorEquals(color, colorTransparent))
						{
							if (flags & AUTOMAP_FLAGS_MASK)
							{
								color.a = MASK_ALPHA;
							}
							DrawPoint(drawPos, color);
						}
					}
				}
			}
		}
	}
	if (flags & AUTOMAP_FLAGS_MASK)
	{
		const color_t color = {255, 255, 255, 128};
		DrawRectangle(
			&gGraphicsDevice,
			svec2i_subtract(center, svec2i_scale_divide(size, 2)), size, color,
			false);
	}
}

static void DrawThing(
	Thing *t, Tile *tile, struct vec2i pos, int scale, int flags);
/* tileX0..tileY1 are half-open bounds, matching DrawMap. Fullscreen passes
 * [0, map->Size); HUD radar passes the same window as DrawMap. */
static void DrawObjectivesAndKeys(
	Map *map, struct vec2i pos, int scale, int flags, const int tileX0,
	const int tileY0, const int tileX1, const int tileY1)
{
	for (int y = tileY0; y < tileY1; y++)
	{
		for (int x = tileX0; x < tileX1; x++)
		{
			if (s_automapHudRadar)
			{
				VitaProfileDrawCount(VITA_DRAW_CNT_RADAR_OBJ_TILES_SCANNED, 1);
			}
			Tile *tile = MapGetTile(map, svec2i(x, y));
			CA_FOREACH(ThingId, tid, tile->things)
			if (s_automapHudRadar)
			{
				VitaProfileDrawCount(VITA_DRAW_CNT_RADAR_OBJ_THINGS_INSPECTED, 1);
			}
			DrawThing(ThingIdGetThing(tid), tile, pos, scale, flags);
			CA_FOREACH_END()
		}
	}
}
static void DrawThing(
	Thing *t, Tile *tile, struct vec2i pos, int scale, int flags)
{
	if ((t->flags & THING_OBJECTIVE) != 0)
	{
		const int obj = ObjectiveFromThing(t->flags);
		const Objective *o = CArrayGet(&gMission.missionData->Objectives, obj);
		if (!(o->Flags & OBJECTIVE_HIDDEN) || (flags & AUTOMAP_FLAGS_SHOWALL))
		{
			if ((o->Flags & OBJECTIVE_POSKNOWN) || tile->isVisited ||
				(flags & AUTOMAP_FLAGS_SHOWALL))
			{
				if (s_automapHudRadar)
				{
					VitaProfileDrawCount(VITA_DRAW_CNT_RADAR_OBJ_MARKERS_DRAWN, 1);
				}
				DisplayObjective(t, obj, pos, scale, flags);
			}
		}
	}
	else if (t->kind == KIND_PICKUP && tile->isVisited)
	{
		const Pickup *p = CArrayGet(&gPickups, t->id);
		const int keyFlags = PickupClassGetKeys(p->class);
		if (keyFlags != 0)
		{
			const color_t dotColor = KeyColor(keyFlags);
			if (s_automapHudRadar)
			{
				VitaProfileDrawCount(VITA_DRAW_CNT_RADAR_OBJ_MARKERS_DRAWN, 1);
			}
			DrawDot(t, dotColor, pos, scale);
		}
	}
}

void AutomapDraw(
	GraphicsDevice *g, SDL_Renderer *renderer, const int flags,
	const bool showExit)
{
	if (renderer == NULL)
	{
		renderer = g->gameWindow.renderer;
	}
	struct vec2i mapCenter =
		svec2i(g->cachedConfig.Res.x / 2, g->cachedConfig.Res.y / 2);
	struct vec2i centerOn = svec2i(gMap.Size.x / 2, gMap.Size.y / 2);
	// Set the map scale to fit on screen
	int mapScale = MAP_SCALE_DEFAULT;
	// TODO: allow fractional scales for really big maps / really small screens
	if (gMap.Size.x * mapScale > g->cachedConfig.Res.x ||
		gMap.Size.y * mapScale > g->cachedConfig.Res.y)
	{
		mapScale--;
	}
	struct vec2i pos =
		svec2i_add(mapCenter, svec2i_scale(centerOn, -(float)mapScale));

	// Draw faded green overlay
	const color_t mask = {0, 128, 0, 128};
	DrawRectangle(g, svec2i_zero(), g->cachedConfig.Res,
		mask, true);

	DrawMap(
		&gMap, mapCenter, centerOn, gMap.Size, mapScale, flags, 0, 0,
		gMap.Size.x, gMap.Size.y);
	DrawObjectivesAndKeys(
		&gMap, pos, mapScale, flags, 0, 0, gMap.Size.x, gMap.Size.y);

	CA_FOREACH(const PlayerData, p, gPlayerDatas)
	if (!IsPlayerAlive(p))
	{
		continue;
	}
	DisplayPlayer(renderer, ActorGetByUID(p->ActorUID), pos, mapScale);
	CA_FOREACH_END()

	if (showExit)
	{
		DisplayExits(&gMap, pos, mapScale, flags);
	}
	DisplaySummary();
}

// Draw mini automap (HUD radar). scale is always 1.
void AutomapDrawRegion(
	SDL_Renderer *renderer, Map *map, struct vec2i pos,
	const struct vec2i size, const struct vec2i mapCenter, const int flags,
	const bool showExit)
{
	s_automapHudRadar = 1;
	VitaProfileHudBegin(VITA_HUD_RADAR);
	VitaProfileDrawCount(VITA_DRAW_CNT_RADAR_CALLS, 1);
	VitaProfileSetDrawSource(VITA_SRC_RADAR);
	const int scale = 1;
	const Rect2i oldClip = GraphicsGetClip(renderer);
	GraphicsSetClip(renderer, Rect2iNew(pos, size));
	pos = svec2i_add(pos, svec2i_scale_divide(size, 2));

	/*
		Exact scale-1 clip window (see DrawMap comment):
		  x in [mapCenter.x - size.x/2, mapCenter.x + (size.x - size.x/2))
		One-tile margin on each side covers any off-by-one from integer
		center = clipPos + size/2; SDL clip remains the correctness backstop.
	*/
	int tileX0 = mapCenter.x - size.x / 2 - 1;
	int tileY0 = mapCenter.y - size.y / 2 - 1;
	int tileX1 = mapCenter.x + (size.x - size.x / 2) + 1;
	int tileY1 = mapCenter.y + (size.y - size.y / 2) + 1;
	if (tileX0 < 0)
	{
		tileX0 = 0;
	}
	if (tileY0 < 0)
	{
		tileY0 = 0;
	}
	if (tileX1 > map->Size.x)
	{
		tileX1 = map->Size.x;
	}
	if (tileY1 > map->Size.y)
	{
		tileY1 = map->Size.y;
	}

	VitaProfileHudBegin(VITA_HUD_RADAR_DRAWMAP);
	DrawMap(map, pos, mapCenter, size, scale, flags, tileX0, tileY0, tileX1,
		tileY1);
	VitaProfileHudEnd(VITA_HUD_RADAR_DRAWMAP);
	const struct vec2i centerOn =
		svec2i_add(pos, svec2i_scale(mapCenter, (float)-scale));
	VitaProfileHudBegin(VITA_HUD_RADAR_PLAYERS);
	CA_FOREACH(const PlayerData, p, gPlayerDatas)
	if (!IsPlayerAlive(p))
	{
		continue;
	}
	const TActor *player = ActorGetByUID(p->ActorUID);
	VitaProfileDrawCount(VITA_DRAW_CNT_RADAR_PLAYER_MARKERS, 1);
	DisplayPlayer(renderer, player, centerOn, scale);
	CA_FOREACH_END()
	VitaProfileHudEnd(VITA_HUD_RADAR_PLAYERS);
	VitaProfileHudBegin(VITA_HUD_RADAR_OBJECTIVES);
	DrawObjectivesAndKeys(
		&gMap, centerOn, scale, flags, tileX0, tileY0, tileX1, tileY1);
	VitaProfileHudEnd(VITA_HUD_RADAR_OBJECTIVES);
	if (showExit)
	{
		VitaProfileHudBegin(VITA_HUD_RADAR_EXITS);
		DisplayExits(map, centerOn, scale, flags);
		VitaProfileHudEnd(VITA_HUD_RADAR_EXITS);
	}
	GraphicsSetClip(renderer, oldClip);
	VitaProfileSetDrawSource(VITA_SRC_HUD);
	VitaProfileHudEnd(VITA_HUD_RADAR);
	s_automapHudRadar = 0;
}
