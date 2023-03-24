// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 1999-2022 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file hw_segs.c
/// \brief Processing segs, rendering walls

#ifdef HWRENDER
#include "hw_glob.h"
#include "hw_batching.h"
#include "../p_slopes.h"
#include "../r_local.h"

static FUINT HWR_CalcWallLight(FUINT lightnum, fixed_t v1x, fixed_t v1y, fixed_t v2x, fixed_t v2y)
{
	INT16 finallight = lightnum;

	if (cv_glfakecontrast.value != 0)
	{
		const UINT8 contrast = 8;
		fixed_t extralight = 0;

		if (cv_glfakecontrast.value == 2) // Smooth setting
		{
			extralight = (-(contrast<<FRACBITS) +
			FixedDiv(AngleFixed(R_PointToAngle2(0, 0,
				abs(v1x - v2x),
				abs(v1y - v2y))), 90<<FRACBITS)
			* (contrast * 2)) >> FRACBITS;
		}
		else
		{
			if (v1y == v2y)
				extralight = -contrast;
			else if (v1x == v2x)
				extralight = contrast;
		}

		if (extralight != 0)
		{
			finallight += extralight;

			if (finallight < 0)
				finallight = 0;
			if (finallight > 255)
				finallight = 255;
		}
	}

	return (FUINT)finallight;
}

// ==========================================================================
// Wall generation from subsector segs
// ==========================================================================

/*
   wallVerts order is :
		3--2
		| /|
		|/ |
		0--1
*/

static void HWR_ProjectWall(FOutVector *wallVerts, FSurfaceInfo *pSurf, FBITFIELD blendmode, INT32 lightlevel, extracolormap_t *wallcolormap)
{
	INT32 shader = -1;

	HWR_Lighting(pSurf, lightlevel, wallcolormap);

	if (HWR_UseShader())
	{
		shader = SHADER_WALL;
		blendmode |= PF_ColorMapped;
	}

	// don't draw to color buffer when drawing to stencil
	if (gl_drawing_stencil)
	{
		blendmode |= PF_Invisible|PF_NoAlphaTest; // TODO not sure if any others than PF_Invisible are needed??
		blendmode &= ~PF_Masked;
	}

	HWR_ProcessPolygon(pSurf, wallVerts, 4, blendmode|PF_Modulated|PF_Occlude, shader, false);
}

static void HWR_SplitWall(sector_t *sector, FOutVector *wallVerts, INT32 texnum, FSurfaceInfo* Surf, INT32 cutflag, ffloor_t *pfloor, FBITFIELD polyflags)
{
	/* SoM: split up and light walls according to the
	 lightlist. This may also include leaving out parts
	 of the wall that can't be seen */

	float realtop, realbot, top, bot;
	float pegt, pegb, pegmul;
	float height = 0.0f, bheight = 0.0f;

	float endrealtop, endrealbot, endtop, endbot;
	float endpegt, endpegb, endpegmul;
	float endheight = 0.0f, endbheight = 0.0f;

	// compiler complains when P_GetSlopeZAt is used in FLOAT_TO_FIXED directly
	// use this as a temp var to store P_GetSlopeZAt's return value each time
	fixed_t temp;

	fixed_t v1x = FLOAT_TO_FIXED(wallVerts[0].x);
	fixed_t v1y = FLOAT_TO_FIXED(wallVerts[0].z); // not a typo
	fixed_t v2x = FLOAT_TO_FIXED(wallVerts[1].x);
	fixed_t v2y = FLOAT_TO_FIXED(wallVerts[1].z); // not a typo

	INT32 solid, i;
	lightlist_t *  list = sector->lightlist;
	const UINT8 alpha = Surf->PolyColor.s.alpha;
	FUINT lightnum = HWR_CalcWallLight(sector->lightlevel, v1x, v1y, v2x, v2y);
	extracolormap_t *colormap = NULL;

	realtop = top = wallVerts[3].y;
	realbot = bot = wallVerts[0].y;
	pegt = wallVerts[3].t;
	pegb = wallVerts[0].t;
	pegmul = (pegb - pegt) / (top - bot);

	endrealtop = endtop = wallVerts[2].y;
	endrealbot = endbot = wallVerts[1].y;
	endpegt = wallVerts[2].t;
	endpegb = wallVerts[1].t;
	endpegmul = (endpegb - endpegt) / (endtop - endbot);

	for (i = 0; i < sector->numlights; i++)
	{
		if (endtop < endrealbot && top < realbot)
			return;

		if (!(list[i].flags & FF_NOSHADE))
		{
			if (pfloor && (pfloor->flags & FF_FOG))
			{
				lightnum = pfloor->master->frontsector->lightlevel;
				colormap = pfloor->master->frontsector->extra_colormap;
				lightnum = colormap ? lightnum : HWR_CalcWallLight(lightnum, v1x, v1y, v2x, v2y);
			}
			else
			{
				lightnum = *list[i].lightlevel;
				colormap = *list[i].extra_colormap;
				lightnum = colormap ? lightnum : HWR_CalcWallLight(lightnum, v1x, v1y, v2x, v2y);
			}
		}

		solid = false;

		if ((sector->lightlist[i].flags & FF_CUTSOLIDS) && !(cutflag & FF_EXTRA))
			solid = true;
		else if ((sector->lightlist[i].flags & FF_CUTEXTRA) && (cutflag & FF_EXTRA))
		{
			if (sector->lightlist[i].flags & FF_EXTRA)
			{
				if ((sector->lightlist[i].flags & (FF_FOG|FF_SWIMMABLE)) == (cutflag & (FF_FOG|FF_SWIMMABLE))) // Only merge with your own types
					solid = true;
			}
			else
				solid = true;
		}
		else
			solid = false;

		temp = P_GetLightZAt(&list[i], v1x, v1y);
		height = FIXED_TO_FLOAT(temp);
		temp = P_GetLightZAt(&list[i], v2x, v2y);
		endheight = FIXED_TO_FLOAT(temp);
		if (solid)
		{
			temp = P_GetFFloorBottomZAt(list[i].caster, v1x, v1y);
			bheight = FIXED_TO_FLOAT(temp);
			temp = P_GetFFloorBottomZAt(list[i].caster, v2x, v2y);
			endbheight = FIXED_TO_FLOAT(temp);
		}

		if (endheight >= endtop && height >= top)
		{
			if (solid && top > bheight)
				top = bheight;
			if (solid && endtop > endbheight)
				endtop = endbheight;
		}

		if (i + 1 < sector->numlights)
		{
			temp = P_GetLightZAt(&list[i+1], v1x, v1y);
			bheight = FIXED_TO_FLOAT(temp);
			temp = P_GetLightZAt(&list[i+1], v2x, v2y);
			endbheight = FIXED_TO_FLOAT(temp);
		}
		else
		{
			bheight = realbot;
			endbheight = endrealbot;
		}

		if (endbheight >= endtop && bheight >= top)
			continue;

		//Found a break;
		bot = bheight;

		if (bot < realbot)
			bot = realbot;

		endbot = endbheight;

		if (endbot < endrealbot)
			endbot = endrealbot;

		Surf->PolyColor.s.alpha = alpha;

		wallVerts[3].t = pegt + ((realtop - top) * pegmul);
		wallVerts[2].t = endpegt + ((endrealtop - endtop) * endpegmul);
		wallVerts[0].t = pegt + ((realtop - bot) * pegmul);
		wallVerts[1].t = endpegt + ((endrealtop - endbot) * endpegmul);

		// set top/bottom coords
		wallVerts[3].y = top;
		wallVerts[2].y = endtop;
		wallVerts[0].y = bot;
		wallVerts[1].y = endbot;

		if (cutflag & FF_FOG)
			HWR_AddTransparentWall(wallVerts, Surf, texnum, PF_Fog|PF_NoTexture|polyflags, true, lightnum, colormap);
		else if (polyflags & (PF_Translucent|PF_Additive|PF_Subtractive|PF_ReverseSubtract|PF_Multiplicative|PF_Environment))
			HWR_AddTransparentWall(wallVerts, Surf, texnum, polyflags, false, lightnum, colormap);
		else
			HWR_ProjectWall(wallVerts, Surf, PF_Masked|polyflags, lightnum, colormap);

		top = bot;
		endtop = endbot;
	}

	bot = realbot;
	endbot = endrealbot;
	if (endtop <= endrealbot && top <= realbot)
		return;

	Surf->PolyColor.s.alpha = alpha;

	wallVerts[3].t = pegt + ((realtop - top) * pegmul);
	wallVerts[2].t = endpegt + ((endrealtop - endtop) * endpegmul);
	wallVerts[0].t = pegt + ((realtop - bot) * pegmul);
	wallVerts[1].t = endpegt + ((endrealtop - endbot) * endpegmul);

	// set top/bottom coords
	wallVerts[3].y = top;
	wallVerts[2].y = endtop;
	wallVerts[0].y = bot;
	wallVerts[1].y = endbot;

	if (cutflag & FF_FOG)
		HWR_AddTransparentWall(wallVerts, Surf, texnum, PF_Fog|PF_NoTexture|polyflags, true, lightnum, colormap);
	else if (polyflags & (PF_Translucent|PF_Additive|PF_Subtractive|PF_ReverseSubtract|PF_Multiplicative|PF_Environment))
		HWR_AddTransparentWall(wallVerts, Surf, texnum, polyflags, false, lightnum, colormap);
	else
		HWR_ProjectWall(wallVerts, Surf, PF_Masked|polyflags, lightnum, colormap);
}

// HWR_DrawSkyWall
// Draw walls into the depth buffer so that anything behind is culled properly
static void HWR_DrawSkyWall(FOutVector *wallVerts, FSurfaceInfo *Surf)
{
	if (cv_glskydebug.value)
	{
		wallVerts[3].t = wallVerts[2].t = 4;
		wallVerts[0].t = wallVerts[1].t = 0;
		wallVerts[0].s = wallVerts[3].s = 0.1;
		wallVerts[2].s = wallVerts[1].s = 0;
		HWR_GetTexture(cv_glskydebug.value);
		HWR_ProjectWall(wallVerts, Surf, 0, 255, NULL);
		return;
	}

	HWR_SetCurrentTexture(NULL);
	// no texture
	wallVerts[3].t = wallVerts[2].t = 0;
	wallVerts[0].t = wallVerts[1].t = 0;
	wallVerts[0].s = wallVerts[3].s = 0;
	wallVerts[2].s = wallVerts[1].s = 0;
	// this no longer sets top/bottom coords, this should be done before caling the function
	HWR_ProjectWall(wallVerts, Surf, PF_Invisible|PF_NoTexture, 255, NULL);
	// PF_Invisible so it's not drawn into the colour buffer
	// PF_NoTexture for no texture
	// PF_Occlude is set in HWR_ProjectWall to draw into the depth buffer
}

// struct for HWR_ProcessSeg
typedef struct
{
	fixed_t worldtop, worldbottom;
	fixed_t worldhigh, worldlow;
	fixed_t worldtopslope, worldbottomslope;
	fixed_t worldhighslope, worldlowslope;
} gl_seg_bounds;

#define SLOPEPARAMS(slope, end1, end2, normalheight) \
	end1 = P_GetZAt(slope, v1x, v1y, normalheight); \
	end2 = P_GetZAt(slope, v2x, v2y, normalheight);

static void HWR_ProcessTwoSidedSegTop(FOutVector *wallVerts, gl_seg_bounds *b,
		float cliplow, float cliphigh, FUINT lightnum, INT32 gl_toptexture)
{
	FSurfaceInfo Surf;
	GLMapTexture_t *grTex = HWR_GetTexture(gl_toptexture);

	fixed_t texturevpegtop;

	Surf.PolyColor.s.alpha = 255;

	// PEGGING
	if (gl_linedef->flags & ML_DONTPEGTOP)
		texturevpegtop = 0;
	else if (gl_linedef->flags & ML_EFFECT1)
		texturevpegtop = b->worldhigh + textureheight[gl_sidedef->toptexture] - b->worldtop;
	else
		texturevpegtop = gl_backsector->ceilingheight + textureheight[gl_sidedef->toptexture] - gl_frontsector->ceilingheight;

	texturevpegtop += gl_sidedef->rowoffset;

	// This is so that it doesn't overflow and screw up the wall, it doesn't need to go higher than the texture's height anyway
	texturevpegtop %= (textures[gl_toptexture]->height)<<FRACBITS;

	wallVerts[3].t = wallVerts[2].t = texturevpegtop * grTex->scaleY;
	wallVerts[0].t = wallVerts[1].t = (texturevpegtop + gl_frontsector->ceilingheight - gl_backsector->ceilingheight) * grTex->scaleY;
	wallVerts[0].s = wallVerts[3].s = cliplow * grTex->scaleX;
	wallVerts[2].s = wallVerts[1].s = cliphigh * grTex->scaleX;

	// Adjust t value for sloped walls
	if (!(gl_linedef->flags & ML_EFFECT1))
	{
		// Unskewed
		wallVerts[3].t -= (b->worldtop - gl_frontsector->ceilingheight) * grTex->scaleY;
		wallVerts[2].t -= (b->worldtopslope - gl_frontsector->ceilingheight) * grTex->scaleY;
		wallVerts[0].t -= (b->worldhigh - gl_backsector->ceilingheight) * grTex->scaleY;
		wallVerts[1].t -= (b->worldhighslope - gl_backsector->ceilingheight) * grTex->scaleY;
	}
	else if (gl_linedef->flags & ML_DONTPEGTOP)
	{
		// Skewed by top
		wallVerts[0].t = (texturevpegtop + b->worldtop - b->worldhigh) * grTex->scaleY;
		wallVerts[1].t = (texturevpegtop + b->worldtopslope - b->worldhighslope) * grTex->scaleY;
	}
	else
	{
		// Skewed by bottom
		wallVerts[0].t = wallVerts[1].t = (texturevpegtop + b->worldtop - b->worldhigh) * grTex->scaleY;
		wallVerts[3].t = wallVerts[0].t - (b->worldtop - b->worldhigh) * grTex->scaleY;
		wallVerts[2].t = wallVerts[1].t - (b->worldtopslope - b->worldhighslope) * grTex->scaleY;
	}

	// set top/bottom coords
	wallVerts[3].y = FIXED_TO_FLOAT(b->worldtop);
	wallVerts[0].y = FIXED_TO_FLOAT(b->worldhigh);
	wallVerts[2].y = FIXED_TO_FLOAT(b->worldtopslope);
	wallVerts[1].y = FIXED_TO_FLOAT(b->worldhighslope);

	if (!gl_drawing_stencil && gl_frontsector->numlights)
		HWR_SplitWall(gl_frontsector, wallVerts, gl_toptexture, &Surf, FF_CUTLEVEL, NULL, 0);
	else if (!gl_drawing_stencil && (grTex->mipmap.flags & TF_TRANSPARENT))
		HWR_AddTransparentWall(wallVerts, &Surf, gl_toptexture, PF_Environment, false, lightnum, gl_frontsector->extra_colormap);
	else
		HWR_ProjectWall(wallVerts, &Surf, PF_Masked, lightnum, gl_frontsector->extra_colormap);
}

static void HWR_ProcessTwoSidedSegBottom(FOutVector *wallVerts, gl_seg_bounds *b,
		float cliplow, float cliphigh, FUINT lightnum, INT32 gl_bottomtexture)
{
	FSurfaceInfo Surf;
	GLMapTexture_t *grTex = HWR_GetTexture(gl_bottomtexture);

	fixed_t texturevpegbottom = 0;

	Surf.PolyColor.s.alpha = 255;

	// PEGGING
	if (!(gl_linedef->flags & ML_DONTPEGBOTTOM))
		texturevpegbottom = 0;
	else if (gl_linedef->flags & ML_EFFECT1)
		texturevpegbottom = b->worldbottom - b->worldlow;
	else
		texturevpegbottom = gl_frontsector->floorheight - gl_backsector->floorheight;

	texturevpegbottom += gl_sidedef->rowoffset;

	// This is so that it doesn't overflow and screw up the wall, it doesn't need to go higher than the texture's height anyway
	texturevpegbottom %= (textures[gl_bottomtexture]->height)<<FRACBITS;

	wallVerts[3].t = wallVerts[2].t = texturevpegbottom * grTex->scaleY;
	wallVerts[0].t = wallVerts[1].t = (texturevpegbottom + gl_backsector->floorheight - gl_frontsector->floorheight) * grTex->scaleY;
	wallVerts[0].s = wallVerts[3].s = cliplow * grTex->scaleX;
	wallVerts[2].s = wallVerts[1].s = cliphigh * grTex->scaleX;

	// Adjust t value for sloped walls
	if (!(gl_linedef->flags & ML_EFFECT1))
	{
		// Unskewed
		wallVerts[0].t -= (b->worldbottom - gl_frontsector->floorheight) * grTex->scaleY;
		wallVerts[1].t -= (b->worldbottomslope - gl_frontsector->floorheight) * grTex->scaleY;
		wallVerts[3].t -= (b->worldlow - gl_backsector->floorheight) * grTex->scaleY;
		wallVerts[2].t -= (b->worldlowslope - gl_backsector->floorheight) * grTex->scaleY;
	}
	else if (gl_linedef->flags & ML_DONTPEGBOTTOM)
	{
		// Skewed by bottom
		wallVerts[0].t = wallVerts[1].t = (texturevpegbottom + b->worldlow - b->worldbottom) * grTex->scaleY;
		//wallVerts[3].t = wallVerts[0].t - (worldlow - worldbottom) * grTex->scaleY; // no need, [3] is already this
		wallVerts[2].t = wallVerts[1].t - (b->worldlowslope - b->worldbottomslope) * grTex->scaleY;
	}
	else
	{
		// Skewed by top
		wallVerts[0].t = (texturevpegbottom + b->worldlow - b->worldbottom) * grTex->scaleY;
		wallVerts[1].t = (texturevpegbottom + b->worldlowslope - b->worldbottomslope) * grTex->scaleY;
	}


	// set top/bottom coords
	wallVerts[3].y = FIXED_TO_FLOAT(b->worldlow);
	wallVerts[0].y = FIXED_TO_FLOAT(b->worldbottom);
	wallVerts[2].y = FIXED_TO_FLOAT(b->worldlowslope);
	wallVerts[1].y = FIXED_TO_FLOAT(b->worldbottomslope);

	if (!gl_drawing_stencil && gl_frontsector->numlights)
		HWR_SplitWall(gl_frontsector, wallVerts, gl_bottomtexture, &Surf, FF_CUTLEVEL, NULL, 0);
	else if (!gl_drawing_stencil && (grTex->mipmap.flags & TF_TRANSPARENT))
		HWR_AddTransparentWall(wallVerts, &Surf, gl_bottomtexture, PF_Environment, false, lightnum, gl_frontsector->extra_colormap);
	else
		HWR_ProjectWall(wallVerts, &Surf, PF_Masked, lightnum, gl_frontsector->extra_colormap);
}

// gl_midtexture can be inactive for this function
// when rendering twosided portal midtextures
static void HWR_ProcessTwoSidedSegMiddle(FOutVector *wallVerts, gl_seg_bounds *b,
		float cliplow, float cliphigh, FUINT lightnum, INT32 gl_midtexture)
{
	FSurfaceInfo Surf;
	GLMapTexture_t *grTex = NULL;

	FBITFIELD blendmode;
	sector_t *front, *back;
	fixed_t h, l; // 2s middle textures
	fixed_t  popentop, popenbottom, polytop, polybottom, lowcut, highcut;
	fixed_t     texturevpeg = 0;
	INT32 repeats;

	Surf.PolyColor.s.alpha = 255;

	if (gl_linedef->frontsector->heightsec != -1)
		front = &sectors[gl_linedef->frontsector->heightsec];
	else
		front = gl_linedef->frontsector;

	if (gl_linedef->backsector->heightsec != -1)
		back = &sectors[gl_linedef->backsector->heightsec];
	else
		back = gl_linedef->backsector;

	if (gl_midtexture)
	{
		if (gl_sidedef->repeatcnt)
			repeats = 1 + gl_sidedef->repeatcnt;
		else if (gl_linedef->flags & ML_EFFECT5)
		{
			fixed_t high, low;

			if (front->ceilingheight > back->ceilingheight)
				high = back->ceilingheight;
			else
				high = front->ceilingheight;

			if (front->floorheight > back->floorheight)
				low = front->floorheight;
			else
				low = back->floorheight;

			repeats = (high - low)/textureheight[gl_sidedef->midtexture];
			if ((high-low)%textureheight[gl_sidedef->midtexture])
				repeats++; // tile an extra time to fill the gap -- Monster Iestyn
		}
		else
			repeats = 1;
	}

	// SoM: a little note: This code re-arranging will
	// fix the bug in Nimrod map02. popentop and popenbottom
	// record the limits the texture can be displayed in.
	// polytop and polybottom, are the ideal (i.e. unclipped)
	// heights of the polygon, and h & l, are the final (clipped)
	// poly coords.

	// NOTE: With polyobjects, whenever you need to check the properties of the polyobject sector it belongs to,
	// you must use the linedef's backsector to be correct
	// From CB
	if (gl_curline->polyseg)
	{
		popentop = back->ceilingheight;
		popenbottom = back->floorheight;
	}
	else
	{
		popentop = min(b->worldtop, b->worldhigh);
		popenbottom = max(b->worldbottom, b->worldlow);
	}

	if (gl_midtexture)
	{
		if (gl_linedef->flags & ML_EFFECT2)
		{
			if (!!(gl_linedef->flags & ML_DONTPEGBOTTOM) ^ !!(gl_linedef->flags & ML_EFFECT3))
			{
				polybottom = max(front->floorheight, back->floorheight) + gl_sidedef->rowoffset;
				polytop = polybottom + textureheight[gl_midtexture]*repeats;
			}
			else
			{
				polytop = min(front->ceilingheight, back->ceilingheight) + gl_sidedef->rowoffset;
				polybottom = polytop - textureheight[gl_midtexture]*repeats;
			}
		}
		else if (!!(gl_linedef->flags & ML_DONTPEGBOTTOM) ^ !!(gl_linedef->flags & ML_EFFECT3))
		{
			polybottom = popenbottom + gl_sidedef->rowoffset;
			polytop = polybottom + textureheight[gl_midtexture]*repeats;
		}
		else
		{
			polytop = popentop + gl_sidedef->rowoffset;
			polybottom = polytop - textureheight[gl_midtexture]*repeats;
		}
	}
	else
	{
		// portal stencil twosided midtexture, fills entire space
		polytop = popentop;
		polybottom = popenbottom;
	}
	// CB
	// NOTE: With polyobjects, whenever you need to check the properties of the polyobject sector it belongs to,
	// you must use the linedef's backsector to be correct
	if (gl_curline->polyseg)
	{
		lowcut = polybottom;
		highcut = polytop;
	}
	else
	{
		// The cut-off values of a linedef can always be constant, since every line has an absoulute front and or back sector
		lowcut = popenbottom;
		highcut = popentop;
	}

	h = min(highcut, polytop);
	l = max(polybottom, lowcut);

	// gl_midtexture can be inactive when rendering twosided portal midtextures
	if (gl_midtexture)
	{
		// PEGGING
		if (!!(gl_linedef->flags & ML_DONTPEGBOTTOM) ^ !!(gl_linedef->flags & ML_EFFECT3))
			texturevpeg = textureheight[gl_sidedef->midtexture]*repeats - h + polybottom;
		else
			texturevpeg = polytop - h;

		grTex = HWR_GetTexture(gl_midtexture);

		wallVerts[3].t = wallVerts[2].t = texturevpeg * grTex->scaleY;
		wallVerts[0].t = wallVerts[1].t = (h - l + texturevpeg) * grTex->scaleY;
		wallVerts[0].s = wallVerts[3].s = cliplow * grTex->scaleX;
		wallVerts[2].s = wallVerts[1].s = cliphigh * grTex->scaleX;
	}

	// set top/bottom coords
	// Take the texture peg into account, rather than changing the offsets past
	// where the polygon might not be.
	wallVerts[2].y = wallVerts[3].y = FIXED_TO_FLOAT(h);
	wallVerts[0].y = wallVerts[1].y = FIXED_TO_FLOAT(l);

	// Correct to account for slopes
	{
		fixed_t midtextureslant;

		if (gl_linedef->flags & ML_EFFECT2)
			midtextureslant = 0;
		else if (!!(gl_linedef->flags & ML_DONTPEGBOTTOM) ^ !!(gl_linedef->flags & ML_EFFECT3))
			midtextureslant = b->worldlow < b->worldbottom
						? b->worldbottomslope - b->worldbottom
						: b->worldlowslope - b->worldlow;
		else
			midtextureslant = b->worldtop < b->worldhigh
						? b->worldtopslope - b->worldtop
						: b->worldhighslope - b->worldhigh;

		polytop += midtextureslant;
		polybottom += midtextureslant;

		highcut += b->worldtop < b->worldhigh
					? b->worldtopslope - b->worldtop
					: b->worldhighslope - b->worldhigh;
		lowcut += b->worldlow < b->worldbottom
				? b->worldbottomslope - b->worldbottom
				: b->worldlowslope - b->worldlow;

		// Texture stuff
		h = min(highcut, polytop);
		l = max(polybottom, lowcut);

		if (grTex)
		{
			// PEGGING
			if (!!(gl_linedef->flags & ML_DONTPEGBOTTOM) ^ !!(gl_linedef->flags & ML_EFFECT3))
				texturevpeg = textureheight[gl_sidedef->midtexture]*repeats - h + polybottom;
			else
				texturevpeg = polytop - h;
			wallVerts[2].t = texturevpeg * grTex->scaleY;
			wallVerts[1].t = (h - l + texturevpeg) * grTex->scaleY;
		}

		wallVerts[2].y = FIXED_TO_FLOAT(h);
		wallVerts[1].y = FIXED_TO_FLOAT(l);
	}

	// set alpha for transparent walls
	// ooops ! this do not work at all because render order we should render it in backtofront order
	switch (gl_linedef->special)
	{
		//  Translucent
		case 102:
		case 121:
		case 123:
		case 124:
		case 125:
		case 141:
		case 142:
		case 144:
		case 145:
		case 174:
		case 175:
		case 192:
		case 195:
		case 221:
		case 253:
		case 256:
			if (gl_linedef->blendmode && gl_linedef->blendmode != AST_FOG)
				blendmode = HWR_SurfaceBlend(gl_linedef->blendmode, R_GetLinedefTransTable(gl_linedef->alpha), &Surf);
			else
				blendmode = PF_Translucent;
			break;
		default:
			if (gl_linedef->blendmode && gl_linedef->blendmode != AST_FOG)
			{
				if (gl_linedef->alpha >= 0 && gl_linedef->alpha < FRACUNIT)
					blendmode = HWR_SurfaceBlend(gl_linedef->blendmode, R_GetLinedefTransTable(gl_linedef->alpha), &Surf);
				else
					blendmode = HWR_GetBlendModeFlag(gl_linedef->blendmode);
			}
			else if (gl_linedef->alpha >= 0 && gl_linedef->alpha < FRACUNIT)
				blendmode = HWR_TranstableToAlpha(R_GetLinedefTransTable(gl_linedef->alpha), &Surf);
			else
				blendmode = PF_Masked;
			break;
	}

	if (gl_curline->polyseg && gl_curline->polyseg->translucency > 0)
	{
		if (gl_curline->polyseg->translucency >= NUMTRANSMAPS) // wall not drawn
		{
			Surf.PolyColor.s.alpha = 0x00; // This shouldn't draw anything regardless of blendmode
			blendmode = PF_Masked;
		}
		else
			blendmode = HWR_TranstableToAlpha(gl_curline->polyseg->translucency, &Surf);
	}

	// Render midtextures on two-sided lines with a z-buffer offset.
	// This will cause the midtexture appear on top, if a FOF overlaps with it.
	blendmode |= PF_Decal;

	if (!grTex)
		blendmode |= PF_NoTexture;

	if (!gl_drawing_stencil && gl_frontsector->numlights)
	{
		if (!(blendmode & PF_Masked))
			HWR_SplitWall(gl_frontsector, wallVerts, gl_midtexture, &Surf, FF_TRANSLUCENT, NULL, blendmode);
		else
			HWR_SplitWall(gl_frontsector, wallVerts, gl_midtexture, &Surf, FF_CUTLEVEL, NULL, blendmode);
	}
	else if (!gl_drawing_stencil && !(blendmode & PF_Masked))
		HWR_AddTransparentWall(wallVerts, &Surf, gl_midtexture, blendmode, false, lightnum, gl_frontsector->extra_colormap);
	else
		HWR_ProjectWall(wallVerts, &Surf, blendmode, lightnum, gl_frontsector->extra_colormap);
}

static void HWR_ProcessTwoSidedSeg(FOutVector *wallVerts, gl_seg_bounds *b,
		float cliplow, float cliphigh, FUINT lightnum)
{
	FSurfaceInfo Surf;

	INT32 gl_toptexture = 0, gl_bottomtexture = 0;
	INT32 gl_midtexture;
	// two sided line
	boolean bothceilingssky = false; // turned on if both back and front ceilings are sky
	boolean bothfloorssky = false; // likewise, but for floors

	Surf.PolyColor.s.alpha = 255;

	// hack to allow height changes in outdoor areas
	// This is what gets rid of the upper textures if there should be sky
	if (gl_frontsector->ceilingpic == skyflatnum
		&& gl_backsector->ceilingpic == skyflatnum)
	{
		bothceilingssky = true;
	}

	// likewise, but for floors and upper textures
	if (gl_frontsector->floorpic == skyflatnum
		&& gl_backsector->floorpic == skyflatnum)
	{
		bothfloorssky = true;
	}

	if (!bothceilingssky)
		gl_toptexture = R_GetTextureNum(gl_sidedef->toptexture);
	if (!bothfloorssky)
		gl_bottomtexture = R_GetTextureNum(gl_sidedef->bottomtexture);

	// check TOP TEXTURE
	if ((b->worldhighslope < b->worldtopslope || b->worldhigh < b->worldtop)
			&& gl_toptexture)
	{
		HWR_ProcessTwoSidedSegTop(wallVerts, b, cliplow, cliphigh, lightnum, gl_toptexture);
	}

	// check BOTTOM TEXTURE
	if ((b->worldlowslope > b->worldbottomslope || b->worldlow > b->worldbottom)
			&& gl_bottomtexture)
	{
		HWR_ProcessTwoSidedSegBottom(wallVerts, b, cliplow, cliphigh, lightnum, gl_bottomtexture);
	}

	gl_midtexture = R_GetTextureNum(gl_sidedef->midtexture);
	// twosided midtexture goes to stencil even if there's no midtexture
	// (software also renders twosided midtexture portals like this)
	if (gl_midtexture || gl_drawing_stencil)
	{
		HWR_ProcessTwoSidedSegMiddle(wallVerts, b, cliplow, cliphigh, lightnum, gl_midtexture);
	}

	// Sky culling
	// No longer so much a mess as before!
	if (!gl_curline->polyseg) // Don't do it for polyobjects
	{
		if (gl_frontsector->ceilingpic == skyflatnum)
		{
			if (gl_backsector->ceilingpic != skyflatnum) // don't cull if back sector is also sky
			{
				wallVerts[2].y = wallVerts[3].y = FIXED_TO_FLOAT(INT32_MAX); // draw to top of map space
				wallVerts[0].y = FIXED_TO_FLOAT(b->worldtop);
				wallVerts[1].y = FIXED_TO_FLOAT(b->worldtopslope);
				HWR_DrawSkyWall(wallVerts, &Surf);
			}
		}

		if (gl_frontsector->floorpic == skyflatnum)
		{
			if (gl_backsector->floorpic != skyflatnum) // don't cull if back sector is also sky
			{
				wallVerts[3].y = FIXED_TO_FLOAT(b->worldbottom);
				wallVerts[2].y = FIXED_TO_FLOAT(b->worldbottomslope);
				wallVerts[0].y = wallVerts[1].y = FIXED_TO_FLOAT(INT32_MIN); // draw to bottom of map space
				HWR_DrawSkyWall(wallVerts, &Surf);
			}
		}
	}
}

static void HWR_ProcessSingleSidedSeg(FOutVector *wallVerts, gl_seg_bounds *b,
		float cliplow, float cliphigh, FUINT lightnum)
{
	FSurfaceInfo Surf;
	GLMapTexture_t *grTex = NULL;

	Surf.PolyColor.s.alpha = 255;

	// Single sided line... Deal only with the middletexture (if one exists)
	INT32 gl_midtexture = R_GetTextureNum(gl_sidedef->midtexture);
	if (gl_midtexture && gl_linedef->special != 41) // (Ignore horizon line for OGL)
	{
		fixed_t     texturevpeg;
		// PEGGING
		if ((gl_linedef->flags & (ML_DONTPEGBOTTOM|ML_EFFECT2)) == (ML_DONTPEGBOTTOM|ML_EFFECT2))
			texturevpeg = gl_frontsector->floorheight + textureheight[gl_sidedef->midtexture] - gl_frontsector->ceilingheight + gl_sidedef->rowoffset;
		else if (gl_linedef->flags & ML_DONTPEGBOTTOM)
			texturevpeg = b->worldbottom + textureheight[gl_sidedef->midtexture] - b->worldtop + gl_sidedef->rowoffset;
		else
			// top of texture at top
			texturevpeg = gl_sidedef->rowoffset;

		grTex = HWR_GetTexture(gl_midtexture);

		wallVerts[3].t = wallVerts[2].t = texturevpeg * grTex->scaleY;
		wallVerts[0].t = wallVerts[1].t = (texturevpeg + gl_frontsector->ceilingheight - gl_frontsector->floorheight) * grTex->scaleY;
		wallVerts[0].s = wallVerts[3].s = cliplow * grTex->scaleX;
		wallVerts[2].s = wallVerts[1].s = cliphigh * grTex->scaleX;

		// Texture correction for slopes
		if (gl_linedef->flags & ML_EFFECT2) {
			wallVerts[3].t += (gl_frontsector->ceilingheight - b->worldtop) * grTex->scaleY;
			wallVerts[2].t += (gl_frontsector->ceilingheight - b->worldtopslope) * grTex->scaleY;
			wallVerts[0].t += (gl_frontsector->floorheight - b->worldbottom) * grTex->scaleY;
			wallVerts[1].t += (gl_frontsector->floorheight - b->worldbottomslope) * grTex->scaleY;
		} else if (gl_linedef->flags & ML_DONTPEGBOTTOM) {
			wallVerts[3].t = wallVerts[0].t + (b->worldbottom - b->worldtop) * grTex->scaleY;
			wallVerts[2].t = wallVerts[1].t + (b->worldbottomslope - b->worldtopslope) * grTex->scaleY;
		} else {
			wallVerts[0].t = wallVerts[3].t - (b->worldbottom - b->worldtop) * grTex->scaleY;
			wallVerts[1].t = wallVerts[2].t - (b->worldbottomslope - b->worldtopslope) * grTex->scaleY;
		}

		//Set textures properly on single sided walls that are sloped
		wallVerts[3].y = FIXED_TO_FLOAT(b->worldtop);
		wallVerts[0].y = FIXED_TO_FLOAT(b->worldbottom);
		wallVerts[2].y = FIXED_TO_FLOAT(b->worldtopslope);
		wallVerts[1].y = FIXED_TO_FLOAT(b->worldbottomslope);

		// I don't think that solid walls can use translucent linedef types...
		if (!gl_drawing_stencil && gl_frontsector->numlights)
			HWR_SplitWall(gl_frontsector, wallVerts, gl_midtexture, &Surf, FF_CUTLEVEL, NULL, 0);
		else
		{
			if (!gl_drawing_stencil && (grTex->mipmap.flags & TF_TRANSPARENT))
				HWR_AddTransparentWall(wallVerts, &Surf, gl_midtexture, PF_Environment, false, lightnum, gl_frontsector->extra_colormap);
			else
				HWR_ProjectWall(wallVerts, &Surf, PF_Masked, lightnum, gl_frontsector->extra_colormap);
		}
	}

	if (!gl_curline->polyseg)
	{
		if (gl_frontsector->ceilingpic == skyflatnum) // It's a single-sided line with sky for its sector
		{
			wallVerts[2].y = wallVerts[3].y = FIXED_TO_FLOAT(INT32_MAX); // draw to top of map space
			wallVerts[0].y = FIXED_TO_FLOAT(b->worldtop);
			wallVerts[1].y = FIXED_TO_FLOAT(b->worldtopslope);
			HWR_DrawSkyWall(wallVerts, &Surf);
		}
		if (gl_frontsector->floorpic == skyflatnum)
		{
			wallVerts[3].y = FIXED_TO_FLOAT(b->worldbottom);
			wallVerts[2].y = FIXED_TO_FLOAT(b->worldbottomslope);
			wallVerts[0].y = wallVerts[1].y = FIXED_TO_FLOAT(INT32_MIN); // draw to bottom of map space
			HWR_DrawSkyWall(wallVerts, &Surf);
		}
	}
}

// struct for HWR_ProcessSegFOFs
typedef struct
{
	fixed_t highcut, lowcut;
	fixed_t lowcutslope, highcutslope;
} gl_fof_cuts;

// is_front_sector is true when this_sector is the front sector
static void HWR_ProcessSegFOFsSector(FOutVector *wallVerts, gl_fof_cuts *c,
		float cliplow, float cliphigh, FUINT lightnum,
		fixed_t v1x, fixed_t v1y, fixed_t v2x, fixed_t v2y,
		v2d_t vs, v2d_t ve,
		sector_t *this_sector, sector_t *other_sector, boolean is_front_sector)
{
	FSurfaceInfo Surf;
	GLMapTexture_t *grTex = NULL;

	extracolormap_t *colormap = gl_frontsector->extra_colormap;

	ffloor_t * rover;
	fixed_t h, l; // 3D sides
	fixed_t hS, lS;

	// Used for height comparisons and etc across FOFs and slopes
	fixed_t high1, highslope1, low1, lowslope1;

	INT32 texnum;
	line_t * newline = NULL; // Multi-Property FOF

	Surf.PolyColor.s.alpha = 255;

	for (rover = this_sector->ffloors; rover; rover = rover->next)
	{
		boolean bothsides = false;
		// Skip if it exists on both sectors.
		ffloor_t * r2;
		for (r2 = other_sector->ffloors; r2; r2 = r2->next)
			if (rover->master == r2->master)
			{
				bothsides = true;
				break;
			}

		if (bothsides) continue;

		if (!(rover->flags & FF_EXISTS) || !(rover->flags & FF_RENDERSIDES))
			continue;
		if (is_front_sector)
		{
			if (!(rover->flags & FF_ALLSIDES || rover->flags & FF_INVERTSIDES))
				continue;
		}
		else
		{
			if (!(rover->flags & FF_ALLSIDES) && rover->flags & FF_INVERTSIDES)
				continue;
		}

		SLOPEPARAMS(*rover->t_slope, high1, highslope1, *rover->topheight)
		SLOPEPARAMS(*rover->b_slope, low1,  lowslope1,  *rover->bottomheight)

		if ((high1 < c->lowcut && highslope1 < c->lowcutslope) || (low1 > c->highcut && lowslope1 > c->highcutslope))
			continue;

		texnum = R_GetTextureNum(sides[rover->master->sidenum[0]].midtexture);

		if (rover->master->flags & ML_TFERLINE)
		{
			size_t linenum = gl_curline->linedef-gl_backsector->lines[0];
			newline = rover->master->frontsector->lines[0] + linenum;
			texnum = R_GetTextureNum(sides[newline->sidenum[0]].midtexture);
		}

		h  = P_GetFFloorTopZAt   (rover, v1x, v1y);
		hS = P_GetFFloorTopZAt   (rover, v2x, v2y);
		l  = P_GetFFloorBottomZAt(rover, v1x, v1y);
		lS = P_GetFFloorBottomZAt(rover, v2x, v2y);
		// Adjust the heights so the FOF does not overlap with top and bottom textures.
		if (h >= c->highcut && hS >= c->highcutslope)
		{
			h = c->highcut;
			hS = c->highcutslope;
		}
		if (l <= c->lowcut && lS <= c->lowcutslope)
		{
			l = c->lowcut;
			lS = c->lowcutslope;
		}
		//Hurdler: HW code starts here
		//FIXME: check if peging is correct
		// set top/bottom coords

		wallVerts[3].y = FIXED_TO_FLOAT(h);
		wallVerts[2].y = FIXED_TO_FLOAT(hS);
		wallVerts[0].y = FIXED_TO_FLOAT(l);
		wallVerts[1].y = FIXED_TO_FLOAT(lS);
		if (rover->flags & FF_FOG)
		{
			wallVerts[3].t = wallVerts[2].t = 0;
			wallVerts[0].t = wallVerts[1].t = 0;
			wallVerts[0].s = wallVerts[3].s = 0;
			wallVerts[2].s = wallVerts[1].s = 0;
		}
		else
		{
			fixed_t texturevpeg;
			boolean attachtobottom = false;
			boolean slopeskew = false; // skew FOF walls with slopes?

			// Wow, how was this missing from OpenGL for so long?
			// ...Oh well, anyway, Lower Unpegged now changes pegging of FOFs like in software
			// -- Monster Iestyn 26/06/18
			if (newline)
			{
				texturevpeg = sides[newline->sidenum[0]].rowoffset;
				attachtobottom = !!(newline->flags & ML_DONTPEGBOTTOM);
				slopeskew = !!(newline->flags & ML_DONTPEGTOP);
			}
			else
			{
				texturevpeg = sides[rover->master->sidenum[0]].rowoffset;
				attachtobottom = !!(gl_linedef->flags & ML_DONTPEGBOTTOM);
				slopeskew = !!(rover->master->flags & ML_DONTPEGTOP);
			}

			grTex = HWR_GetTexture(texnum);

			if (!slopeskew) // no skewing
			{
				if (attachtobottom)
					texturevpeg -= *rover->topheight - *rover->bottomheight;
				wallVerts[3].t = (*rover->topheight - h + texturevpeg) * grTex->scaleY;
				wallVerts[2].t = (*rover->topheight - hS + texturevpeg) * grTex->scaleY;
				wallVerts[0].t = (*rover->topheight - l + texturevpeg) * grTex->scaleY;
				wallVerts[1].t = (*rover->topheight - lS + texturevpeg) * grTex->scaleY;
			}
			else
			{
				if (!attachtobottom) // skew by top
				{
					wallVerts[3].t = wallVerts[2].t = texturevpeg * grTex->scaleY;
					wallVerts[0].t = (h - l + texturevpeg) * grTex->scaleY;
					wallVerts[1].t = (hS - lS + texturevpeg) * grTex->scaleY;
				}
				else // skew by bottom
				{
					wallVerts[0].t = wallVerts[1].t = texturevpeg * grTex->scaleY;
					wallVerts[3].t = wallVerts[0].t - (h - l) * grTex->scaleY;
					wallVerts[2].t = wallVerts[1].t - (hS - lS) * grTex->scaleY;
				}
			}

			wallVerts[0].s = wallVerts[3].s = cliplow * grTex->scaleX;
			wallVerts[2].s = wallVerts[1].s = cliphigh * grTex->scaleX;
		}

		if (rover->flags & FF_FOG)
		{
			FBITFIELD blendmode;

			blendmode = PF_Fog|PF_NoTexture;

			lightnum = rover->master->frontsector->lightlevel;
			colormap = rover->master->frontsector->extra_colormap;
			lightnum = colormap ? lightnum : HWR_CalcWallLight(lightnum, vs.x, vs.y, ve.x, ve.y);

			Surf.PolyColor.s.alpha = HWR_FogBlockAlpha(rover->master->frontsector->lightlevel, rover->master->frontsector->extra_colormap);

			if (other_sector->numlights)
				HWR_SplitWall(other_sector, wallVerts, 0, &Surf, rover->flags, rover, blendmode);
			else
				HWR_AddTransparentWall(wallVerts, &Surf, 0, blendmode, true, lightnum, colormap);
		}
		else
		{
			FBITFIELD blendmode = PF_Masked;

			if ((rover->flags & FF_TRANSLUCENT && rover->alpha < 256) || rover->blend)
			{
				blendmode = rover->blend ? HWR_GetBlendModeFlag(rover->blend) : PF_Translucent;
				Surf.PolyColor.s.alpha = (UINT8)rover->alpha-1 > 255 ? 255 : rover->alpha-1;
			}

			if (other_sector->numlights)
				HWR_SplitWall(other_sector, wallVerts, texnum, &Surf, rover->flags, rover, blendmode);
			else
			{
				if (blendmode != PF_Masked)
					HWR_AddTransparentWall(wallVerts, &Surf, texnum, blendmode, false, lightnum, colormap);
				else
					HWR_ProjectWall(wallVerts, &Surf, PF_Masked, lightnum, colormap);
			}
		}
	}
}

static void HWR_ProcessSegFOFs(FOutVector *wallVerts, gl_seg_bounds *b,
		float cliplow, float cliphigh, FUINT lightnum,
		fixed_t v1x, fixed_t v1y, fixed_t v2x, fixed_t v2y,
		v2d_t vs, v2d_t ve)
{
	gl_fof_cuts c;

	c.lowcut = max(b->worldbottom, b->worldlow);
	c.highcut = min(b->worldtop, b->worldhigh);
	c.lowcutslope = max(b->worldbottomslope, b->worldlowslope);
	c.highcutslope = min(b->worldtopslope, b->worldhighslope);

	if (gl_backsector->ffloors)
	{
		HWR_ProcessSegFOFsSector(wallVerts, &c, cliplow, cliphigh, lightnum,
				v1x, v1y, v2x, v2y, vs, ve, gl_backsector, gl_frontsector, false);
	}

	if (gl_frontsector->ffloors) // Putting this seperate should allow 2 FOF sectors to be connected without too many errors? I think?
	{
		HWR_ProcessSegFOFsSector(wallVerts, &c, cliplow, cliphigh, lightnum,
				v1x, v1y, v2x, v2y, vs, ve, gl_frontsector, gl_backsector, true);
	}
}

void HWR_ProcessSeg(void) // Sort of like GLWall::Process in GZDoom
{
	FOutVector wallVerts[4] = {0};
	v2d_t vs, ve; // start, end vertices of 2d line (view from above)

	gl_seg_bounds b = {0};
	fixed_t v1x, v1y, v2x, v2y;

	float cliplow = 0.0f, cliphigh = 0.0f;

	FUINT lightnum = 0;

	gl_sidedef = gl_curline->sidedef;
	gl_linedef = gl_curline->linedef;

	vs.x = ((polyvertex_t *)gl_curline->pv1)->x;
	vs.y = ((polyvertex_t *)gl_curline->pv1)->y;
	ve.x = ((polyvertex_t *)gl_curline->pv2)->x;
	ve.y = ((polyvertex_t *)gl_curline->pv2)->y;

	v1x = FLOAT_TO_FIXED(vs.x);
	v1y = FLOAT_TO_FIXED(vs.y);
	v2x = FLOAT_TO_FIXED(ve.x);
	v2y = FLOAT_TO_FIXED(ve.y);

	// get boundaries for front sector
	SLOPEPARAMS(gl_frontsector->c_slope, b.worldtop,    b.worldtopslope,    gl_frontsector->ceilingheight)
	SLOPEPARAMS(gl_frontsector->f_slope, b.worldbottom, b.worldbottomslope, gl_frontsector->floorheight)

	// remember vertices ordering
	//  3--2
	//  | /|
	//  |/ |
	//  0--1
	// make a wall polygon (with 2 triangles), using the floor/ceiling heights,
	// and the 2d map coords of start/end vertices
	wallVerts[0].x = wallVerts[3].x = vs.x;
	wallVerts[0].z = wallVerts[3].z = vs.y;
	wallVerts[2].x = wallVerts[1].x = ve.x;
	wallVerts[2].z = wallVerts[1].z = ve.y;

	// x offset the texture
	{
		fixed_t texturehpeg = gl_sidedef->textureoffset + gl_curline->offset;
		cliplow = (float)texturehpeg;
		cliphigh = (float)(texturehpeg + (gl_curline->flength*FRACUNIT));
	}

	lightnum = gl_frontsector->lightlevel;
	lightnum = gl_frontsector->extra_colormap ?
			lightnum : HWR_CalcWallLight(lightnum, vs.x, vs.y, ve.x, ve.y);

	if (gl_backsector)
	{
		// get boundaries for top and bottom textures
		SLOPEPARAMS(gl_backsector->c_slope, b.worldhigh, b.worldhighslope, gl_backsector->ceilingheight)
		SLOPEPARAMS(gl_backsector->f_slope, b.worldlow,  b.worldlowslope,  gl_backsector->floorheight)

		HWR_ProcessTwoSidedSeg(wallVerts, &b, cliplow, cliphigh, lightnum);
	}
	else
	{
		HWR_ProcessSingleSidedSeg(wallVerts, &b, cliplow, cliphigh, lightnum);
	}

	// no fofs to stencil
	if (!gl_drawing_stencil && gl_frontsector && gl_backsector &&
			!Tag_Compare(&gl_frontsector->tags, &gl_backsector->tags) &&
			(gl_backsector->ffloors || gl_frontsector->ffloors))
	{
		HWR_ProcessSegFOFs(wallVerts, &b, cliplow, cliphigh, lightnum,
				v1x, v1y, v2x, v2y, vs, ve);
	}
}

#undef SLOPEPARAMS

void HWR_RenderWall(FOutVector *wallVerts, FSurfaceInfo *pSurf, FBITFIELD blend, boolean fogwall, INT32 lightlevel, extracolormap_t *wallcolormap)
{
	FBITFIELD blendmode = blend;
	UINT8 alpha = pSurf->PolyColor.s.alpha; // retain the alpha

	INT32 shader = -1;

	// Lighting is done here instead so that fog isn't drawn incorrectly on transparent walls after sorting
	HWR_Lighting(pSurf, lightlevel, wallcolormap);

	pSurf->PolyColor.s.alpha = alpha; // put the alpha back after lighting

	if (blend & PF_Environment)
		blendmode |= PF_Occlude;	// PF_Occlude must be used for solid objects

	if (HWR_UseShader())
	{
		if (fogwall)
			shader = SHADER_FOG;
		else
			shader = SHADER_WALL;

		blendmode |= PF_ColorMapped;
	}

	if (fogwall)
		blendmode |= PF_Fog;

	blendmode |= PF_Modulated;	// No PF_Occlude means overlapping (incorrect) transparency
	HWR_ProcessPolygon(pSurf, wallVerts, 4, blendmode, shader, false);
}

#endif
