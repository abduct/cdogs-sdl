/*
	C-Dogs SDL — Vita on-screen performance profiler.

	Profile-6: ~1 Hz timing CSV + overlay (WORK/PRESENT/SKIP, section ms).
	Profile-7: particle forensics — event create/remove/draw counters;
	full gParticles class scan at ~1 Hz only; vita-particles.csv;
	vita-particles-final.csv (Term + ~30 s rewrite).
	Profile-8: draw-stage breakdown of RunGameDraw / VITA_PROF_DRAW.
*/
#include "vita_profile.h"

#if defined(CDOGS_VITA_PROFILE)

#include <SDL_timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actors.h"
#include "color.h"
#include "font.h"
#include "grafx.h"
#include "map.h"
#include "objs.h"
#include "particle.h"
#include "particle_class.h"
#include "texture.h"

/* Lightweight validation: Profile-9 forensic probes compile but are
 * no-ops unless CDOGS_VITA_PROFILE_DETAILED is defined. */
#if defined(CDOGS_VITA_PROFILE_DETAILED)
#define VITA_PROF_IS_DETAILED 1
#else
#define VITA_PROF_IS_DETAILED 0
#endif
#include "utils.h"

typedef enum
{
	VP_PREV_NONE = 0,
	VP_PREV_SKIP,
	VP_PREV_PRESENT,
	VP_PREV_OTHER
} VitaPrevWorkKind;

#define VP_MAX_CLASSES 192
#define VP_UNKNOWN_IDX (VP_MAX_CLASSES - 1)
#define VP_AGE_BUCKETS 6
#define VP_SCAN_CAP 4096

typedef struct
{
	const ParticleClass *cls;
	char name[48];
	int rangeLow;
	int rangeHigh;
	ParticleType type;
	float gravity;
	int hitsWalls;
	int bounces;
	int metaWritten;

	uint32_t createdInterval;
	uint32_t removedInterval;
	uint64_t createdTotal;
	uint64_t removedTotal;
	uint32_t drawCallsInterval;

	/* Filled at forensic sample */
	uint32_t active;
	int ageMin;
	int ageMax;
	int64_t ageSum;
	int64_t remainSum;
	uint32_t ageBucket[VP_AGE_BUCKETS];
	uint32_t stationary;
	uint32_t moving;
	uint32_t uniqueTiles;
	uint32_t tiles2;
	uint32_t tiles4;
	uint32_t tiles8;
	uint32_t tiles16;
	uint32_t maxSameTile;
	uint32_t uniquePositions;
	uint32_t duplicatePositionParticles;
	uint32_t maxSamePosition;
} VpClassState;

typedef struct
{
	uint16_t classIdx;
	int16_t tileX;
	int16_t tileY;
	uint32_t posXBits;
	uint32_t posYBits;
} VpScanRec;

typedef struct
{
	Uint64 freq;
	Uint64 sectionStart[VITA_PROF_NUM];
	Uint64 sectionAccum[VITA_PROF_NUM];
	Uint32 sectionInvocations[VITA_PROF_NUM];

	Uint64 workStart;
	Uint64 skipWorkAccum;
	Uint32 skipWorkInvocations;
	Uint64 presentWorkAccum;
	Uint32 presentWorkInvocations;

	Uint64 schedDtAfterSkipAccum;
	Uint32 schedDtAfterSkipCount;
	Uint64 schedDtAfterPresentAccum;
	Uint32 schedDtAfterPresentCount;

	VitaPrevWorkKind prevWorkKind;
	int schedDtPending;

	Uint32 workCount;
	Uint32 updateCount;
	Uint32 skipCount;

	Uint32 drawnThisFrame;
	Uint64 drawnAccum;

	Uint64 sampleStart;
	Uint64 sessionStart;

	float avgMs[VITA_PROF_NUM];
	float skipWorkMs;
	float presentWorkMs;
	float schedDtAfterSkipMs;
	float schedDtAfterPresentMs;
	float workHz;
	float updateHz;
	float drawHz;
	float presentHz;
	float skipHz;
	float drawnPerDraw;
	float sampleElapsedMs;

	unsigned workCountOut;
	unsigned updateCountOut;
	unsigned drawCountOut;
	unsigned presentCountOut;
	unsigned skipCountOut;
	unsigned skipWorkCountOut;
	unsigned presentWorkCountOut;
	unsigned schedDtAfterSkipCountOut;
	unsigned schedDtAfterPresentCountOut;

	unsigned pSize;
	unsigned pActive;
	unsigned oSize;
	unsigned oActive;
	unsigned bSize;
	unsigned bActive;
	unsigned aSize;
	unsigned aActive;

	/* Profile-7 summary for vita-profile.csv */
	unsigned particleClassesObserved;
	unsigned particlesInactive;
	int highestActiveSlot;
	float particleDrawCallsPerSec;
	float particleDrawsPerPresent;
	unsigned particleDrawCallsInterval;
	unsigned presentCountForDrawAvg;
	uint32_t slotsReusedInterval;
	uint32_t slotsReusedTotal;
	uint32_t newSlotsInterval;
	uint32_t newSlotsTotal;
	uint32_t forensicMismatch;
	int classesRegistered;

	char line0[56];
	char line1[56];
	char line2[64];
	char line3[64];
	char line4[56];
	char line5[56];
	char line6[56];
	char line7[56];
	int ready;
	FILE *csv;
	FILE *particlesCsv;
	FILE *particlesMetaCsv;
	unsigned csvRowsSinceFlush;
	unsigned sampleIndex;
	uint64_t nextDiagId;

	VpClassState classes[VP_MAX_CLASSES];
	int classCount; /* named classes in [0, classCount); UNKNOWN at VP_UNKNOWN_IDX */
	int classesSeeded;

	uint8_t *slotEverLived;
	int slotEverLivedCap;

	VpScanRec scanBuf[VP_SCAN_CAP];
	int scanCount;

	/* Profile-8 draw stages */
	Uint64 drawStart[VITA_DRAW_NUM];
	Uint64 drawAccum[VITA_DRAW_NUM];
	Uint32 drawInvocations[VITA_DRAW_NUM];
	uint64_t drawCountAccum[VITA_DRAW_CNT_NUM];
	int inDraw;

	float avgDrawMs[VITA_DRAW_NUM];
	float drawOtherMs;
	float picrenderMsPerCall;
	float thingsMsPerThing;
	float floorPicPerPresent;
	float wallPicPerPresent;
	float thingDrawPerPresent;
	float picrenderPerPresent;
	float textureRenderPerPresent;
	float actorPicPerPresent;
	float particlePicPerPresent;
	float objectPicPerPresent;
	float tilesConsideredPerPresent;
	float tilesDrawnPerPresent;
	float geomBatchesPerPresent;
	float particleAtlasHitsPerPresent;
	float particleAtlasFallbacksPerPresent;
	float terrainCacheDrawsPerPresent;
	float terrainCacheRebuildsPerPresent;
	float terrainWallRowBlitsPerPresent;

	/* Profile-9 */
	Uint64 updStart[VITA_UPD_NUM];
	Uint64 updAccum[VITA_UPD_NUM];
	Uint64 hudStart[VITA_HUD_NUM];
	Uint64 hudAccum[VITA_HUD_NUM];
	Uint64 camStart[VITA_CAM_NUM];
	Uint64 camAccum[VITA_CAM_NUM];
	float avgUpdMs[VITA_UPD_NUM];
	float avgHudMs[VITA_HUD_NUM];
	float avgCamMs[VITA_CAM_NUM];
	float updateResidualMs;
	float updatefuncResidualMs;
	float hudResidualMs;

	uint64_t flushReasonAccum[TEX_FLUSH_NUM];
	uint64_t geomHistAccum[VITA_GEOM_HIST_BINS];
	uint64_t geomQuadsSubmitted;
	uint64_t geomVertsSubmitted;
	uint64_t geomIndicesSubmitted;
	int geomQuadsMaxBatch;

	/* Per-current-batch source quad counts; rolled into interval on submit */
	uint32_t curBatchSrcQuads[VITA_SRC_NUM];
	uint64_t geomQuadsBySrc[VITA_SRC_NUM];
	VitaProfileDrawSource drawSource;

	float flushReasonPerPresent[TEX_FLUSH_NUM];
	float geomHistPerPresent[VITA_GEOM_HIST_BINS];
	float geomQuadsPerPresent;
	float geomQuadsPerBatchAvg;
	float geomQuadsPerBatchMax;
	float geomVertsPerPresent;
	float geomIndicesPerPresent;
	float geomQuadsSrcPerPresent[VITA_SRC_NUM];
	float pickupPicPerPresent;
	float bulletPicPerPresent;
	float doorDrawPerPresent;
	float shadowDrawPerPresent;

	/* Cached particle class pointers for category counters (no hot strcmp) */
	const ParticleClass *pcSmoke[8];
	int pcSmokeN;
	const ParticleClass *pcBlood[8];
	int pcBloodN;
	const ParticleClass *pcBrass[8];
	int pcBrassN;
	const ParticleClass *pcFoot[8];
	int pcFootN;
	const ParticleClass *pcSpall[8];
	int pcSpallN;
	const ParticleClass *pcHole[4];
	int pcHoleN;
	const ParticleClass *pcFlame[16];
	int pcFlameN;
	int pcSeeded;

	/* Draw-decision measurement (AABB opportunity + particle prepare). */
	uint64_t spriteBoundsTests;
	uint64_t spriteBoundsWouldCull;
	uint64_t particleBoundsTests;
	uint64_t particleBoundsWouldCull;
	uint64_t nonparticleBoundsTests;
	uint64_t nonparticleBoundsWouldCull;
	float spriteBoundsTestsPerPresent;
	float spriteBoundsWouldCullPerPresent;
	float particleBoundsTestsPerPresent;
	float particleBoundsWouldCullPerPresent;
	float nonparticleBoundsTestsPerPresent;
	float nonparticleBoundsWouldCullPerPresent;
	Uint64 particlePrepareAccum;
	Uint64 particlePrepareStart;
	int particlePrepareDepth;
	int particlePreparePaused;
	float particleDrawPrepareMs;

	/* Generic submission timing (nested; do NOT sum render+append+flush). */
	Uint64 texRenderAccum;
	Uint64 texRenderStart;
	int texRenderDepth;
	Uint64 texAppendAccum;
	Uint64 texAppendStart;
	int texAppendDepth;
	Uint64 texFlushAccum;
	Uint64 texFlushStart;
	int texFlushDepth;
	uint64_t texQueryCalls;
	uint64_t texAlphaQueryCalls;
	float textureRenderMs;
	float textureBatchAppendMs;
	float textureBatchFlushMs;
	float textureQueryCallsPerPresent;
	float alphaQueryCallsPerPresent;
} VitaProfileState;

static VitaProfileState s_vp;

static uint32_t FloatBits(float f)
{
	union
	{
		float f;
		uint32_t u;
	} u;
	u.f = f;
	return u.u;
}

static void VpEnsureSlotCap(int slot)
{
	if (slot < s_vp.slotEverLivedCap)
	{
		return;
	}
	const int newCap = slot + 256;
	uint8_t *n = NULL;
	CMALLOC(n, (size_t)newCap);
	if (n == NULL)
	{
		return;
	}
	if (s_vp.slotEverLived != NULL && s_vp.slotEverLivedCap > 0)
	{
		memcpy(n, s_vp.slotEverLived, (size_t)s_vp.slotEverLivedCap);
		CFREE(s_vp.slotEverLived);
	}
	memset(n + s_vp.slotEverLivedCap, 0,
		(size_t)(newCap - s_vp.slotEverLivedCap));
	s_vp.slotEverLived = n;
	s_vp.slotEverLivedCap = newCap;
}

static void VpInitClassSlot(VpClassState *c, const ParticleClass *cls)
{
	memset(c, 0, sizeof *c);
	c->cls = cls;
	c->ageMin = 0;
	c->ageMax = 0;
	if (cls != NULL && cls->Name != NULL)
	{
		strncpy(c->name, cls->Name, sizeof c->name - 1);
		c->rangeLow = cls->RangeLow;
		c->rangeHigh = cls->RangeHigh;
		c->type = cls->Type;
		c->gravity = cls->GravityFactor;
		c->hitsWalls = cls->HitsWalls ? 1 : 0;
		c->bounces = cls->Bounces ? 1 : 0;
	}
	else
	{
		strncpy(c->name, "UNKNOWN", sizeof c->name - 1);
	}
}

static void VpWriteClassMeta(VpClassState *c)
{
	if (c->metaWritten || s_vp.particlesMetaCsv == NULL)
	{
		return;
	}
	fprintf(
		s_vp.particlesMetaCsv,
		"%s,%d,%d,%d,%.3f,%d,%d\n", c->name, c->rangeLow, c->rangeHigh,
		(int)c->type, (double)c->gravity, c->hitsWalls, c->bounces);
	c->metaWritten = 1;
	s_vp.csvRowsSinceFlush++;
}

static int VpFindOrAddClass(const ParticleClass *cls)
{
	if (cls == NULL)
	{
		if (s_vp.classes[VP_UNKNOWN_IDX].name[0] == '\0')
		{
			VpInitClassSlot(&s_vp.classes[VP_UNKNOWN_IDX], NULL);
			VpWriteClassMeta(&s_vp.classes[VP_UNKNOWN_IDX]);
		}
		return VP_UNKNOWN_IDX;
	}
	for (int i = 0; i < s_vp.classCount; i++)
	{
		if (s_vp.classes[i].cls == cls)
		{
			return i;
		}
	}
	if (s_vp.classCount >= VP_UNKNOWN_IDX)
	{
		return VP_UNKNOWN_IDX;
	}
	const int idx = s_vp.classCount++;
	VpInitClassSlot(&s_vp.classes[idx], cls);
	VpWriteClassMeta(&s_vp.classes[idx]);
	return idx;
}

static void VpSeedLoadedClasses(void)
{
	if (s_vp.classesSeeded)
	{
		return;
	}
	if (gParticleClasses.Classes.size == 0 &&
		gParticleClasses.CustomClasses.size == 0)
	{
		return;
	}
	CA_FOREACH(const ParticleClass, c, gParticleClasses.Classes)
	VpFindOrAddClass(c);
	CA_FOREACH_END()
	CA_FOREACH(const ParticleClass, c, gParticleClasses.CustomClasses)
	VpFindOrAddClass(c);
	CA_FOREACH_END()
	s_vp.classesSeeded = 1;
}

static int AgeBucket(int age)
{
	if (age <= 70)
		return 0;
	if (age <= 350)
		return 1;
	if (age <= 1400)
		return 2;
	if (age <= 4200)
		return 3;
	if (age <= 8400)
		return 4;
	return 5;
}

static int CmpScanTile(const void *a, const void *b)
{
	const VpScanRec *x = a;
	const VpScanRec *y = b;
	if (x->classIdx != y->classIdx)
		return (int)x->classIdx - (int)y->classIdx;
	if (x->tileY != y->tileY)
		return (int)x->tileY - (int)y->tileY;
	return (int)x->tileX - (int)y->tileX;
}

static int CmpScanPos(const void *a, const void *b)
{
	const VpScanRec *x = a;
	const VpScanRec *y = b;
	if (x->classIdx != y->classIdx)
		return (int)x->classIdx - (int)y->classIdx;
	if (x->posXBits != y->posXBits)
		return (x->posXBits < y->posXBits) ? -1 : 1;
	if (x->posYBits != y->posYBits)
		return (x->posYBits < y->posYBits) ? -1 : 1;
	return 0;
}

static void VpResetSampleClassStats(void)
{
	for (int i = 0; i < s_vp.classCount; i++)
	{
		VpClassState *c = &s_vp.classes[i];
		c->active = 0;
		c->ageMin = 0;
		c->ageMax = 0;
		c->ageSum = 0;
		c->remainSum = 0;
		memset(c->ageBucket, 0, sizeof c->ageBucket);
		c->stationary = 0;
		c->moving = 0;
		c->uniqueTiles = 0;
		c->tiles2 = c->tiles4 = c->tiles8 = c->tiles16 = 0;
		c->maxSameTile = 0;
		c->uniquePositions = 0;
		c->duplicatePositionParticles = 0;
		c->maxSamePosition = 0;
	}
	VpClassState *u = &s_vp.classes[VP_UNKNOWN_IDX];
	if (u->name[0] != '\0')
	{
		u->active = 0;
		u->ageMin = 0;
		u->ageMax = 0;
		u->ageSum = 0;
		u->remainSum = 0;
		memset(u->ageBucket, 0, sizeof u->ageBucket);
		u->stationary = 0;
		u->moving = 0;
		u->uniqueTiles = 0;
		u->tiles2 = u->tiles4 = u->tiles8 = u->tiles16 = 0;
		u->maxSameTile = 0;
		u->uniquePositions = 0;
		u->duplicatePositionParticles = 0;
		u->maxSamePosition = 0;
	}
}

static void VpAccumulateTileRuns(void)
{
	if (s_vp.scanCount <= 0)
	{
		return;
	}
	qsort(s_vp.scanBuf, (size_t)s_vp.scanCount, sizeof(VpScanRec), CmpScanTile);
	int i = 0;
	while (i < s_vp.scanCount)
	{
		const uint16_t ci = s_vp.scanBuf[i].classIdx;
		VpClassState *c = &s_vp.classes[ci];
		int j = i;
		while (j < s_vp.scanCount && s_vp.scanBuf[j].classIdx == ci)
		{
			const int16_t tx = s_vp.scanBuf[j].tileX;
			const int16_t ty = s_vp.scanBuf[j].tileY;
			int k = j;
			while (k < s_vp.scanCount && s_vp.scanBuf[k].classIdx == ci &&
				   s_vp.scanBuf[k].tileX == tx && s_vp.scanBuf[k].tileY == ty)
			{
				k++;
			}
			const uint32_t n = (uint32_t)(k - j);
			c->uniqueTiles++;
			if (n >= 2)
				c->tiles2++;
			if (n >= 4)
				c->tiles4++;
			if (n >= 8)
				c->tiles8++;
			if (n >= 16)
				c->tiles16++;
			if (n > c->maxSameTile)
				c->maxSameTile = n;
			j = k;
		}
		i = j;
	}
}

static void VpAccumulatePosRuns(void)
{
	if (s_vp.scanCount <= 0)
	{
		return;
	}
	qsort(s_vp.scanBuf, (size_t)s_vp.scanCount, sizeof(VpScanRec), CmpScanPos);
	int i = 0;
	while (i < s_vp.scanCount)
	{
		const uint16_t ci = s_vp.scanBuf[i].classIdx;
		VpClassState *c = &s_vp.classes[ci];
		int j = i;
		while (j < s_vp.scanCount && s_vp.scanBuf[j].classIdx == ci)
		{
			const uint32_t px = s_vp.scanBuf[j].posXBits;
			const uint32_t py = s_vp.scanBuf[j].posYBits;
			int k = j;
			while (k < s_vp.scanCount && s_vp.scanBuf[k].classIdx == ci &&
				   s_vp.scanBuf[k].posXBits == px &&
				   s_vp.scanBuf[k].posYBits == py)
			{
				k++;
			}
			const uint32_t n = (uint32_t)(k - j);
			c->uniquePositions++;
			if (n > 1)
			{
				c->duplicatePositionParticles += n;
			}
			if (n > c->maxSamePosition)
				c->maxSamePosition = n;
			j = k;
		}
		i = j;
	}
}

static void VpForensicScan(void)
{
	VpSeedLoadedClasses();
	VpResetSampleClassStats();
	s_vp.scanCount = 0;
	s_vp.highestActiveSlot = -1;
	s_vp.pActive = 0;

	for (int i = 0; i < (int)gParticles.size; i++)
	{
		const Particle *p = CArrayGet(&gParticles, i);
		if (!p->isInUse)
		{
			continue;
		}
		s_vp.pActive++;
		s_vp.highestActiveSlot = i;
		const int ci = VpFindOrAddClass(p->Class);
		VpClassState *c = &s_vp.classes[ci];
		c->active++;
		const int age = p->Count;
		if (c->active == 1)
		{
			c->ageMin = age;
			c->ageMax = age;
		}
		else
		{
			if (age < c->ageMin)
				c->ageMin = age;
			if (age > c->ageMax)
				c->ageMax = age;
		}
		c->ageSum += age;
		c->remainSum += (p->Range - age);
		c->ageBucket[AgeBucket(age)]++;
		/* Stationary := XY Vel == 0 (DZ ignored; matches settled decals). */
		if (svec2_is_zero(p->thing.Vel))
		{
			c->stationary++;
		}
		else
		{
			c->moving++;
		}
		if (s_vp.scanCount < VP_SCAN_CAP)
		{
			VpScanRec *r = &s_vp.scanBuf[s_vp.scanCount++];
			r->classIdx = (uint16_t)ci;
			const struct vec2i tile = Vec2ToTile(p->Pos);
			r->tileX = (int16_t)tile.x;
			r->tileY = (int16_t)tile.y;
			r->posXBits = FloatBits(p->Pos.x);
			r->posYBits = FloatBits(p->Pos.y);
		}
	}

	VpAccumulateTileRuns();
	VpAccumulatePosRuns();

	uint32_t sumActive = 0;
	uint32_t sumStatMove = 0;
	uint32_t sumBuckets = 0;
	unsigned classesWithActive = 0;
	const int lim = s_vp.classCount;
	for (int pass = 0; pass < 2; pass++)
	{
		const int from = (pass == 0) ? 0 : VP_UNKNOWN_IDX;
		const int to = (pass == 0) ? lim : VP_UNKNOWN_IDX + 1;
		for (int i = from; i < to; i++)
		{
			VpClassState *c = &s_vp.classes[i];
			if (c->name[0] == '\0')
				continue;
			sumActive += c->active;
			sumStatMove += c->stationary + c->moving;
			for (int b = 0; b < VP_AGE_BUCKETS; b++)
				sumBuckets += c->ageBucket[b];
			if (c->active > 0)
				classesWithActive++;
		}
	}
	s_vp.particleClassesObserved = classesWithActive;
	s_vp.classesRegistered = s_vp.classCount +
		(s_vp.classes[VP_UNKNOWN_IDX].name[0] != '\0' ? 1 : 0);
	s_vp.pSize = (unsigned)gParticles.size;
	s_vp.particlesInactive = s_vp.pSize - s_vp.pActive;

	s_vp.forensicMismatch = 0;
	if (sumActive != s_vp.pActive)
		s_vp.forensicMismatch |= 1u;
	if (sumStatMove != sumActive)
		s_vp.forensicMismatch |= 2u;
	if (sumBuckets != sumActive)
		s_vp.forensicMismatch |= 4u;
	/* Balance: sum(createdTotal - removedTotal) across classes vs active */
	{
		int64_t net = 0;
		for (int i = 0; i < s_vp.classCount; i++)
		{
			net += (int64_t)s_vp.classes[i].createdTotal -
				   (int64_t)s_vp.classes[i].removedTotal;
		}
		if (s_vp.classes[VP_UNKNOWN_IDX].name[0] != '\0')
		{
			net += (int64_t)s_vp.classes[VP_UNKNOWN_IDX].createdTotal -
				   (int64_t)s_vp.classes[VP_UNKNOWN_IDX].removedTotal;
		}
		if (net != (int64_t)s_vp.pActive)
			s_vp.forensicMismatch |= 8u;
	}
}

static void VpWriteParticlesCsv(float seconds, float wallSec)
{
	if (s_vp.particlesCsv == NULL)
	{
		return;
	}
	const unsigned presents = s_vp.presentCountOut;
	for (int pass = 0; pass < 2; pass++)
	{
		const int from = (pass == 0) ? 0 : VP_UNKNOWN_IDX;
		const int to = (pass == 0) ? s_vp.classCount : VP_UNKNOWN_IDX + 1;
		for (int i = from; i < to; i++)
		{
			VpClassState *c = &s_vp.classes[i];
			if (c->name[0] == '\0')
				continue;
			if (c->active == 0 && c->createdInterval == 0 &&
				c->removedInterval == 0 && c->drawCallsInterval == 0 &&
				c->createdTotal == 0 && c->removedTotal == 0)
			{
				continue;
			}
			const float ageAvg =
				c->active ? (float)c->ageSum / (float)c->active : 0.0f;
			const float remainAvg =
				c->active ? (float)c->remainSum / (float)c->active : 0.0f;
			const int net = (int)c->createdInterval - (int)c->removedInterval;
			const float drawPerPresent =
				presents > 0 ? (float)c->drawCallsInterval / (float)presents
							 : 0.0f;
			fprintf(
				s_vp.particlesCsv,
				"%.1f,%s,%u,%u,%u,%llu,%llu,%d,"
				"%d,%.1f,%d,%.1f,"
				"%u,%u,%u,%u,%u,%u,"
				"%u,%u,"
				"%u,%u,%u,%u,%u,%u,"
				"%u,%u,%u,"
				"%u,%.3f,"
				"%d,%d,"
				"%.4f,%.4f,%.4f\n",
				(double)seconds, c->name, c->active, c->createdInterval,
				c->removedInterval, (unsigned long long)c->createdTotal,
				(unsigned long long)c->removedTotal, net, c->ageMin,
				(double)ageAvg, c->ageMax, (double)remainAvg, c->ageBucket[0],
				c->ageBucket[1], c->ageBucket[2], c->ageBucket[3],
				c->ageBucket[4], c->ageBucket[5], c->stationary, c->moving,
				c->uniqueTiles, c->tiles2, c->tiles4, c->tiles8, c->tiles16,
				c->maxSameTile, c->uniquePositions,
				c->duplicatePositionParticles, c->maxSamePosition,
				c->drawCallsInterval, (double)drawPerPresent, c->rangeLow,
				c->rangeHigh,
				wallSec > 0.0f ? (double)c->createdInterval / (double)wallSec
							   : 0.0,
				wallSec > 0.0f ? (double)c->removedInterval / (double)wallSec
							   : 0.0,
				wallSec > 0.0f ? (double)net / (double)wallSec : 0.0);
		}
	}
}

static void VpWriteFinalSnapshot(void)
{
	char path[CDOGS_PATH_MAX];
	GetDataFilePath(path, "vita-particles-final.csv");
	FILE *f = fopen(path, "w");
	if (f == NULL)
	{
		return;
	}
	fputs(
		"diagnostic_unique_id,slot_index,class_name,count_age,range,"
		"pos_x,pos_y,tile_x,tile_y,vel_x,vel_y,stationary,dz\n",
		f);
	for (int i = 0; i < (int)gParticles.size; i++)
	{
		const Particle *p = CArrayGet(&gParticles, i);
		if (!p->isInUse)
		{
			continue;
		}
		const char *name =
			(p->Class && p->Class->Name) ? p->Class->Name : "UNKNOWN";
		const struct vec2i tile = Vec2ToTile(p->Pos);
		const int stationary = svec2_is_zero(p->thing.Vel) ? 1 : 0;
		fprintf(
			f, "%llu,%d,%s,%d,%d,%.4f,%.4f,%d,%d,%.4f,%.4f,%d,%.4f\n",
			(unsigned long long)p->profileDiagId, i, name, p->Count, p->Range,
			(double)p->Pos.x, (double)p->Pos.y, tile.x, tile.y,
			(double)p->thing.Vel.x, (double)p->thing.Vel.y, stationary,
			(double)p->DZ);
	}
	fclose(f);
}

static unsigned CountActiveObjs(void)
{
	unsigned n = 0;
	CA_FOREACH(const TObject, o, gObjs)
	if (o->isInUse)
	{
		n++;
	}
	CA_FOREACH_END()
	return n;
}

static unsigned CountActiveBullets(void)
{
	unsigned n = 0;
	CA_FOREACH(const TMobileObject, m, gMobObjs)
	if (m->isInUse)
	{
		n++;
	}
	CA_FOREACH_END()
	return n;
}

static unsigned CountActiveActors(void)
{
	unsigned n = 0;
	CA_FOREACH(const TActor, a, gActors)
	if (a->isInUse)
	{
		n++;
	}
	CA_FOREACH_END()
	return n;
}

static float TicksToMs(Uint64 ticks)
{
	return (float)((double)ticks * 1000.0 / (double)s_vp.freq);
}

static float AvgMs(VitaProfileSection section)
{
	const Uint32 n = s_vp.sectionInvocations[section];
	if (n == 0)
	{
		return 0.0f;
	}
	return TicksToMs(s_vp.sectionAccum[section]) / (float)n;
}

static float AvgAccumMs(Uint64 accumTicks, Uint32 n)
{
	if (n == 0)
	{
		return 0.0f;
	}
	return TicksToMs(accumTicks) / (float)n;
}

static float AvgAccumUintMs(Uint64 accumMs, Uint32 n)
{
	if (n == 0)
	{
		return 0.0f;
	}
	return (float)accumMs / (float)n;
}

static float AvgDrawMs(VitaProfileDrawStage stage)
{
	/* Always normalize to VITA_PROF_DRAW frames so nested timers
	   (things/picrender/sort) report total cost per draw, not per call. */
	if (s_vp.drawCountOut == 0)
	{
		return 0.0f;
	}
	return TicksToMs(s_vp.drawAccum[stage]) / (float)s_vp.drawCountOut;
}

static float PerPresentCount(uint64_t total)
{
	if (s_vp.presentCountOut == 0)
	{
		return 0.0f;
	}
	return (float)total / (float)s_vp.presentCountOut;
}


static int GeomHistBin(int quads)
{
	if (quads <= 1) return 0;
	if (quads == 2) return 1;
	if (quads <= 4) return 2;
	if (quads <= 8) return 3;
	if (quads <= 16) return 4;
	if (quads <= 32) return 5;
	if (quads <= 64) return 6;
	if (quads <= 128) return 7;
	if (quads <= 256) return 8;
	if (quads <= 512) return 9;
	if (quads <= 1024) return 10;
	return 11;
}

static float AvgSubMs(Uint64 accum, unsigned drawCountOut)
{
	if (drawCountOut == 0)
	{
		return 0.0f;
	}
	return TicksToMs(accum) / (float)drawCountOut;
}

static float AvgUpdMs(Uint64 accum, unsigned updateCountOut)
{
	/* Normalize update subs to updatefunc/update invocations via updateCountOut */
	if (updateCountOut == 0)
	{
		return 0.0f;
	}
	return TicksToMs(accum) / (float)updateCountOut;
}

static void WriteCsvSample(float seconds)
{
	if (s_vp.csv == NULL)
	{
		return;
	}
#if !VITA_PROF_IS_DETAILED
	fprintf(
		s_vp.csv,
		"%.1f,%.1f,"
		"%.2f,%.2f,%.2f,%.2f,%.2f,"
		"%u,%u,%u,%u,%u,"
		"%.2f,%.2f,%.2f,%.2f,%.2f,"
		"%.2f,%.2f,%.2f,"
		"%.3f,%.3f,%.3f,"
		"%.1f,%.1f,"
		"%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,"
		"%.3f,"
		"%.3f,%.3f,%.3f,"
		"%.1f,%.1f\n",
		(double)seconds, (double)s_vp.sampleElapsedMs,
		(double)s_vp.workHz, (double)s_vp.updateHz, (double)s_vp.drawHz,
		(double)s_vp.presentHz, (double)s_vp.skipHz, s_vp.workCountOut,
		s_vp.updateCountOut, s_vp.drawCountOut, s_vp.presentCountOut,
		s_vp.skipCountOut, (double)s_vp.avgMs[VITA_PROF_FRAME],
		(double)s_vp.avgMs[VITA_PROF_PREUPDATE],
		(double)s_vp.avgMs[VITA_PROF_UPDATEFUNC],
		(double)s_vp.avgMs[VITA_PROF_UPDATE],
		(double)s_vp.avgMs[VITA_PROF_PARTICLES],
		(double)s_vp.avgMs[VITA_PROF_PRERENDER],
		(double)s_vp.avgMs[VITA_PROF_DRAW],
		(double)s_vp.avgMs[VITA_PROF_PRESENT],
		(double)s_vp.avgDrawMs[VITA_DRAW_CAMERA],
		(double)s_vp.avgDrawMs[VITA_DRAW_HUD],
		(double)s_vp.drawOtherMs,
		(double)s_vp.textureRenderPerPresent,
		(double)s_vp.geomBatchesPerPresent,
		(double)s_vp.spriteBoundsTestsPerPresent,
		(double)s_vp.spriteBoundsWouldCullPerPresent,
		(double)s_vp.particleBoundsTestsPerPresent,
		(double)s_vp.particleBoundsWouldCullPerPresent,
		(double)s_vp.nonparticleBoundsTestsPerPresent,
		(double)s_vp.nonparticleBoundsWouldCullPerPresent,
		(double)s_vp.particleDrawPrepareMs,
		(double)s_vp.textureRenderMs,
		(double)s_vp.textureBatchAppendMs,
		(double)s_vp.textureBatchFlushMs,
		(double)s_vp.textureQueryCallsPerPresent,
		(double)s_vp.alphaQueryCallsPerPresent);
	s_vp.csvRowsSinceFlush++;
	if (s_vp.csvRowsSinceFlush >= 10)
	{
		if (s_vp.csv)
			fflush(s_vp.csv);
		if (s_vp.particlesCsv)
			fflush(s_vp.particlesCsv);
		if (s_vp.particlesMetaCsv)
			fflush(s_vp.particlesMetaCsv);
		s_vp.csvRowsSinceFlush = 0;
	}
	return;
#endif
	fprintf(
		s_vp.csv,
		"%.1f,%.1f,"
		"%.2f,%.2f,%.2f,%.2f,%.2f,"
		"%u,%u,%u,%u,%u,"
		"%.2f,%.2f,%.2f,%.2f,%.2f,"
		"%.2f,%.2f,%.2f,"
		"%.2f,%.2f,"
		"%.2f,%.2f,"
		"%u,%u,%u,%u,"
		"%u,%u,%u,%u,%u,%u,%u,%u,"
		"%.0f,"
		"%u,%u,%d,%.2f,%.3f,%u,%u,%u,%u,%u,"
		/* Profile-8 */
		"%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
		"%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
		"%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,"
		"%.4f,%.4f,"
		/* UPDATE */
		"%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
		/* CAMERA */
		"%.3f,%.3f,%.3f,%.3f,%.3f,"
		/* HUD */
		"%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
		/* ENTITY extras */
		"%.1f,%.1f,%.1f,%.1f,"
		/* GEOMETRY */
		"%.1f,%.2f,%.1f,%.1f,%.1f,"
		/* FLUSH REASONS (15) */
		"%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,"
		/* HIST (12) */
		"%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,"
		/* SRC QUADS (9) */
		"%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,"
		/* PRIMITIVES/STATE extras already in drawCountAccum columns via entity */
		"%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,"
		/* RADAR */
		"%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,"
		/* PARTICLE CATS */
		"%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n",
		(double)seconds, (double)s_vp.sampleElapsedMs,
		(double)s_vp.workHz, (double)s_vp.updateHz, (double)s_vp.drawHz,
		(double)s_vp.presentHz, (double)s_vp.skipHz, s_vp.workCountOut,
		s_vp.updateCountOut, s_vp.drawCountOut, s_vp.presentCountOut,
		s_vp.skipCountOut, (double)s_vp.avgMs[VITA_PROF_FRAME],
		(double)s_vp.avgMs[VITA_PROF_PREUPDATE],
		(double)s_vp.avgMs[VITA_PROF_UPDATEFUNC],
		(double)s_vp.avgMs[VITA_PROF_UPDATE],
		(double)s_vp.avgMs[VITA_PROF_PARTICLES],
		(double)s_vp.avgMs[VITA_PROF_PRERENDER],
		(double)s_vp.avgMs[VITA_PROF_DRAW],
		(double)s_vp.avgMs[VITA_PROF_PRESENT], (double)s_vp.skipWorkMs,
		(double)s_vp.presentWorkMs, (double)s_vp.schedDtAfterSkipMs,
		(double)s_vp.schedDtAfterPresentMs, s_vp.skipWorkCountOut,
		s_vp.presentWorkCountOut, s_vp.schedDtAfterSkipCountOut,
		s_vp.schedDtAfterPresentCountOut, s_vp.pSize, s_vp.pActive, s_vp.oSize,
		s_vp.oActive, s_vp.bSize, s_vp.bActive, s_vp.aSize, s_vp.aActive,
		(double)s_vp.drawnPerDraw, s_vp.particleClassesObserved,
		s_vp.particlesInactive, s_vp.highestActiveSlot,
		(double)s_vp.particleDrawCallsPerSec,
		(double)s_vp.particleDrawsPerPresent, s_vp.slotsReusedInterval,
		s_vp.newSlotsInterval, s_vp.slotsReusedTotal, s_vp.newSlotsTotal,
		s_vp.forensicMismatch,
		(double)s_vp.avgDrawMs[VITA_DRAW_CLEAR],
		(double)s_vp.avgDrawMs[VITA_DRAW_CAMERA],
		(double)s_vp.avgDrawMs[VITA_DRAW_BUFFER_BUILD],
		(double)s_vp.avgDrawMs[VITA_DRAW_TILES_FLOOR],
		(double)s_vp.avgDrawMs[VITA_DRAW_TILES_BELOW],
		(double)s_vp.avgDrawMs[VITA_DRAW_TILES_MAIN],
		(double)s_vp.avgDrawMs[VITA_DRAW_TILES_ABOVE],
		(double)s_vp.avgDrawMs[VITA_DRAW_WORLD_HUD],
		(double)s_vp.avgDrawMs[VITA_DRAW_THINGS],
		(double)s_vp.avgDrawMs[VITA_DRAW_PICRENDER],
		(double)s_vp.avgDrawMs[VITA_DRAW_SORT],
		(double)s_vp.avgDrawMs[VITA_DRAW_HUD],
		(double)s_vp.avgDrawMs[VITA_DRAW_OVERLAY],
		(double)s_vp.drawOtherMs,
		(double)s_vp.floorPicPerPresent, (double)s_vp.wallPicPerPresent,
		(double)s_vp.thingDrawPerPresent, (double)s_vp.picrenderPerPresent,
		(double)s_vp.textureRenderPerPresent, (double)s_vp.actorPicPerPresent,
		(double)s_vp.particlePicPerPresent, (double)s_vp.objectPicPerPresent,
		(double)s_vp.tilesConsideredPerPresent,
		(double)s_vp.tilesDrawnPerPresent, (double)s_vp.geomBatchesPerPresent,
		(double)s_vp.particleAtlasHitsPerPresent,
		(double)s_vp.particleAtlasFallbacksPerPresent,
		(double)s_vp.terrainCacheDrawsPerPresent,
		(double)s_vp.terrainCacheRebuildsPerPresent,
		(double)s_vp.terrainWallRowBlitsPerPresent,
		(double)s_vp.picrenderMsPerCall, (double)s_vp.thingsMsPerThing,
		/* UPDATE */
		(double)s_vp.avgUpdMs[VITA_UPD_AI],
		(double)s_vp.avgUpdMs[VITA_UPD_ACTORS],
		(double)s_vp.avgUpdMs[VITA_UPD_OBJECTS],
		(double)s_vp.avgUpdMs[VITA_UPD_BULLETS],
		(double)s_vp.avgUpdMs[VITA_UPD_PICKUPS],
		(double)s_vp.avgUpdMs[VITA_UPD_MAP],
		(double)s_vp.avgUpdMs[VITA_UPD_WATCHES],
		(double)s_vp.avgUpdMs[VITA_UPD_POWERUPS],
		(double)s_vp.avgUpdMs[VITA_UPD_MISSION],
		(double)s_vp.avgUpdMs[VITA_UPD_EVENTS],
		(double)s_vp.avgUpdMs[VITA_UPD_LOS_PLAYER],
		(double)s_vp.avgUpdMs[VITA_UPD_CAMERA],
		(double)s_vp.avgUpdMs[VITA_UPD_NET_FLUSH],
		(double)s_vp.updateResidualMs,
		(double)s_vp.updatefuncResidualMs,
		/* CAMERA */
		(double)s_vp.avgCamMs[VITA_CAM_FLOOR_CACHE],
		(double)s_vp.avgCamMs[VITA_CAM_FLOOR_LOS],
		(double)s_vp.avgCamMs[VITA_CAM_WALL_ROW],
		(double)s_vp.avgCamMs[VITA_CAM_WALL_LOS],
		(double)s_vp.avgCamMs[VITA_CAM_REBUILD],
		/* HUD */
		(double)s_vp.avgHudMs[VITA_HUD_RADAR],
		(double)s_vp.avgHudMs[VITA_HUD_RADAR_DRAWMAP],
		(double)s_vp.avgHudMs[VITA_HUD_RADAR_OBJECTIVES],
		(double)s_vp.avgHudMs[VITA_HUD_RADAR_PLAYERS],
		(double)s_vp.avgHudMs[VITA_HUD_RADAR_EXITS],
		(double)s_vp.avgHudMs[VITA_HUD_STATUS],
		(double)s_vp.avgHudMs[VITA_HUD_POPUPS],
		(double)s_vp.avgHudMs[VITA_HUD_COMPASS],
		(double)s_vp.avgHudMs[VITA_HUD_DEATHMATCH],
		(double)s_vp.avgHudMs[VITA_HUD_MESSAGE],
		(double)s_vp.avgHudMs[VITA_HUD_KEYCARDS],
		(double)s_vp.avgHudMs[VITA_HUD_MISSION_TIME],
		(double)s_vp.avgHudMs[VITA_HUD_OBJECTIVE_COUNTS],
		(double)s_vp.avgHudMs[VITA_HUD_MISSION_STATE],
		(double)s_vp.avgHudMs[VITA_HUD_PROFILE_OVERLAY],
		(double)s_vp.avgHudMs[VITA_HUD_FPS_CLOCK],
		(double)s_vp.hudResidualMs,
		/* ENTITY */
		(double)s_vp.pickupPicPerPresent, (double)s_vp.bulletPicPerPresent,
		(double)s_vp.doorDrawPerPresent, (double)s_vp.shadowDrawPerPresent,
		/* GEOM */
		(double)s_vp.geomQuadsPerPresent, (double)s_vp.geomQuadsPerBatchAvg,
		(double)s_vp.geomQuadsPerBatchMax, (double)s_vp.geomVertsPerPresent,
		(double)s_vp.geomIndicesPerPresent,
		/* FLUSH */
		(double)s_vp.flushReasonPerPresent[TEX_FLUSH_TEXTURE_CHANGE],
		(double)s_vp.flushReasonPerPresent[TEX_FLUSH_BLEND_CHANGE],
		(double)s_vp.flushReasonPerPresent[TEX_FLUSH_RENDERER_CHANGE],
		(double)s_vp.flushReasonPerPresent[TEX_FLUSH_CAPACITY],
		(double)s_vp.flushReasonPerPresent[TEX_FLUSH_CLIP],
		(double)s_vp.flushReasonPerPresent[TEX_FLUSH_RENDER_TARGET],
		(double)s_vp.flushReasonPerPresent[TEX_FLUSH_PRE_RENDER],
		(double)s_vp.flushReasonPerPresent[TEX_FLUSH_POST_PRESENT],
		(double)s_vp.flushReasonPerPresent[TEX_FLUSH_DRAW_POINT],
		(double)s_vp.flushReasonPerPresent[TEX_FLUSH_DRAW_RECT],
		(double)s_vp.flushReasonPerPresent[TEX_FLUSH_DRAW_CROSS],
		(double)s_vp.flushReasonPerPresent[TEX_FLUSH_TERRAIN_LOS],
		(double)s_vp.flushReasonPerPresent[TEX_FLUSH_TEXTURE_UPLOAD],
		(double)s_vp.flushReasonPerPresent[TEX_FLUSH_EXPLICIT_OTHER],
		(double)s_vp.flushReasonPerPresent[TEX_FLUSH_UNCLASSIFIED],
		/* HIST */
		(double)s_vp.geomHistPerPresent[0], (double)s_vp.geomHistPerPresent[1],
		(double)s_vp.geomHistPerPresent[2], (double)s_vp.geomHistPerPresent[3],
		(double)s_vp.geomHistPerPresent[4], (double)s_vp.geomHistPerPresent[5],
		(double)s_vp.geomHistPerPresent[6], (double)s_vp.geomHistPerPresent[7],
		(double)s_vp.geomHistPerPresent[8], (double)s_vp.geomHistPerPresent[9],
		(double)s_vp.geomHistPerPresent[10], (double)s_vp.geomHistPerPresent[11],
		/* SRC */
		(double)s_vp.geomQuadsSrcPerPresent[VITA_SRC_OTHER],
		(double)s_vp.geomQuadsSrcPerPresent[VITA_SRC_PARTICLE],
		(double)s_vp.geomQuadsSrcPerPresent[VITA_SRC_ACTOR],
		(double)s_vp.geomQuadsSrcPerPresent[VITA_SRC_WORLD_THING],
		(double)s_vp.geomQuadsSrcPerPresent[VITA_SRC_TERRAIN],
		(double)s_vp.geomQuadsSrcPerPresent[VITA_SRC_DOOR],
		(double)s_vp.geomQuadsSrcPerPresent[VITA_SRC_HUD],
		(double)s_vp.geomQuadsSrcPerPresent[VITA_SRC_RADAR],
		(double)s_vp.geomQuadsSrcPerPresent[VITA_SRC_LOS],
		/* PRIMITIVES */
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_SDL_DRAWPOINT]),
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_SDL_DRAWRECT]),
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_SDL_DRAWCROSS]),
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_SDL_FILLRECT]),
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_CLIP_CHANGE]),
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_RENDER_TARGET_CHANGE]),
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_SDL_UPDATE_TEXTURE]),
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_LOGICAL_TEX_SWITCH]),
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_LOGICAL_BLEND_SWITCH]),
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_LOGICAL_RENDERER_SWITCH]),
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_TERRAIN_LOS_FILLRECT]),
		/* RADAR — NOTE: these use drawCountAccum which is cleared AFTER WriteCsvSample;
		   so we must compute into locals before clear. Here drawCountAccum still valid. */
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_RADAR_CALLS]),
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_RADAR_TILES_ITERATED]),
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_RADAR_TILES_VISITED]),
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_RADAR_DRAWPOINTS]),
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_RADAR_OBJ_TILES_SCANNED]),
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_RADAR_OBJ_THINGS_INSPECTED]),
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_RADAR_OBJ_MARKERS_DRAWN]),
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_RADAR_PLAYER_MARKERS]),
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_RADAR_EXIT_MARKERS]),
		/* PARTICLE CATS */
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_PARTICLE_CAT_SMOKE]),
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_PARTICLE_CAT_BLOOD]),
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_PARTICLE_CAT_BRASS]),
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_PARTICLE_CAT_FOOTPRINT]),
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_PARTICLE_CAT_SPALL]),
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_PARTICLE_CAT_BULLET_HOLE]),
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_PARTICLE_CAT_FLAME]),
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_PARTICLE_CAT_OTHER]));
	s_vp.csvRowsSinceFlush++;
	if (s_vp.csvRowsSinceFlush >= 10)
	{
		if (s_vp.csv)
			fflush(s_vp.csv);
		if (s_vp.particlesCsv)
			fflush(s_vp.particlesCsv);
		if (s_vp.particlesMetaCsv)
			fflush(s_vp.particlesMetaCsv);
		s_vp.csvRowsSinceFlush = 0;
	}
}

void VitaProfileInit(void)
{
	char path[CDOGS_PATH_MAX];

	memset(&s_vp, 0, sizeof s_vp);
	s_vp.freq = SDL_GetPerformanceFrequency();
	if (s_vp.freq == 0)
	{
		s_vp.freq = 1;
	}
	s_vp.sampleStart = SDL_GetPerformanceCounter();
	s_vp.sessionStart = s_vp.sampleStart;
	s_vp.nextDiagId = 1;
	s_vp.highestActiveSlot = -1;

	GetDataFilePath(path, "vita-profile.csv");
	s_vp.csv = fopen(path, "w");
	if (s_vp.csv != NULL)
	{
#if VITA_PROF_IS_DETAILED
		fputs(
			"seconds,sample_elapsed_ms,"
			"work_hz,update_hz,draw_hz,present_hz,skip_hz,"
			"work_count,update_count,draw_count,present_count,skip_count,"
			"frame_ms,preupdate_ms,updatefunc_ms,update_ms,particles_ms,"
			"prerender_ms,draw_ms,present_ms,"
			"skip_work_ms,present_work_ms,"
			"scheduler_dt_after_skip_ms,scheduler_dt_after_present_ms,"
			"skip_work_count,present_work_count,"
			"scheduler_dt_after_skip_count,scheduler_dt_after_present_count,"
			"particles_size,particles_active,objs_size,objs_active,"
			"bullets_size,bullets_active,actors_size,actors_active,"
			"drawn_per_draw,"
			"particle_classes_active,particles_inactive_slots,"
			"highest_active_slot_index,particle_draw_calls_per_sec,"
			"particle_draws_per_present,slots_reused_interval,"
			"new_slots_appended_interval,slots_reused_total,"
			"new_slots_appended_total,forensic_mismatch,"
			"draw_clear_ms,draw_camera_total_ms,drawbuffer_build_ms,"
			"draw_tiles_floor_ms,draw_tiles_below_ms,draw_tiles_main_ms,"
			"draw_tiles_above_ms,draw_world_hud_ms,"
			"draw_things_ms,draw_picrender_ms,draw_sort_ms,"
			"draw_hud_ms,draw_overlay_ms,draw_other_ms,"
			"floor_pic_per_present,wall_pic_per_present,"
			"thing_draw_per_present,picrender_per_present,"
			"texture_render_per_present,actor_pic_per_present,"
			"particle_pic_per_present,object_pic_per_present,"
			"drawbuffer_tiles_considered_per_present,"
			"drawbuffer_tiles_drawn_per_present,"
			"geometry_batches_per_present,"
			"particle_atlas_hits_per_present,"
			"particle_atlas_fallbacks_per_present,"
			"terrain_cache_draws_per_present,"
			"terrain_cache_rebuilds_per_present,"
			"terrain_wall_row_cache_blits_per_present,"
			"picrender_ms_per_call,things_ms_per_thing,"
			/* Profile-9 UPDATE */
			"update_ai_ms,update_actors_ms,update_objects_ms,update_bullets_ms,"
			"update_pickups_ms,update_map_ms,update_watches_ms,update_powerups_ms,"
			"update_mission_ms,update_events_ms,update_los_player_ms,camera_update_ms,"
			"net_flush_ms,update_residual_ms,updatefunc_residual_ms,"
			/* CAMERA */
			"cam_floor_cache_ms,cam_floor_los_ms,cam_wall_row_ms,cam_wall_los_ms,cam_rebuild_ms,"
			/* HUD */
			"hud_radar_ms,hud_radar_drawmap_ms,hud_radar_objectives_ms,hud_radar_players_ms,"
			"hud_radar_exits_ms,hud_status_ms,hud_popups_ms,hud_compass_ms,"
			"hud_deathmatch_ms,hud_message_ms,hud_keycards_ms,hud_mission_time_ms,"
			"hud_objective_counts_ms,hud_mission_state_ms,hud_profile_overlay_ms,"
			"hud_fps_clock_ms,hud_residual_ms,"
			/* ENTITY */
			"pickup_pic_per_present,bullet_pic_per_present,door_draw_per_present,shadow_draw_per_present,"
			/* GEOMETRY */
			"geometry_quads_submitted_per_present,geometry_quads_per_batch_avg,"
			"geometry_quads_per_batch_max,geometry_verts_per_present,geometry_indices_per_present,"
			/* FLUSH */
			"geometry_flush_texture_change_per_present,geometry_flush_blend_change_per_present,"
			"geometry_flush_renderer_change_per_present,geometry_flush_capacity_per_present,"
			"geometry_flush_clip_per_present,geometry_flush_render_target_per_present,"
			"geometry_flush_pre_render_per_present,geometry_flush_post_present_per_present,"
			"geometry_flush_drawpoint_per_present,geometry_flush_drawrect_per_present,"
			"geometry_flush_drawcross_per_present,geometry_flush_terrain_los_per_present,"
			"geometry_flush_texture_upload_per_present,geometry_flush_explicit_other_per_present,"
			"geometry_flush_unclassified_per_present,"
			/* HIST */
			"geom_batch_hist_1,geom_batch_hist_2,geom_batch_hist_3_4,geom_batch_hist_5_8,"
			"geom_batch_hist_9_16,geom_batch_hist_17_32,geom_batch_hist_33_64,geom_batch_hist_65_128,"
			"geom_batch_hist_129_256,geom_batch_hist_257_512,geom_batch_hist_513_1024,geom_batch_hist_1025p,"
			/* SRC QUADS */
			"geometry_quads_other,geometry_quads_particle,geometry_quads_actor,geometry_quads_world_thing,"
			"geometry_quads_terrain,geometry_quads_door,geometry_quads_hud,geometry_quads_radar,geometry_quads_los,"
			/* PRIMITIVES/STATE */
			"sdl_drawpoint_per_present,sdl_drawrect_per_present,sdl_drawcross_per_present,"
			"sdl_fillrect_per_present,clip_changes_per_present,render_target_changes_per_present,"
			"sdl_update_texture_per_present,logical_tex_switch_per_present,logical_blend_switch_per_present,"
			"logical_renderer_switch_per_present,terrain_los_fillrect_per_present,"
			/* RADAR */
			"radar_calls_per_present,radar_tiles_iterated_per_present,radar_tiles_visited_per_present,"
			"radar_drawpoints_per_present,radar_obj_tiles_scanned_per_present,"
			"radar_obj_things_inspected_per_present,radar_obj_markers_drawn_per_present,"
			"radar_player_markers_per_present,radar_exit_markers_per_present,"
			/* PARTICLE CATS */
			"particle_cat_smoke_per_present,particle_cat_blood_per_present,particle_cat_brass_per_present,"
			"particle_cat_footprint_per_present,particle_cat_spall_per_present,"
			"particle_cat_bullet_hole_per_present,particle_cat_flame_per_present,particle_cat_other_per_present\n",
			s_vp.csv);
#else
		fputs(
			"seconds,sample_elapsed_ms,"
			"work_hz,update_hz,draw_hz,present_hz,skip_hz,"
			"work_count,update_count,draw_count,present_count,skip_count,"
			"frame_ms,preupdate_ms,updatefunc_ms,update_ms,particles_ms,"
			"prerender_ms,draw_ms,present_ms,"
			"draw_camera_total_ms,draw_hud_ms,draw_other_ms,"
			"texture_render_per_present,geometry_batches_per_present,"
			"sprite_bounds_tests_per_present,sprite_bounds_would_cull_per_present,"
			"particle_bounds_tests_per_present,particle_bounds_would_cull_per_present,"
			"nonparticle_bounds_tests_per_present,nonparticle_bounds_would_cull_per_present,"
			"particle_draw_prepare_ms,"
			"texture_render_ms,texture_batch_append_ms,texture_batch_flush_ms,"
			"texture_query_calls,alpha_query_calls\n",
			s_vp.csv);
#endif
		fflush(s_vp.csv);
	}

	GetDataFilePath(path, "vita-particles.csv");
	s_vp.particlesCsv = fopen(path, "w");
	if (s_vp.particlesCsv != NULL)
	{
		fputs(
			"seconds,class_name,active,created_interval,removed_interval,"
			"created_total,removed_total,net_interval,"
			"age_min,age_avg,age_max,remaining_life_avg,"
			"age_0_70,age_71_350,age_351_1400,age_1401_4200,"
			"age_4201_8400,age_8401_plus,"
			"stationary,moving,"
			"unique_tiles,tiles_2plus,tiles_4plus,tiles_8plus,tiles_16plus,"
			"max_same_tile,"
			"unique_positions,duplicate_position_particles,max_same_position,"
			"draw_calls_interval,draw_calls_per_present,"
			"range_min,range_max,"
			"created_per_sec,removed_per_sec,net_growth_per_sec\n",
			s_vp.particlesCsv);
		fflush(s_vp.particlesCsv);
	}

	GetDataFilePath(path, "vita-particles-meta.csv");
	s_vp.particlesMetaCsv = fopen(path, "w");
	if (s_vp.particlesMetaCsv != NULL)
	{
		fputs(
			"class_name,range_min,range_max,type,gravity,hits_walls,bounces\n",
			s_vp.particlesMetaCsv);
		fflush(s_vp.particlesMetaCsv);
	}
}

void VitaProfileTerm(void)
{
	VpWriteFinalSnapshot();
	if (s_vp.csv != NULL)
	{
		fflush(s_vp.csv);
		fclose(s_vp.csv);
		s_vp.csv = NULL;
	}
	if (s_vp.particlesCsv != NULL)
	{
		fflush(s_vp.particlesCsv);
		fclose(s_vp.particlesCsv);
		s_vp.particlesCsv = NULL;
	}
	if (s_vp.particlesMetaCsv != NULL)
	{
		fflush(s_vp.particlesMetaCsv);
		fclose(s_vp.particlesMetaCsv);
		s_vp.particlesMetaCsv = NULL;
	}
	CFREE(s_vp.slotEverLived);
	s_vp.slotEverLived = NULL;
	s_vp.slotEverLivedCap = 0;
}

uint64_t VitaProfileParticleNextId(void)
{
	const uint64_t id = s_vp.nextDiagId++;
	if (s_vp.nextDiagId == 0)
	{
		s_vp.nextDiagId = 1;
	}
	return id;
}

void VitaProfileParticleSpawned(
	int slot, const struct Particle *p, int wasAppend)
{
	const int ci = VpFindOrAddClass(p ? p->Class : NULL);
	s_vp.classes[ci].createdInterval++;
	s_vp.classes[ci].createdTotal++;
	VpEnsureSlotCap(slot);
	if (wasAppend)
	{
		s_vp.newSlotsInterval++;
		s_vp.newSlotsTotal++;
	}
	else if (s_vp.slotEverLived != NULL && slot < s_vp.slotEverLivedCap &&
			 s_vp.slotEverLived[slot])
	{
		s_vp.slotsReusedInterval++;
		s_vp.slotsReusedTotal++;
	}
	if (s_vp.slotEverLived != NULL && slot < s_vp.slotEverLivedCap)
	{
		s_vp.slotEverLived[slot] = 1;
	}
}

void VitaProfileParticleRemoved(int slot, const struct Particle *p)
{
	UNUSED(slot);
	const int ci = VpFindOrAddClass(p ? p->Class : NULL);
	s_vp.classes[ci].removedInterval++;
	s_vp.classes[ci].removedTotal++;
}

void VitaProfileParticleDrawn(const struct Particle *p)
{
	const int ci = VpFindOrAddClass(p ? p->Class : NULL);
	s_vp.classes[ci].drawCallsInterval++;
	s_vp.particleDrawCallsInterval++;
}

void VitaProfileBegin(VitaProfileSection section)
{
	if ((unsigned)section >= VITA_PROF_NUM)
	{
		return;
	}
	const Uint64 now = SDL_GetPerformanceCounter();
	s_vp.sectionStart[section] = now;
	if (section == VITA_PROF_FRAME)
	{
		s_vp.workStart = now;
	}
	else if (section == VITA_PROF_DRAW)
	{
		s_vp.inDraw = 1;
	}
}

void VitaProfileEnd(VitaProfileSection section)
{
	if ((unsigned)section >= VITA_PROF_NUM)
	{
		return;
	}
	const Uint64 now = SDL_GetPerformanceCounter();
	s_vp.sectionAccum[section] += now - s_vp.sectionStart[section];
	s_vp.sectionInvocations[section]++;
	if (section == VITA_PROF_DRAW)
	{
		s_vp.drawnAccum += s_vp.drawnThisFrame;
		s_vp.inDraw = 0;
	}
}

void VitaProfileDrawnReset(void)
{
	s_vp.drawnThisFrame = 0;
}

void VitaProfileDrawnInc(void)
{
	s_vp.drawnThisFrame++;
}

void VitaProfileDrawBegin(VitaProfileDrawStage stage)
{
	if (!s_vp.inDraw || (unsigned)stage >= VITA_DRAW_NUM)
	{
		return;
	}
#if !VITA_PROF_IS_DETAILED
	/* Lightweight: keep only top-level stages needed for draw_camera/draw_hud. */
	if (stage != VITA_DRAW_CLEAR && stage != VITA_DRAW_CAMERA &&
		stage != VITA_DRAW_HUD && stage != VITA_DRAW_OVERLAY)
	{
		return;
	}
#endif
	s_vp.drawStart[stage] = SDL_GetPerformanceCounter();
}

void VitaProfileDrawEnd(VitaProfileDrawStage stage)
{
	if (!s_vp.inDraw || (unsigned)stage >= VITA_DRAW_NUM)
	{
		return;
	}
#if !VITA_PROF_IS_DETAILED
	if (stage != VITA_DRAW_CLEAR && stage != VITA_DRAW_CAMERA &&
		stage != VITA_DRAW_HUD && stage != VITA_DRAW_OVERLAY)
	{
		return;
	}
#endif
	const Uint64 now = SDL_GetPerformanceCounter();
	s_vp.drawAccum[stage] += now - s_vp.drawStart[stage];
	s_vp.drawInvocations[stage]++;
}

void VitaProfileDrawCount(VitaProfileDrawCounter counter, unsigned n)
{
	if (!s_vp.inDraw || (unsigned)counter >= VITA_DRAW_CNT_NUM)
	{
		return;
	}
#if !VITA_PROF_IS_DETAILED
	if (counter != VITA_DRAW_CNT_TEXTURE_RENDER &&
		counter != VITA_DRAW_CNT_GEOM_BATCHES)
	{
		return;
	}
#endif
	s_vp.drawCountAccum[counter] += n;
}

void VitaProfileNoteUpdate(void)
{
	s_vp.updateCount++;
}

void VitaProfileNoteSkip(void)
{
	s_vp.skipCount++;
}

void VitaProfileObserveSchedulerDt(unsigned dtMs)
{
	if (!s_vp.schedDtPending)
	{
		return;
	}
	s_vp.schedDtPending = 0;
	if (s_vp.prevWorkKind == VP_PREV_SKIP)
	{
		s_vp.schedDtAfterSkipAccum += dtMs;
		s_vp.schedDtAfterSkipCount++;
	}
	else if (s_vp.prevWorkKind == VP_PREV_PRESENT)
	{
		s_vp.schedDtAfterPresentAccum += dtMs;
		s_vp.schedDtAfterPresentCount++;
	}
}

static void VitaProfileEmitSampleIfDue(void)
{
	s_vp.workCount++;

	const Uint64 now = SDL_GetPerformanceCounter();
	const Uint64 elapsed = now - s_vp.sampleStart;
	if (elapsed < s_vp.freq)
	{
		return;
	}

	const float wallSec = TicksToMs(elapsed) / 1000.0f;
	s_vp.sampleElapsedMs = TicksToMs(elapsed);

	s_vp.workCountOut = s_vp.workCount;
	s_vp.updateCountOut = s_vp.updateCount;
	s_vp.drawCountOut = s_vp.sectionInvocations[VITA_PROF_DRAW];
	s_vp.presentCountOut = s_vp.sectionInvocations[VITA_PROF_PRESENT];
	s_vp.skipCountOut = s_vp.skipCount;
	s_vp.skipWorkCountOut = s_vp.skipWorkInvocations;
	s_vp.presentWorkCountOut = s_vp.presentWorkInvocations;
	s_vp.schedDtAfterSkipCountOut = s_vp.schedDtAfterSkipCount;
	s_vp.schedDtAfterPresentCountOut = s_vp.schedDtAfterPresentCount;

	s_vp.workHz = (float)s_vp.workCountOut / wallSec;
	s_vp.updateHz = (float)s_vp.updateCountOut / wallSec;
	s_vp.drawHz = (float)s_vp.drawCountOut / wallSec;
	s_vp.presentHz = (float)s_vp.presentCountOut / wallSec;
	s_vp.skipHz = (float)s_vp.skipCountOut / wallSec;

	for (int i = 0; i < VITA_PROF_NUM; i++)
	{
		s_vp.avgMs[i] = AvgMs((VitaProfileSection)i);
	}

	for (int i = 0; i < VITA_DRAW_NUM; i++)
	{
		s_vp.avgDrawMs[i] = AvgDrawMs((VitaProfileDrawStage)i);
	}

	{
		const float accounted =
			s_vp.avgDrawMs[VITA_DRAW_CLEAR] +
			s_vp.avgDrawMs[VITA_DRAW_CAMERA] +
			s_vp.avgDrawMs[VITA_DRAW_HUD] +
			s_vp.avgDrawMs[VITA_DRAW_OVERLAY];
		const float rem = s_vp.avgMs[VITA_PROF_DRAW] - accounted;
		s_vp.drawOtherMs = rem > 0.0f ? rem : 0.0f;
	}

	s_vp.floorPicPerPresent =
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_FLOOR_PIC]);
	s_vp.wallPicPerPresent =
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_WALL_PIC]);
	s_vp.thingDrawPerPresent =
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_THING_DRAW]);
	s_vp.picrenderPerPresent =
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_PICRENDER]);
	s_vp.textureRenderPerPresent =
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_TEXTURE_RENDER]);
	s_vp.actorPicPerPresent =
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_ACTOR_PIC]);
	s_vp.particlePicPerPresent =
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_PARTICLE_PIC]);
	s_vp.objectPicPerPresent =
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_OBJECT_PIC]);
	s_vp.tilesConsideredPerPresent =
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_TILES_CONSIDERED]);
	s_vp.tilesDrawnPerPresent =
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_TILES_DRAWN]);
	s_vp.geomBatchesPerPresent =
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_GEOM_BATCHES]);
	s_vp.particleAtlasHitsPerPresent =
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_PARTICLE_ATLAS_HIT]);
	s_vp.particleAtlasFallbacksPerPresent = PerPresentCount(
		s_vp.drawCountAccum[VITA_DRAW_CNT_PARTICLE_ATLAS_FALLBACK]);
	s_vp.terrainCacheDrawsPerPresent =
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_TERRAIN_CACHE_DRAW]);
	s_vp.terrainCacheRebuildsPerPresent = PerPresentCount(
		s_vp.drawCountAccum[VITA_DRAW_CNT_TERRAIN_CACHE_REBUILD]);
	s_vp.terrainWallRowBlitsPerPresent = PerPresentCount(
		s_vp.drawCountAccum[VITA_DRAW_CNT_TERRAIN_WALL_ROW_BLIT]);

	/* Derived at sample write — not in hot loops */
	if (s_vp.drawCountAccum[VITA_DRAW_CNT_PICRENDER] > 0 &&
		s_vp.drawInvocations[VITA_DRAW_PICRENDER] > 0)
	{
		const float totalPicMs = TicksToMs(s_vp.drawAccum[VITA_DRAW_PICRENDER]);
		s_vp.picrenderMsPerCall =
			totalPicMs / (float)s_vp.drawCountAccum[VITA_DRAW_CNT_PICRENDER];
	}
	else
	{
		s_vp.picrenderMsPerCall = 0.0f;
	}
	if (s_vp.drawCountAccum[VITA_DRAW_CNT_THING_DRAW] > 0 &&
		s_vp.drawInvocations[VITA_DRAW_THINGS] > 0)
	{
		const float totalThingsMs = TicksToMs(s_vp.drawAccum[VITA_DRAW_THINGS]);
		s_vp.thingsMsPerThing =
			totalThingsMs / (float)s_vp.drawCountAccum[VITA_DRAW_CNT_THING_DRAW];
	}
	else
	{
		s_vp.thingsMsPerThing = 0.0f;
	}

	s_vp.skipWorkMs =
		AvgAccumMs(s_vp.skipWorkAccum, s_vp.skipWorkInvocations);
	s_vp.presentWorkMs =
		AvgAccumMs(s_vp.presentWorkAccum, s_vp.presentWorkInvocations);
	s_vp.schedDtAfterSkipMs =
		AvgAccumUintMs(s_vp.schedDtAfterSkipAccum, s_vp.schedDtAfterSkipCount);
	s_vp.schedDtAfterPresentMs = AvgAccumUintMs(
		s_vp.schedDtAfterPresentAccum, s_vp.schedDtAfterPresentCount);

	if (s_vp.drawCountOut > 0)
	{
		s_vp.drawnPerDraw =
			(float)s_vp.drawnAccum / (float)s_vp.drawCountOut;
	}
	else
	{
		s_vp.drawnPerDraw = 0.0f;
	}

	/* Profile-7 forensic scan (~1 Hz) */
	VpForensicScan();
	s_vp.particleDrawCallsPerSec =
		(float)s_vp.particleDrawCallsInterval / wallSec;
	s_vp.particleDrawsPerPresent =
		s_vp.presentCountOut > 0
			? (float)s_vp.particleDrawCallsInterval /
				  (float)s_vp.presentCountOut
			: 0.0f;

	s_vp.oSize = (unsigned)gObjs.size;
	s_vp.oActive = CountActiveObjs();
	s_vp.bSize = (unsigned)gMobObjs.size;
	s_vp.bActive = CountActiveBullets();
	s_vp.aSize = (unsigned)gActors.size;
	s_vp.aActive = CountActiveActors();


	/* Profile-9 update/hud/cam averages */
	for (int i = 0; i < VITA_UPD_NUM; i++)
	{
		s_vp.avgUpdMs[i] = AvgUpdMs(s_vp.updAccum[i], s_vp.updateCountOut);
	}
	for (int i = 0; i < VITA_HUD_NUM; i++)
	{
		s_vp.avgHudMs[i] = AvgSubMs(s_vp.hudAccum[i], s_vp.drawCountOut);
	}
	for (int i = 0; i < VITA_CAM_NUM; i++)
	{
		s_vp.avgCamMs[i] = AvgSubMs(s_vp.camAccum[i], s_vp.drawCountOut);
	}
	{
		float sum = s_vp.avgUpdMs[VITA_UPD_AI] + s_vp.avgUpdMs[VITA_UPD_ACTORS] +
			s_vp.avgUpdMs[VITA_UPD_OBJECTS] + s_vp.avgUpdMs[VITA_UPD_BULLETS] +
			s_vp.avgUpdMs[VITA_UPD_PICKUPS] + s_vp.avgMs[VITA_PROF_PARTICLES] +
			s_vp.avgUpdMs[VITA_UPD_MAP] + s_vp.avgUpdMs[VITA_UPD_WATCHES] +
			s_vp.avgUpdMs[VITA_UPD_POWERUPS] + s_vp.avgUpdMs[VITA_UPD_MISSION] +
			s_vp.avgUpdMs[VITA_UPD_EVENTS];
		float rem = s_vp.avgMs[VITA_PROF_UPDATE] - sum;
		s_vp.updateResidualMs = rem > 0.0f ? rem : 0.0f;
	}
	{
		float sum = s_vp.avgUpdMs[VITA_UPD_LOS_PLAYER] +
			s_vp.avgMs[VITA_PROF_UPDATE] + s_vp.avgUpdMs[VITA_UPD_CAMERA];
		float rem = s_vp.avgMs[VITA_PROF_UPDATEFUNC] - sum;
		s_vp.updatefuncResidualMs = rem > 0.0f ? rem : 0.0f;
	}
	{
		float sum = 0.0f;
		for (int i = 0; i < VITA_HUD_NUM; i++)
		{
			sum += s_vp.avgHudMs[i];
		}
		float rem = s_vp.avgDrawMs[VITA_DRAW_HUD] - sum;
		s_vp.hudResidualMs = rem > 0.0f ? rem : 0.0f;
	}
	for (int i = 0; i < TEX_FLUSH_NUM; i++)
	{
		s_vp.flushReasonPerPresent[i] = PerPresentCount(s_vp.flushReasonAccum[i]);
	}
	for (int i = 0; i < VITA_GEOM_HIST_BINS; i++)
	{
		s_vp.geomHistPerPresent[i] = PerPresentCount(s_vp.geomHistAccum[i]);
	}
	s_vp.geomQuadsPerPresent = PerPresentCount(s_vp.geomQuadsSubmitted);
	s_vp.geomVertsPerPresent = PerPresentCount(s_vp.geomVertsSubmitted);
	s_vp.geomIndicesPerPresent = PerPresentCount(s_vp.geomIndicesSubmitted);
	if (s_vp.drawCountAccum[VITA_DRAW_CNT_GEOM_BATCHES] > 0)
	{
		s_vp.geomQuadsPerBatchAvg =
			(float)s_vp.geomQuadsSubmitted /
			(float)s_vp.drawCountAccum[VITA_DRAW_CNT_GEOM_BATCHES];
	}
	else
	{
		s_vp.geomQuadsPerBatchAvg = 0.0f;
	}
	s_vp.geomQuadsPerBatchMax = (float)s_vp.geomQuadsMaxBatch;
	for (int i = 0; i < VITA_SRC_NUM; i++)
	{
		s_vp.geomQuadsSrcPerPresent[i] = PerPresentCount(s_vp.geomQuadsBySrc[i]);
	}
	s_vp.pickupPicPerPresent =
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_PICKUP_PIC]);
	s_vp.bulletPicPerPresent =
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_BULLET_PIC]);
	s_vp.doorDrawPerPresent =
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_DOOR_DRAW]);
	s_vp.shadowDrawPerPresent =
		PerPresentCount(s_vp.drawCountAccum[VITA_DRAW_CNT_SHADOW_DRAW]);

	s_vp.spriteBoundsTestsPerPresent = PerPresentCount(s_vp.spriteBoundsTests);
	s_vp.spriteBoundsWouldCullPerPresent =
		PerPresentCount(s_vp.spriteBoundsWouldCull);
	s_vp.particleBoundsTestsPerPresent =
		PerPresentCount(s_vp.particleBoundsTests);
	s_vp.particleBoundsWouldCullPerPresent =
		PerPresentCount(s_vp.particleBoundsWouldCull);
	s_vp.nonparticleBoundsTestsPerPresent =
		PerPresentCount(s_vp.nonparticleBoundsTests);
	s_vp.nonparticleBoundsWouldCullPerPresent =
		PerPresentCount(s_vp.nonparticleBoundsWouldCull);
	s_vp.particleDrawPrepareMs =
		AvgSubMs(s_vp.particlePrepareAccum, s_vp.drawCountOut);

	s_vp.textureRenderMs = AvgSubMs(s_vp.texRenderAccum, s_vp.drawCountOut);
	s_vp.textureBatchAppendMs =
		AvgSubMs(s_vp.texAppendAccum, s_vp.drawCountOut);
	s_vp.textureBatchFlushMs = AvgSubMs(s_vp.texFlushAccum, s_vp.drawCountOut);
	s_vp.textureQueryCallsPerPresent = PerPresentCount(s_vp.texQueryCalls);
	s_vp.alphaQueryCallsPerPresent = PerPresentCount(s_vp.texAlphaQueryCalls);

	const float seconds = TicksToMs(now - s_vp.sessionStart) / 1000.0f;
	VpWriteParticlesCsv(seconds, wallSec);
	WriteCsvSample(seconds);

	s_vp.sampleIndex++;
	/* Safety rewrite of particle-level snapshot every ~30 s */
	if ((s_vp.sampleIndex % 30) == 0)
	{
		VpWriteFinalSnapshot();
	}

	snprintf(
		s_vp.line0, sizeof s_vp.line0, "WORK %.0f DRAW %.0f PRES %.0f",
		(double)s_vp.workHz, (double)s_vp.drawHz, (double)s_vp.presentHz);
	snprintf(
		s_vp.line1, sizeof s_vp.line1, "SKIP %.0f UPD %.0f",
		(double)s_vp.skipHz, (double)s_vp.updateHz);
	snprintf(
		s_vp.line2, sizeof s_vp.line2, "FRAME %.1f UPD %.1f PART %.1f",
		(double)s_vp.avgMs[VITA_PROF_FRAME],
		(double)s_vp.avgMs[VITA_PROF_UPDATE],
		(double)s_vp.avgMs[VITA_PROF_PARTICLES]);
	snprintf(
		s_vp.line3, sizeof s_vp.line3, "DRAWms %.1f PRESms %.1f",
		(double)s_vp.avgMs[VITA_PROF_DRAW],
		(double)s_vp.avgMs[VITA_PROF_PRESENT]);
	snprintf(
		s_vp.line4, sizeof s_vp.line4, "P %u/%u O %u/%u B %u/%u A %u/%u",
		s_vp.pSize, s_vp.pActive, s_vp.oSize, s_vp.oActive, s_vp.bSize,
		s_vp.bActive, s_vp.aSize, s_vp.aActive);
	snprintf(
		s_vp.line5, sizeof s_vp.line5, "D %.0f PR %.0f",
		(double)s_vp.drawnPerDraw, (double)s_vp.picrenderPerPresent);
	snprintf(
		s_vp.line6, sizeof s_vp.line6, "SW %.1f PW %.1f",
		(double)s_vp.skipWorkMs, (double)s_vp.presentWorkMs);
	snprintf(
		s_vp.line7, sizeof s_vp.line7, "CAM %.1f FL %.1f MN %.1f",
		(double)s_vp.avgDrawMs[VITA_DRAW_CAMERA],
		(double)s_vp.avgDrawMs[VITA_DRAW_TILES_FLOOR],
		(double)s_vp.avgDrawMs[VITA_DRAW_TILES_MAIN]);

	/* Clear interval counters */
	s_vp.workCount = 0;
	s_vp.updateCount = 0;
	s_vp.skipCount = 0;
	s_vp.drawnAccum = 0;
	s_vp.skipWorkAccum = 0;
	s_vp.skipWorkInvocations = 0;
	s_vp.presentWorkAccum = 0;
	s_vp.presentWorkInvocations = 0;
	s_vp.schedDtAfterSkipAccum = 0;
	s_vp.schedDtAfterSkipCount = 0;
	s_vp.schedDtAfterPresentAccum = 0;
	s_vp.schedDtAfterPresentCount = 0;
	s_vp.particleDrawCallsInterval = 0;
	s_vp.slotsReusedInterval = 0;
	s_vp.newSlotsInterval = 0;
	for (int i = 0; i < VITA_PROF_NUM; i++)
	{
		s_vp.sectionAccum[i] = 0;
		s_vp.sectionInvocations[i] = 0;
	}
	for (int i = 0; i < VITA_DRAW_NUM; i++)
	{
		s_vp.drawAccum[i] = 0;
		s_vp.drawInvocations[i] = 0;
	}
	for (int i = 0; i < VITA_DRAW_CNT_NUM; i++)
	{
		s_vp.drawCountAccum[i] = 0;
	}
	for (int i = 0; i < VITA_UPD_NUM; i++)
	{
		s_vp.updAccum[i] = 0;
	}
	for (int i = 0; i < VITA_HUD_NUM; i++)
	{
		s_vp.hudAccum[i] = 0;
	}
	for (int i = 0; i < VITA_CAM_NUM; i++)
	{
		s_vp.camAccum[i] = 0;
	}
	for (int i = 0; i < TEX_FLUSH_NUM; i++)
	{
		s_vp.flushReasonAccum[i] = 0;
	}
	for (int i = 0; i < VITA_GEOM_HIST_BINS; i++)
	{
		s_vp.geomHistAccum[i] = 0;
	}
	for (int i = 0; i < VITA_SRC_NUM; i++)
	{
		s_vp.geomQuadsBySrc[i] = 0;
		s_vp.curBatchSrcQuads[i] = 0;
	}
	s_vp.geomQuadsSubmitted = 0;
	s_vp.geomVertsSubmitted = 0;
	s_vp.geomIndicesSubmitted = 0;
	s_vp.geomQuadsMaxBatch = 0;
	s_vp.spriteBoundsTests = 0;
	s_vp.spriteBoundsWouldCull = 0;
	s_vp.particleBoundsTests = 0;
	s_vp.particleBoundsWouldCull = 0;
	s_vp.nonparticleBoundsTests = 0;
	s_vp.nonparticleBoundsWouldCull = 0;
	s_vp.particlePrepareAccum = 0;
	s_vp.texRenderAccum = 0;
	s_vp.texAppendAccum = 0;
	s_vp.texFlushAccum = 0;
	s_vp.texQueryCalls = 0;
	s_vp.texAlphaQueryCalls = 0;
	for (int i = 0; i < s_vp.classCount; i++)
	{
		s_vp.classes[i].createdInterval = 0;
		s_vp.classes[i].removedInterval = 0;
		s_vp.classes[i].drawCallsInterval = 0;
	}
	if (s_vp.classes[VP_UNKNOWN_IDX].name[0] != '\0')
	{
		s_vp.classes[VP_UNKNOWN_IDX].createdInterval = 0;
		s_vp.classes[VP_UNKNOWN_IDX].removedInterval = 0;
		s_vp.classes[VP_UNKNOWN_IDX].drawCallsInterval = 0;
	}
	s_vp.sampleStart = now;
	s_vp.ready = 1;
}

void VitaProfileNoteSkipWork(void)
{
	const Uint64 now = SDL_GetPerformanceCounter();
	s_vp.skipWorkAccum += now - s_vp.workStart;
	s_vp.skipWorkInvocations++;
	s_vp.prevWorkKind = VP_PREV_SKIP;
	s_vp.schedDtPending = 1;
	VitaProfileEmitSampleIfDue();
}

void VitaProfileNotePresentWork(void)
{
	const Uint64 now = SDL_GetPerformanceCounter();
	s_vp.presentWorkAccum += now - s_vp.workStart;
	s_vp.presentWorkInvocations++;
	s_vp.prevWorkKind = VP_PREV_PRESENT;
	s_vp.schedDtPending = 1;
	VitaProfileEmitSampleIfDue();
}

void VitaProfileNoteWork(void)
{
	s_vp.prevWorkKind = VP_PREV_OTHER;
	s_vp.schedDtPending = 0;
	VitaProfileEmitSampleIfDue();
}

void VitaProfileDrawOverlay(void)
{
#if !VITA_PROF_IS_DETAILED
	return;
#endif
	if (!s_vp.ready)
	{
		return;
	}

	FontOpts opts = FontOptsNew();
	opts.HAlign = ALIGN_START;
	opts.VAlign = ALIGN_START;
	opts.Area = gGraphicsDevice.cachedConfig.Res;
	opts.Pad = svec2i(4, 4);
	opts.Mask = colorYellow;

	FontStrOpt(s_vp.line0, svec2i_zero(), opts);
	opts.Pad.y += FontH();
	FontStrOpt(s_vp.line1, svec2i_zero(), opts);
	opts.Pad.y += FontH();
	FontStrOpt(s_vp.line2, svec2i_zero(), opts);
	opts.Pad.y += FontH();
	FontStrOpt(s_vp.line3, svec2i_zero(), opts);
	opts.Pad.y += FontH();
	FontStrOpt(s_vp.line4, svec2i_zero(), opts);
	opts.Pad.y += FontH();
	FontStrOpt(s_vp.line5, svec2i_zero(), opts);
	opts.Pad.y += FontH();
	FontStrOpt(s_vp.line6, svec2i_zero(), opts);
	opts.Pad.y += FontH();
	FontStrOpt(s_vp.line7, svec2i_zero(), opts);
}


void VitaProfileUpdateBegin(VitaProfileUpdateSub sub)
{
#if !VITA_PROF_IS_DETAILED
	(void)sub;
	return;
#endif
	if ((unsigned)sub >= VITA_UPD_NUM)
	{
		return;
	}
	s_vp.updStart[sub] = SDL_GetPerformanceCounter();
}

void VitaProfileUpdateEnd(VitaProfileUpdateSub sub)
{
#if !VITA_PROF_IS_DETAILED
	(void)sub;
	return;
#endif
	if ((unsigned)sub >= VITA_UPD_NUM)
	{
		return;
	}
	const Uint64 now = SDL_GetPerformanceCounter();
	s_vp.updAccum[sub] += now - s_vp.updStart[sub];
}

void VitaProfileHudBegin(VitaProfileHudSub sub)
{
#if !VITA_PROF_IS_DETAILED
	(void)sub;
	return;
#endif
	if (!s_vp.inDraw || (unsigned)sub >= VITA_HUD_NUM)
	{
		return;
	}
	s_vp.hudStart[sub] = SDL_GetPerformanceCounter();
}

void VitaProfileHudEnd(VitaProfileHudSub sub)
{
#if !VITA_PROF_IS_DETAILED
	(void)sub;
	return;
#endif
	if (!s_vp.inDraw || (unsigned)sub >= VITA_HUD_NUM)
	{
		return;
	}
	const Uint64 now = SDL_GetPerformanceCounter();
	s_vp.hudAccum[sub] += now - s_vp.hudStart[sub];
}

void VitaProfileCamBegin(VitaProfileCamSub sub)
{
#if !VITA_PROF_IS_DETAILED
	(void)sub;
	return;
#endif
	if (!s_vp.inDraw || (unsigned)sub >= VITA_CAM_NUM)
	{
		return;
	}
	s_vp.camStart[sub] = SDL_GetPerformanceCounter();
}

void VitaProfileCamEnd(VitaProfileCamSub sub)
{
#if !VITA_PROF_IS_DETAILED
	(void)sub;
	return;
#endif
	if (!s_vp.inDraw || (unsigned)sub >= VITA_CAM_NUM)
	{
		return;
	}
	const Uint64 now = SDL_GetPerformanceCounter();
	s_vp.camAccum[sub] += now - s_vp.camStart[sub];
}

void VitaProfileSetDrawSource(VitaProfileDrawSource src)
{
	/* Always track source in lightweight builds: needed for particle vs
	 * non-particle AABB decision metrics (assignment only; cheap). */
	if ((unsigned)src >= VITA_SRC_NUM)
	{
		src = VITA_SRC_OTHER;
	}
	s_vp.drawSource = src;
}

VitaProfileDrawSource VitaProfileGetDrawSource(void)
{
	return s_vp.drawSource;
}

void VitaProfileNoteQuadSource(VitaProfileDrawSource src)
{
#if !VITA_PROF_IS_DETAILED
	(void)src;
	return;
#endif
	if ((unsigned)src >= VITA_SRC_NUM)
	{
		src = VITA_SRC_OTHER;
	}
	s_vp.curBatchSrcQuads[src]++;
}

void VitaProfileGeomBatchSubmitted(int reason, int quadCount)
{
	if (quadCount <= 0)
	{
		return;
	}
#if !VITA_PROF_IS_DETAILED
	(void)reason;
	if (s_vp.inDraw)
	{
		VitaProfileDrawCount(VITA_DRAW_CNT_GEOM_BATCHES, 1);
	}
	return;
#endif
	/* Always clear per-batch source counters so they cannot leak. */
	if (s_vp.inDraw)
	{
		if ((unsigned)reason >= TEX_FLUSH_NUM)
		{
			reason = TEX_FLUSH_UNCLASSIFIED;
		}
		VitaProfileDrawCount(VITA_DRAW_CNT_GEOM_BATCHES, 1);
		s_vp.flushReasonAccum[reason]++;
		s_vp.geomQuadsSubmitted += (uint64_t)quadCount;
		s_vp.geomVertsSubmitted += (uint64_t)quadCount * 4u;
		s_vp.geomIndicesSubmitted += (uint64_t)quadCount * 6u;
		if (quadCount > s_vp.geomQuadsMaxBatch)
		{
			s_vp.geomQuadsMaxBatch = quadCount;
		}
		s_vp.geomHistAccum[GeomHistBin(quadCount)]++;
		for (int i = 0; i < VITA_SRC_NUM; i++)
		{
			s_vp.geomQuadsBySrc[i] += s_vp.curBatchSrcQuads[i];
			s_vp.curBatchSrcQuads[i] = 0;
		}
	}
	else
	{
		for (int i = 0; i < VITA_SRC_NUM; i++)
		{
			s_vp.curBatchSrcQuads[i] = 0;
		}
	}
}

static void VpSeedParticleClasses(void)
{
	if (s_vp.pcSeeded)
	{
		return;
	}
	s_vp.pcSeeded = 1;
#define VP_ADD(arr, nfield, name) \
	do { \
		const ParticleClass *_c = StrParticleClass(&gParticleClasses, name); \
		if (_c != NULL && s_vp.nfield < (int)(sizeof s_vp.arr / sizeof s_vp.arr[0])) \
		{ \
			s_vp.arr[s_vp.nfield++] = _c; \
		} \
	} while (0)
	VP_ADD(pcSmoke, pcSmokeN, "smoke");
	VP_ADD(pcSmoke, pcSmokeN, "smoke_big");
	VP_ADD(pcSmoke, pcSmokeN, "smoke_trail");
	VP_ADD(pcSmoke, pcSmokeN, "smoke_trail_small");
	VP_ADD(pcBlood, pcBloodN, "blood1");
	VP_ADD(pcBlood, pcBloodN, "blood2");
	VP_ADD(pcBlood, pcBloodN, "blood3");
	VP_ADD(pcBrass, pcBrassN, "brass");
	VP_ADD(pcBrass, pcBrassN, "brass_big");
	VP_ADD(pcBrass, pcBrassN, "shotshell");
	VP_ADD(pcFoot, pcFootN, "footprint");
	VP_ADD(pcFoot, pcFootN, "footprint_blood");
	VP_ADD(pcSpall, pcSpallN, "spall1");
	VP_ADD(pcSpall, pcSpallN, "spall2");
	VP_ADD(pcSpall, pcSpallN, "spall3");
	VP_ADD(pcHole, pcHoleN, "bullet_hole");
	VP_ADD(pcFlame, pcFlameN, "fireball_hit");
	VP_ADD(pcFlame, pcFlameN, "fireball_green_hit");
	VP_ADD(pcFlame, pcFlameN, "muzzle_flash_flamer");
	VP_ADD(pcFlame, pcFlameN, "explosion_small");
#undef VP_ADD
}

static int VpClassIn(const ParticleClass *c, const ParticleClass *const *arr, int n)
{
	for (int i = 0; i < n; i++)
	{
		if (arr[i] == c)
		{
			return 1;
		}
	}
	return 0;
}

void VitaProfileParticleClassDrawn(const struct Particle *p)
{
#if !VITA_PROF_IS_DETAILED
	(void)p;
	return;
#endif
	if (p == NULL || p->Class == NULL || !s_vp.inDraw)
	{
		return;
	}
	VpSeedParticleClasses();
	const ParticleClass *c = p->Class;
	if (VpClassIn(c, s_vp.pcSmoke, s_vp.pcSmokeN))
	{
		VitaProfileDrawCount(VITA_DRAW_CNT_PARTICLE_CAT_SMOKE, 1);
	}
	else if (VpClassIn(c, s_vp.pcBlood, s_vp.pcBloodN))
	{
		VitaProfileDrawCount(VITA_DRAW_CNT_PARTICLE_CAT_BLOOD, 1);
	}
	else if (VpClassIn(c, s_vp.pcBrass, s_vp.pcBrassN))
	{
		VitaProfileDrawCount(VITA_DRAW_CNT_PARTICLE_CAT_BRASS, 1);
	}
	else if (VpClassIn(c, s_vp.pcFoot, s_vp.pcFootN))
	{
		VitaProfileDrawCount(VITA_DRAW_CNT_PARTICLE_CAT_FOOTPRINT, 1);
	}
	else if (VpClassIn(c, s_vp.pcSpall, s_vp.pcSpallN))
	{
		VitaProfileDrawCount(VITA_DRAW_CNT_PARTICLE_CAT_SPALL, 1);
	}
	else if (VpClassIn(c, s_vp.pcHole, s_vp.pcHoleN))
	{
		VitaProfileDrawCount(VITA_DRAW_CNT_PARTICLE_CAT_BULLET_HOLE, 1);
	}
	else if (VpClassIn(c, s_vp.pcFlame, s_vp.pcFlameN))
	{
		VitaProfileDrawCount(VITA_DRAW_CNT_PARTICLE_CAT_FLAME, 1);
	}
	else
	{
		VitaProfileDrawCount(VITA_DRAW_CNT_PARTICLE_CAT_OTHER, 1);
	}
}

void VitaProfileNoteSpriteDestBounds(int destX, int destY, int destW, int destH)
{
	if (!s_vp.inDraw || destW <= 0 || destH <= 0)
	{
		return;
	}
	const VitaProfileDrawSource src = s_vp.drawSource;
	/* Eligible = displaylist world sprites only (not terrain/HUD/radar). */
	if (src != VITA_SRC_PARTICLE && src != VITA_SRC_ACTOR &&
		src != VITA_SRC_WORLD_THING)
	{
		return;
	}
	const int vw = gGraphicsDevice.cachedConfig.Res.x;
	const int vh = gGraphicsDevice.cachedConfig.Res.y;
	if (vw <= 0 || vh <= 0)
	{
		return;
	}
	/* Exact axis-aligned dest AABB vs gameplay viewport [0,Res).
	 * Rotated sprites are never passed here (caller gates angle==0). */
	const int overlaps = destX < vw && destX + destW > 0 && destY < vh &&
						 destY + destH > 0;
	const int wouldCull = !overlaps;
	s_vp.spriteBoundsTests++;
	if (wouldCull)
	{
		s_vp.spriteBoundsWouldCull++;
	}
	if (src == VITA_SRC_PARTICLE)
	{
		s_vp.particleBoundsTests++;
		if (wouldCull)
		{
			s_vp.particleBoundsWouldCull++;
		}
	}
	else
	{
		s_vp.nonparticleBoundsTests++;
		if (wouldCull)
		{
			s_vp.nonparticleBoundsWouldCull++;
		}
	}
}

void VitaProfileParticlePrepareEnter(void)
{
	if (!s_vp.inDraw)
	{
		return;
	}
	s_vp.particlePrepareDepth++;
	if (s_vp.particlePrepareDepth == 1)
	{
		s_vp.particlePreparePaused = 0;
		s_vp.particlePrepareStart = SDL_GetPerformanceCounter();
	}
}

void VitaProfileParticlePrepareLeave(void)
{
	if (!s_vp.inDraw || s_vp.particlePrepareDepth <= 0)
	{
		return;
	}
	if (s_vp.particlePrepareDepth == 1 && !s_vp.particlePreparePaused)
	{
		const Uint64 now = SDL_GetPerformanceCounter();
		s_vp.particlePrepareAccum += now - s_vp.particlePrepareStart;
	}
	s_vp.particlePrepareDepth--;
	if (s_vp.particlePrepareDepth == 0)
	{
		s_vp.particlePreparePaused = 0;
	}
}

void VitaProfileParticlePreparePauseForSubmit(void)
{
	if (!s_vp.inDraw || s_vp.particlePrepareDepth <= 0 ||
		s_vp.particlePreparePaused)
	{
		return;
	}
	const Uint64 now = SDL_GetPerformanceCounter();
	s_vp.particlePrepareAccum += now - s_vp.particlePrepareStart;
	s_vp.particlePreparePaused = 1;
}

void VitaProfileParticlePrepareResumeAfterSubmit(void)
{
	if (!s_vp.inDraw || s_vp.particlePrepareDepth <= 0 ||
		!s_vp.particlePreparePaused)
	{
		return;
	}
	s_vp.particlePrepareStart = SDL_GetPerformanceCounter();
	s_vp.particlePreparePaused = 0;
}

void VitaProfileTexRenderBegin(void)
{
	s_vp.texRenderDepth++;
	if (s_vp.texRenderDepth == 1)
	{
		s_vp.texRenderStart = SDL_GetPerformanceCounter();
	}
}

void VitaProfileTexRenderEnd(void)
{
	if (s_vp.texRenderDepth <= 0)
	{
		return;
	}
	if (s_vp.texRenderDepth == 1)
	{
		const Uint64 now = SDL_GetPerformanceCounter();
		s_vp.texRenderAccum += now - s_vp.texRenderStart;
	}
	s_vp.texRenderDepth--;
}

void VitaProfileTexAppendBegin(void)
{
	s_vp.texAppendDepth++;
	if (s_vp.texAppendDepth == 1)
	{
		s_vp.texAppendStart = SDL_GetPerformanceCounter();
	}
}

void VitaProfileTexAppendEnd(void)
{
	if (s_vp.texAppendDepth <= 0)
	{
		return;
	}
	if (s_vp.texAppendDepth == 1)
	{
		const Uint64 now = SDL_GetPerformanceCounter();
		s_vp.texAppendAccum += now - s_vp.texAppendStart;
	}
	s_vp.texAppendDepth--;
}

void VitaProfileTexFlushBegin(void)
{
	s_vp.texFlushDepth++;
	if (s_vp.texFlushDepth == 1)
	{
		s_vp.texFlushStart = SDL_GetPerformanceCounter();
	}
}

void VitaProfileTexFlushEnd(void)
{
	if (s_vp.texFlushDepth <= 0)
	{
		return;
	}
	if (s_vp.texFlushDepth == 1)
	{
		const Uint64 now = SDL_GetPerformanceCounter();
		s_vp.texFlushAccum += now - s_vp.texFlushStart;
	}
	s_vp.texFlushDepth--;
}

void VitaProfileTexQueryInc(void)
{
	s_vp.texQueryCalls++;
}

void VitaProfileTexAlphaQueryInc(void)
{
	s_vp.texAlphaQueryCalls++;
}

#endif /* CDOGS_VITA_PROFILE */
