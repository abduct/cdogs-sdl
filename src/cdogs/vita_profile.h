/*
	C-Dogs SDL — Vita performance profiler (compile-gated).

	Profile-6: wall-time accounting (WORK/PRESENT/SKIP, section ms).
	Profile-7: particle forensics (class composition, ages, tiles, draws)
	at ~1 Hz; event-driven create/remove/draw counters.
	Profile-8: draw-stage breakdown of VITA_PROF_DRAW / RunGameDraw.
	Profile-9: hierarchical update/HUD/radar/geometry flush/histogram.

	PSVshellPlus display FPS is external; do not label WORK as FPS.
*/
#pragma once

#if defined(CDOGS_VITA_PROFILE)

#include <stdint.h>

struct Particle;

typedef enum
{
	VITA_PROF_FRAME = 0,
	VITA_PROF_PREUPDATE,
	VITA_PROF_UPDATEFUNC,
	VITA_PROF_UPDATE, /* GameUpdate internals only */
	VITA_PROF_PARTICLES,
	VITA_PROF_PRERENDER,
	VITA_PROF_DRAW,
	VITA_PROF_PRESENT, /* complete WindowContextPostRender */
	VITA_PROF_NUM
} VitaProfileSection;

/*
	Draw-stage timers (Profile-8). Independent of VITA_PROF_* sections.

	HIERARCHY (parent → child; children overlap parents):
	  DRAW
	    CLEAR (RunGameDraw setup)
	    CAMERA (CameraDraw)
	      BUFFER_BUILD (SetFromMap + Fix)
	      TILES_FLOOR / BELOW / MAIN / ABOVE  (each DrawTiles pass)
	        SORT (qsort display list)          [nested]
	        THINGS (DrawThing)                 [nested]
	          PICRENDER (PicRender→TextureRender) [nested]
	      WORLD_HUD (objective/chat/pickup tile passes)
	    HUD (HUDDraw)
	    OVERLAY (CameraDrawMode + pause + automap)

	NON-OVERLAPPING top-level ≈ CLEAR + CAMERA + HUD + OVERLAY + OTHER
	where OTHER = draw_ms − (CLEAR+CAMERA+HUD+OVERLAY).

	Within CAMERA, passes are sequential/non-overlapping with each other;
	SORT/THINGS/PICRENDER nest inside passes and must NOT be summed with them.
*/
typedef enum
{
	VITA_DRAW_CLEAR = 0,
	VITA_DRAW_CAMERA,
	VITA_DRAW_BUFFER_BUILD,
	VITA_DRAW_TILES_FLOOR,
	VITA_DRAW_TILES_BELOW,
	VITA_DRAW_TILES_MAIN,
	VITA_DRAW_TILES_ABOVE,
	VITA_DRAW_WORLD_HUD,
	VITA_DRAW_THINGS,
	VITA_DRAW_PICRENDER,
	VITA_DRAW_SORT,
	VITA_DRAW_HUD,
	VITA_DRAW_OVERLAY,
	VITA_DRAW_NUM
} VitaProfileDrawStage;

typedef enum
{
	VITA_DRAW_CNT_FLOOR_PIC = 0,
	VITA_DRAW_CNT_WALL_PIC,
	VITA_DRAW_CNT_THING_DRAW,
	VITA_DRAW_CNT_PICRENDER,
	VITA_DRAW_CNT_TEXTURE_RENDER,
	VITA_DRAW_CNT_ACTOR_PIC,
	VITA_DRAW_CNT_PARTICLE_PIC,
	VITA_DRAW_CNT_OBJECT_PIC,
	VITA_DRAW_CNT_TILES_CONSIDERED,
	VITA_DRAW_CNT_TILES_DRAWN,
	VITA_DRAW_CNT_GEOM_BATCHES,
	VITA_DRAW_CNT_PARTICLE_ATLAS_HIT,
	VITA_DRAW_CNT_PARTICLE_ATLAS_FALLBACK,
	VITA_DRAW_CNT_TERRAIN_CACHE_DRAW,
	VITA_DRAW_CNT_TERRAIN_CACHE_REBUILD,
	VITA_DRAW_CNT_TERRAIN_WALL_ROW_BLIT,
	/* Profile-9 extensions (appended; existing indices unchanged) */
	VITA_DRAW_CNT_PICKUP_PIC,
	VITA_DRAW_CNT_BULLET_PIC,
	VITA_DRAW_CNT_DOOR_DRAW,
	VITA_DRAW_CNT_SHADOW_DRAW,
	VITA_DRAW_CNT_SDL_DRAWPOINT,
	VITA_DRAW_CNT_SDL_DRAWRECT,
	VITA_DRAW_CNT_SDL_DRAWCROSS,
	VITA_DRAW_CNT_SDL_FILLRECT,
	VITA_DRAW_CNT_CLIP_CHANGE,
	VITA_DRAW_CNT_RENDER_TARGET_CHANGE,
	VITA_DRAW_CNT_SDL_UPDATE_TEXTURE,
	VITA_DRAW_CNT_LOGICAL_TEX_SWITCH,
	VITA_DRAW_CNT_LOGICAL_BLEND_SWITCH,
	VITA_DRAW_CNT_LOGICAL_RENDERER_SWITCH,
	VITA_DRAW_CNT_TERRAIN_LOS_FILLRECT,
	VITA_DRAW_CNT_RADAR_CALLS,
	VITA_DRAW_CNT_RADAR_TILES_ITERATED,
	VITA_DRAW_CNT_RADAR_TILES_VISITED,
	VITA_DRAW_CNT_RADAR_DRAWPOINTS,
	VITA_DRAW_CNT_RADAR_OBJ_TILES_SCANNED,
	VITA_DRAW_CNT_RADAR_OBJ_THINGS_INSPECTED,
	VITA_DRAW_CNT_RADAR_OBJ_MARKERS_DRAWN,
	VITA_DRAW_CNT_RADAR_PLAYER_MARKERS,
	VITA_DRAW_CNT_RADAR_EXIT_MARKERS,
	VITA_DRAW_CNT_PARTICLE_CAT_SMOKE,
	VITA_DRAW_CNT_PARTICLE_CAT_BLOOD,
	VITA_DRAW_CNT_PARTICLE_CAT_BRASS,
	VITA_DRAW_CNT_PARTICLE_CAT_FOOTPRINT,
	VITA_DRAW_CNT_PARTICLE_CAT_SPALL,
	VITA_DRAW_CNT_PARTICLE_CAT_BULLET_HOLE,
	VITA_DRAW_CNT_PARTICLE_CAT_FLAME,
	VITA_DRAW_CNT_PARTICLE_CAT_OTHER,
	VITA_DRAW_CNT_NUM
} VitaProfileDrawCounter;

/* Profile-9: update subsystem timers (inside/around GameUpdate). */
typedef enum
{
	VITA_UPD_AI = 0,
	VITA_UPD_ACTORS,
	VITA_UPD_OBJECTS,
	VITA_UPD_BULLETS,
	VITA_UPD_PICKUPS,
	VITA_UPD_MAP,
	VITA_UPD_WATCHES,
	VITA_UPD_POWERUPS,
	VITA_UPD_MISSION,
	VITA_UPD_EVENTS,
	VITA_UPD_LOS_PLAYER,
	VITA_UPD_CAMERA,
	VITA_UPD_NET_FLUSH,
	VITA_UPD_NUM
} VitaProfileUpdateSub;

/* Profile-9: HUD fine timers (nested under draw_hud_ms). */
typedef enum
{
	VITA_HUD_RADAR = 0,
	VITA_HUD_RADAR_DRAWMAP,
	VITA_HUD_RADAR_OBJECTIVES,
	VITA_HUD_RADAR_PLAYERS,
	VITA_HUD_RADAR_EXITS,
	VITA_HUD_STATUS,
	VITA_HUD_POPUPS,
	VITA_HUD_COMPASS,
	VITA_HUD_DEATHMATCH,
	VITA_HUD_MESSAGE,
	VITA_HUD_KEYCARDS,
	VITA_HUD_MISSION_TIME,
	VITA_HUD_OBJECTIVE_COUNTS,
	VITA_HUD_MISSION_STATE,
	VITA_HUD_PROFILE_OVERLAY,
	VITA_HUD_FPS_CLOCK,
	VITA_HUD_NUM
} VitaProfileHudSub;

/* Profile-9: coarse camera/terrain splits. */
typedef enum
{
	VITA_CAM_FLOOR_CACHE = 0,
	VITA_CAM_FLOOR_LOS,
	VITA_CAM_WALL_ROW,
	VITA_CAM_WALL_LOS,
	VITA_CAM_REBUILD,
	VITA_CAM_NUM
} VitaProfileCamSub;

/* Observational draw source for per-quad batch attribution.
 * NOT part of the geometry batch compatibility key. */
typedef enum
{
	VITA_SRC_OTHER = 0,
	VITA_SRC_PARTICLE,
	VITA_SRC_ACTOR,
	VITA_SRC_WORLD_THING,
	VITA_SRC_TERRAIN,
	VITA_SRC_DOOR,
	VITA_SRC_HUD,
	VITA_SRC_RADAR,
	VITA_SRC_LOS,
	VITA_SRC_NUM
} VitaProfileDrawSource;

#define VITA_GEOM_HIST_BINS 12

void VitaProfileInit(void);
void VitaProfileTerm(void);
void VitaProfileBegin(VitaProfileSection section);
void VitaProfileEnd(VitaProfileSection section);
void VitaProfileDrawnReset(void);
void VitaProfileDrawnInc(void);
void VitaProfileNoteUpdate(void);
void VitaProfileNoteSkip(void);
void VitaProfileObserveSchedulerDt(unsigned dtMs);
void VitaProfileNoteSkipWork(void);
void VitaProfileNotePresentWork(void);
void VitaProfileNoteWork(void);
void VitaProfileDrawOverlay(void);

/* Profile-7 particle hooks (event-driven; cheap). */
uint64_t VitaProfileParticleNextId(void);
void VitaProfileParticleSpawned(int slot, const struct Particle *p, int wasAppend);
void VitaProfileParticleRemoved(int slot, const struct Particle *p);
void VitaProfileParticleDrawn(const struct Particle *p);

/* Profile-8 draw-stage hooks. */
void VitaProfileDrawBegin(VitaProfileDrawStage stage);
void VitaProfileDrawEnd(VitaProfileDrawStage stage);
void VitaProfileDrawCount(VitaProfileDrawCounter counter, unsigned n);

/* Profile-9 hierarchical hooks. */
void VitaProfileUpdateBegin(VitaProfileUpdateSub sub);
void VitaProfileUpdateEnd(VitaProfileUpdateSub sub);
void VitaProfileHudBegin(VitaProfileHudSub sub);
void VitaProfileHudEnd(VitaProfileHudSub sub);
void VitaProfileCamBegin(VitaProfileCamSub sub);
void VitaProfileCamEnd(VitaProfileCamSub sub);
void VitaProfileSetDrawSource(VitaProfileDrawSource src);
VitaProfileDrawSource VitaProfileGetDrawSource(void);
void VitaProfileNoteQuadSource(VitaProfileDrawSource src);
/* reason is TextureFlushReason from texture.h (passed as int to avoid include cycle). */
void VitaProfileGeomBatchSubmitted(int reason, int quadCount);
void VitaProfileParticleClassDrawn(const struct Particle *p);

/* Draw-decision measurement (lightweight; measure-only, never culls). */
void VitaProfileNoteSpriteDestBounds(int destX, int destY, int destW, int destH);
void VitaProfileParticlePrepareEnter(void);
void VitaProfileParticlePrepareLeave(void);
void VitaProfileParticlePreparePauseForSubmit(void);
void VitaProfileParticlePrepareResumeAfterSubmit(void);

/* Generic submission timing (measure-only; nested timers — NOT additive). */
void VitaProfileTexRenderBegin(void);
void VitaProfileTexRenderEnd(void);
void VitaProfileTexAppendBegin(void);
void VitaProfileTexAppendEnd(void);
void VitaProfileTexFlushBegin(void);
void VitaProfileTexFlushEnd(void);
void VitaProfileTexQueryInc(void);
void VitaProfileTexAlphaQueryInc(void);

#else

#define VitaProfileInit() ((void)0)
#define VitaProfileTerm() ((void)0)
#define VitaProfileBegin(section) ((void)0)
#define VitaProfileEnd(section) ((void)0)
#define VitaProfileDrawnReset() ((void)0)
#define VitaProfileDrawnInc() ((void)0)
#define VitaProfileNoteUpdate() ((void)0)
#define VitaProfileNoteSkip() ((void)0)
#define VitaProfileObserveSchedulerDt(dtMs) ((void)0)
#define VitaProfileNoteSkipWork() ((void)0)
#define VitaProfileNotePresentWork() ((void)0)
#define VitaProfileNoteWork() ((void)0)
#define VitaProfileDrawOverlay() ((void)0)
#define VitaProfileParticleNextId() (0ULL)
#define VitaProfileParticleSpawned(slot, p, wasAppend) ((void)0)
#define VitaProfileParticleRemoved(slot, p) ((void)0)
#define VitaProfileParticleDrawn(p) ((void)0)
#define VitaProfileDrawBegin(stage) ((void)0)
#define VitaProfileDrawEnd(stage) ((void)0)
#define VitaProfileDrawCount(counter, n) ((void)0)
#define VitaProfileUpdateBegin(sub) ((void)0)
#define VitaProfileUpdateEnd(sub) ((void)0)
#define VitaProfileHudBegin(sub) ((void)0)
#define VitaProfileHudEnd(sub) ((void)0)
#define VitaProfileCamBegin(sub) ((void)0)
#define VitaProfileCamEnd(sub) ((void)0)
#define VitaProfileSetDrawSource(src) ((void)0)
#define VitaProfileGetDrawSource() (0)
#define VitaProfileNoteQuadSource(src) ((void)0)
#define VitaProfileGeomBatchSubmitted(reason, quadCount) ((void)0)
#define VitaProfileParticleClassDrawn(p) ((void)0)
#define VitaProfileNoteSpriteDestBounds(x, y, w, h) ((void)0)
#define VitaProfileParticlePrepareEnter() ((void)0)
#define VitaProfileParticlePrepareLeave() ((void)0)
#define VitaProfileParticlePreparePauseForSubmit() ((void)0)
#define VitaProfileParticlePrepareResumeAfterSubmit() ((void)0)
#define VitaProfileTexRenderBegin() ((void)0)
#define VitaProfileTexRenderEnd() ((void)0)
#define VitaProfileTexAppendBegin() ((void)0)
#define VitaProfileTexAppendEnd() ((void)0)
#define VitaProfileTexFlushBegin() ((void)0)
#define VitaProfileTexFlushEnd() ((void)0)
#define VitaProfileTexQueryInc() ((void)0)
#define VitaProfileTexAlphaQueryInc() ((void)0)

#endif
