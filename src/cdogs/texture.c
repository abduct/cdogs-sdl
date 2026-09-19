/*
 Copyright (c) 2017-2019, 2026 Cong Xu
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
#include "texture.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Reusable order-preserving sprite geometry batch.
 * Compatibility key: (renderer, texture, blend). Consecutive compatible
 * TextureRender calls accumulate quads; incompatible state or TextureFlush
 * submits via SDL_RenderGeometry. */
#define TEX_BATCH_INIT_QUADS 256
#define TEX_BATCH_MAX_QUADS 8192

typedef struct
{
	SDL_Renderer *renderer;
	SDL_Texture *texture;
	SDL_BlendMode blend;
	/* Underlying SDL_Texture pixel size for UV / full-src clip. Valid when
	 * dimsTexture == texture and texW/texH > 0. Not Pic.size (atlas-safe). */
	SDL_Texture *dimsTexture;
	int texW;
	int texH;
	SDL_Vertex *verts;
	int *indices;
	int quadCount;
	int quadCapacity;
} TextureBatch;

static TextureBatch s_batch;

static void TextureBatchEnsureCapacity(const int quadsNeeded)
{
	if (quadsNeeded <= s_batch.quadCapacity)
	{
		return;
	}
	int cap = s_batch.quadCapacity > 0 ? s_batch.quadCapacity : TEX_BATCH_INIT_QUADS;
	while (cap < quadsNeeded)
	{
		cap *= 2;
	}
	if (cap > TEX_BATCH_MAX_QUADS)
	{
		cap = TEX_BATCH_MAX_QUADS;
	}
	SDL_Vertex *verts =
		(SDL_Vertex *)realloc(s_batch.verts, (size_t)cap * 4 * sizeof(SDL_Vertex));
	int *indices = (int *)realloc(s_batch.indices, (size_t)cap * 6 * sizeof(int));
	if (verts == NULL || indices == NULL)
	{
		LOG(LM_GFX, LL_ERROR, "texture batch realloc failed");
		/* realloc failed: leave previous buffers intact */
		return;
	}
	s_batch.verts = verts;
	s_batch.indices = indices;
	s_batch.quadCapacity = cap;
}

/* Query underlying texture dims once per TextureBatch texture identity.
 * Flush does not invalidate dims; texture pointer change / ResetKey does. */
static int TextureBatchEnsureDims(SDL_Texture *t)
{
	if (t != NULL && s_batch.dimsTexture == t && s_batch.texW > 0 &&
		s_batch.texH > 0)
	{
		return 1;
	}
	int w = 0;
	int h = 0;
	if (t == NULL || SDL_QueryTexture(t, NULL, NULL, &w, &h) != 0 || w <= 0 ||
		h <= 0)
	{
		LOG(LM_MAIN, LL_ERROR, "QueryTexture failed: %s", SDL_GetError());
		s_batch.dimsTexture = NULL;
		s_batch.texW = 0;
		s_batch.texH = 0;
		return 0;
	}
	s_batch.dimsTexture = t;
	s_batch.texW = w;
	s_batch.texH = h;
	return 1;
}

void TextureFlush(void)
{
	if (s_batch.quadCount <= 0 || s_batch.renderer == NULL ||
		s_batch.texture == NULL)
	{
		s_batch.quadCount = 0;
		return;
	}

	const int quads = s_batch.quadCount;
	const int nverts = quads * 4;
	const int nindices = quads * 6;
	if (SDL_RenderGeometry(
			s_batch.renderer, s_batch.texture, s_batch.verts, nverts,
			s_batch.indices, nindices) != 0)
	{
		LOG(LM_MAIN, LL_ERROR, "SDL_RenderGeometry failed: %s",
			SDL_GetError());
	}
	s_batch.quadCount = 0;
}


static void TextureBatchResetKey(void)
{
	s_batch.renderer = NULL;
	s_batch.texture = NULL;
	s_batch.blend = SDL_BLENDMODE_NONE;
	s_batch.dimsTexture = NULL;
	s_batch.texW = 0;
	s_batch.texH = 0;
}

SDL_Texture *TextureCreate(
	SDL_Renderer *renderer, const SDL_TextureAccess access, const struct vec2i res,
	const SDL_BlendMode blend, const Uint8 alpha)
{
	// Don't create 0 sized textures
	if (svec2i_is_zero(res))
	{
		LOG(LM_GFX, LL_ERROR, "attempting to create 0 sized texture");
		return NULL;
	}
	SDL_Texture *t = SDL_CreateTexture(
		renderer, SDL_PIXELFORMAT_ARGB8888, access, res.x, res.y);
	if (t == NULL)
	{
		LOG(LM_GFX, LL_ERROR, "cannot create texture: %s", SDL_GetError());
		return NULL;
	}
	if (SDL_SetTextureBlendMode(t, blend) != 0)
	{
		LOG(LM_GFX, LL_ERROR, "cannot set blend mode: %s", SDL_GetError());
		return NULL;
	}
	if (SDL_SetTextureAlphaMod(t, alpha) != 0)
	{
		LOG(LM_GFX, LL_ERROR, "cannot set texture alpha: %s", SDL_GetError());
		return NULL;
	}
	return t;
}

/*
	Append one textured quad matching SDL 2.32.8 SDL_RenderCopyExF geometry
	path (release-2.32.8 src/render/SDL_render.c). Flip swaps destination
	min/max edges; UVs stay TL/TR/BR/BL of the source rect. Rotation uses
	center = dest origin + (w/2,h/2) when center is NULL (TextureRender).
*/
static void TextureBatchAppendQuad(
	SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *srcRect,
	const SDL_FRect *dstRect, const color_t mask, const double angle,
	const SDL_RendererFlip flip)
{
	if (!TextureBatchEnsureDims(t))
	{
		return;
	}
	const int texW = s_batch.texW;
	const int texH = s_batch.texH;

	SDL_Rect realSrc = {0, 0, texW, texH};
	if (srcRect != NULL)
	{
		const SDL_Rect full = {0, 0, texW, texH};
		if (!SDL_IntersectRect(srcRect, &full, &realSrc))
		{
			return;
		}
	}

	SDL_FRect realDst;
	if (dstRect != NULL)
	{
		realDst = *dstRect;
	}
	else
	{
		/* Match SDL RenderGetViewportSize: origin at (0,0), viewport size. */
		SDL_Rect vp;
		SDL_RenderGetViewport(r, &vp);
		realDst.x = 0.0f;
		realDst.y = 0.0f;
		realDst.w = (float)vp.w;
		realDst.h = (float)vp.h;
	}

	if (s_batch.quadCount >= s_batch.quadCapacity)
	{
		if (s_batch.quadCapacity >= TEX_BATCH_MAX_QUADS)
		{
			TextureFlush();
		}
		else
		{
			TextureBatchEnsureCapacity(s_batch.quadCount + 1);
			if (s_batch.quadCount >= s_batch.quadCapacity)
			{
				TextureFlush();
			}
		}
	}
	if (s_batch.quadCapacity <= 0)
	{
		TextureBatchEnsureCapacity(TEX_BATCH_INIT_QUADS);
		if (s_batch.quadCapacity <= 0)
		{
			return;
		}
	}

	const float minu = (float)realSrc.x / (float)texW;
	const float minv = (float)realSrc.y / (float)texH;
	const float maxu = (float)(realSrc.x + realSrc.w) / (float)texW;
	const float maxv = (float)(realSrc.y + realSrc.h) / (float)texH;

	/* Match old TextureRender CopyEx semantics: ColorMod always = mask.rgb;
	 * AlphaMod is set to mask.a only when mask.a < 255, otherwise the
	 * texture's existing AlphaMod is left unchanged. Geometry ignores
	 * texture mods, so fold that into vertex color. */
	Uint8 effectiveAlpha = mask.a;
	if (mask.a >= 255)
	{
		if (SDL_GetTextureAlphaMod(t, &effectiveAlpha) != 0)
		{
			effectiveAlpha = 255;
		}
	}
	const SDL_Color col = {mask.r, mask.g, mask.b, effectiveAlpha};
	const int base = s_batch.quadCount * 4;
	SDL_Vertex *v = &s_batch.verts[base];

	if (angle == 0.0 && flip == SDL_FLIP_NONE)
	{
		/* Axis-aligned: equivalent to general path with s=0,c=1 and no flip.
		 * General TL/TR/BR/BL collapse to dest corners; UVs unchanged. */
		const float minx = realDst.x;
		const float maxx = realDst.x + realDst.w;
		const float miny = realDst.y;
		const float maxy = realDst.y + realDst.h;
		/* TL */ v[0].position.x = minx;
		v[0].position.y = miny;
		v[0].tex_coord.x = minu;
		v[0].tex_coord.y = minv;
		v[0].color = col;
		/* TR */ v[1].position.x = maxx;
		v[1].position.y = miny;
		v[1].tex_coord.x = maxu;
		v[1].tex_coord.y = minv;
		v[1].color = col;
		/* BR */ v[2].position.x = maxx;
		v[2].position.y = maxy;
		v[2].tex_coord.x = maxu;
		v[2].tex_coord.y = maxv;
		v[2].color = col;
		/* BL */ v[3].position.x = minx;
		v[3].position.y = maxy;
		v[3].tex_coord.x = minu;
		v[3].tex_coord.y = maxv;
		v[3].color = col;
	}
	else
	{
		const float centerx = realDst.w * 0.5f + realDst.x;
		const float centery = realDst.h * 0.5f + realDst.y;

		float minx, maxx, miny, maxy;
		if (flip & SDL_FLIP_HORIZONTAL)
		{
			minx = realDst.x + realDst.w;
			maxx = realDst.x;
		}
		else
		{
			minx = realDst.x;
			maxx = realDst.x + realDst.w;
		}
		if (flip & SDL_FLIP_VERTICAL)
		{
			miny = realDst.y + realDst.h;
			maxy = realDst.y;
		}
		else
		{
			miny = realDst.y;
			maxy = realDst.y + realDst.h;
		}

		const float radian_angle = (float)((M_PI * angle) / 180.0);
		const float s = sinf(radian_angle);
		const float c = cosf(radian_angle);

		const float s_minx = s * (minx - centerx);
		const float s_miny = s * (miny - centery);
		const float s_maxx = s * (maxx - centerx);
		const float s_maxy = s * (maxy - centery);
		const float c_minx = c * (minx - centerx);
		const float c_miny = c * (miny - centery);
		const float c_maxx = c * (maxx - centerx);
		const float c_maxy = c * (maxy - centery);

		/* TL */ v[0].position.x = (c_minx - s_miny) + centerx;
		v[0].position.y = (s_minx + c_miny) + centery;
		v[0].tex_coord.x = minu;
		v[0].tex_coord.y = minv;
		v[0].color = col;
		/* TR */ v[1].position.x = (c_maxx - s_miny) + centerx;
		v[1].position.y = (s_maxx + c_miny) + centery;
		v[1].tex_coord.x = maxu;
		v[1].tex_coord.y = minv;
		v[1].color = col;
		/* BR */ v[2].position.x = (c_maxx - s_maxy) + centerx;
		v[2].position.y = (s_maxx + c_maxy) + centery;
		v[2].tex_coord.x = maxu;
		v[2].tex_coord.y = maxv;
		v[2].color = col;
		/* BL */ v[3].position.x = (c_minx - s_maxy) + centerx;
		v[3].position.y = (s_minx + c_maxy) + centery;
		v[3].tex_coord.x = minu;
		v[3].tex_coord.y = maxv;
		v[3].color = col;
	}

	int *idx = &s_batch.indices[s_batch.quadCount * 6];
	idx[0] = base + 0;
	idx[1] = base + 1;
	idx[2] = base + 2;
	idx[3] = base + 0;
	idx[4] = base + 2;
	idx[5] = base + 3;

	s_batch.quadCount++;
}

void TextureRender(
	SDL_Texture *t, SDL_Renderer *r, const Rect2i src, const Rect2i dest,
	const color_t mask, const double angle, const SDL_RendererFlip flip)
{
	if (t == NULL || r == NULL)
	{
		return;
	}

	SDL_BlendMode blend = SDL_BLENDMODE_BLEND;
	if (SDL_GetTextureBlendMode(t, &blend) != 0)
	{
		blend = SDL_BLENDMODE_BLEND;
	}

	if (s_batch.quadCount > 0 &&
		(s_batch.renderer != r || s_batch.texture != t ||
		 s_batch.blend != blend))
	{
		TextureFlush();
		TextureBatchResetKey();
	}

	s_batch.renderer = r;
	s_batch.texture = t;
	s_batch.blend = blend;

	const SDL_Rect srcRect = {src.Pos.x, src.Pos.y, src.Size.x, src.Size.y};
	const SDL_Rect *srcP = Rect2iIsZero(src) ? NULL : &srcRect;
	SDL_FRect dstF;
	const SDL_FRect *dstP;
	if (Rect2iIsZero(dest))
	{
		dstP = NULL;
	}
	else
	{
		dstF.x = (float)dest.Pos.x;
		dstF.y = (float)dest.Pos.y;
		dstF.w = (float)dest.Size.x;
		dstF.h = (float)dest.Size.y;
		dstP = &dstF;
	}

	TextureBatchAppendQuad(r, t, srcP, dstP, mask, angle, flip);
}
