/*
	Runtime particle texture atlas — shelf packing into shared SDL textures.
*/
#include "particle_atlas.h"

#ifdef CDOGS_PARTICLE_ATLAS

#include <stdlib.h>
#include <string.h>

#include "grafx.h"
#include "log.h"
#include "texture.h"
#include "utils.h"

#define PARTICLE_ATLAS_PAD 1
#define PARTICLE_ATLAS_MAX_PAGES 4

typedef struct
{
	Pic *pic;
	struct vec2i size;
} AtlasCandidate;

typedef struct
{
	SDL_Texture *tex;
	int w;
	int h;
} AtlasPage;

static AtlasPage s_pages[PARTICLE_ATLAS_MAX_PAGES];
static int s_pageCount;

static int CandidateHeightCmp(const void *a, const void *b)
{
	const AtlasCandidate *ca = a;
	const AtlasCandidate *cb = b;
	if (cb->size.y != ca->size.y)
	{
		return cb->size.y - ca->size.y;
	}
	return cb->size.x - ca->size.x;
}

static bool NameIsParticle(const char *name)
{
	return name != NULL && strncmp(name, "particles/", 10) == 0;
}

typedef struct
{
	AtlasCandidate *items;
	int count;
	int cap;
} CandidateList;

static struct vec2i AtlasPicPixelSize(const Pic *p)
{
	if (p->isHD)
	{
		return svec2i_scale(p->size, 2);
	}
	return p->size;
}

static void CandidatesPush(CandidateList *list, Pic *pic)
{
	if (pic == NULL || PicIsNone(pic) || pic->Data == NULL)
	{
		return;
	}
	if (list->count >= list->cap)
	{
		const int ncap = list->cap > 0 ? list->cap * 2 : 64;
		AtlasCandidate *nitems =
			(AtlasCandidate *)realloc(list->items, (size_t)ncap * sizeof *nitems);
		if (nitems == NULL)
		{
			return;
		}
		list->items = nitems;
		list->cap = ncap;
	}
	list->items[list->count].pic = pic;
	list->items[list->count].size = AtlasPicPixelSize(pic);
	list->count++;
}

static int CollectNamedPic(any_t data, any_t item)
{
	CandidateList *list = data;
	NamedPic *n = item;
	if (NameIsParticle(n->name))
	{
		CandidatesPush(list, &n->pic);
	}
	return MAP_OK;
}

static int CollectNamedSprites(any_t data, any_t item)
{
	CandidateList *list = data;
	NamedSprites *n = item;
	if (!NameIsParticle(n->name))
	{
		return MAP_OK;
	}
	CA_FOREACH(Pic, p, n->pics)
	CandidatesPush(list, p);
	CA_FOREACH_END()
	return MAP_OK;
}

static void DestroyPages(void)
{
	for (int i = 0; i < s_pageCount; i++)
	{
		if (s_pages[i].tex != NULL)
		{
			SDL_DestroyTexture(s_pages[i].tex);
			s_pages[i].tex = NULL;
		}
	}
	s_pageCount = 0;
}

void ParticleAtlasDestroy(void)
{
	DestroyPages();
}

static int MaxTexDim(SDL_Renderer *r)
{
	SDL_RendererInfo info;
	int maxDim = 2048;
	if (SDL_GetRendererInfo(r, &info) == 0)
	{
		if (info.max_texture_width > 0 && info.max_texture_height > 0)
		{
			maxDim = info.max_texture_width < info.max_texture_height
						 ? info.max_texture_width
						 : info.max_texture_height;
		}
	}
	if (maxDim > 2048)
	{
		maxDim = 2048;
	}
	if (maxDim < 256)
	{
		maxDim = 256;
	}
	return maxDim;
}

static bool PlaceOnPage(
	AtlasPage *page, const struct vec2i size, int *shelfY, int *shelfH,
	int *cursorX, Rect2i *out)
{
	const int pad = PARTICLE_ATLAS_PAD;
	const int w = size.x;
	const int h = size.y;
	if (w + pad > page->w || h + pad > page->h)
	{
		return false;
	}
	if (*cursorX + w + pad > page->w)
	{
		*shelfY += *shelfH + pad;
		*cursorX = 0;
		*shelfH = 0;
	}
	if (*shelfY + h + pad > page->h)
	{
		return false;
	}
	out->Pos = svec2i(*cursorX, *shelfY);
	out->Size = size;
	*cursorX += w + pad;
	if (h > *shelfH)
	{
		*shelfH = h;
	}
	return true;
}

static AtlasPage *CreatePage(SDL_Renderer *r, const int dim)
{
	if (s_pageCount >= PARTICLE_ATLAS_MAX_PAGES)
	{
		return NULL;
	}
	AtlasPage *page = &s_pages[s_pageCount];
	page->w = dim;
	page->h = dim;
	page->tex = TextureCreate(
		r, SDL_TEXTUREACCESS_STATIC, svec2i(dim, dim), SDL_BLENDMODE_BLEND,
		255);
	if (page->tex == NULL)
	{
		return NULL;
	}
	/* Clear to transparent */
	void *pixels = calloc((size_t)dim * (size_t)dim, sizeof(Uint32));
	if (pixels != NULL)
	{
		SDL_UpdateTexture(page->tex, NULL, pixels, dim * (int)sizeof(Uint32));
		free(pixels);
	}
	s_pageCount++;
	return page;
}

void ParticleAtlasBuild(PicManager *pm)
{
	ParticleAtlasDestroy();
	if (pm == NULL || gGraphicsDevice.gameWindow.renderer == NULL)
	{
		return;
	}

	CandidateList list = {0};
	hashmap_iterate(pm->pics, CollectNamedPic, &list);
	hashmap_iterate(pm->customPics, CollectNamedPic, &list);
	hashmap_iterate(pm->sprites, CollectNamedSprites, &list);
	hashmap_iterate(pm->customSprites, CollectNamedSprites, &list);

	if (list.count == 0)
	{
		free(list.items);
		return;
	}

	qsort(list.items, (size_t)list.count, sizeof(AtlasCandidate),
		CandidateHeightCmp);

	SDL_Renderer *r = gGraphicsDevice.gameWindow.renderer;
	const int maxDim = MaxTexDim(r);
	int dim = 512;
	while (dim < maxDim)
	{
		dim *= 2;
	}
	if (dim > maxDim)
	{
		dim = maxDim;
	}

	AtlasPage *page = CreatePage(r, dim);
	if (page == NULL)
	{
		LOG(LM_GFX, LL_ERROR, "particle atlas: cannot create page");
		free(list.items);
		return;
	}
	int shelfY = 0;
	int shelfH = 0;
	int cursorX = 0;
	int placed = 0;

	for (int i = 0; i < list.count; i++)
	{
		Pic *pic = list.items[i].pic;
		const struct vec2i psz = list.items[i].size;
		Rect2i dest = Rect2iZero();
		bool ok = PlaceOnPage(page, psz, &shelfY, &shelfH, &cursorX, &dest);
		if (!ok)
		{
			page = CreatePage(r, dim);
			if (page == NULL)
			{
				LOG(LM_GFX, LL_WARN,
					"particle atlas: out of pages; %d/%d placed", placed,
					list.count);
				break;
			}
			shelfY = 0;
			shelfH = 0;
			cursorX = 0;
			ok = PlaceOnPage(page, psz, &shelfY, &shelfH, &cursorX, &dest);
			if (!ok)
			{
				LOG(LM_GFX, LL_WARN,
					"particle atlas: pic %dx%d exceeds page; leaving unique tex",
					psz.x, psz.y);
				continue;
			}
		}

		const SDL_Rect rect = {
			dest.Pos.x, dest.Pos.y, dest.Size.x, dest.Size.y};
		if (SDL_UpdateTexture(
				page->tex, &rect, pic->Data,
				psz.x * (int)sizeof(Uint32)) != 0)
		{
			LOG(LM_GFX, LL_ERROR, "particle atlas UpdateTexture: %s",
				SDL_GetError());
			continue;
		}

		if (pic->ownsTex && pic->Tex != NULL)
		{
			SDL_DestroyTexture(pic->Tex);
		}
		pic->Tex = page->tex;
		pic->ownsTex = false;
		pic->texSrc = dest;
		placed++;
	}

	LOG(LM_GFX, LL_INFO,
		"particle atlas: placed %d/%d pics on %d page(s) (%dx%d)", placed,
		list.count, s_pageCount, dim, dim);
	free(list.items);
}

#endif /* CDOGS_PARTICLE_ATLAS */
