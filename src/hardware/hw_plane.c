// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 1999-2022 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file hw_plane.c
/// \brief Plane rendering

#ifdef HWRENDER
#include "hw_glob.h"
#include "hw_batching.h"
#include "../p_slopes.h"
#include "../r_local.h"
#include "../z_zone.h"

static FUINT HWR_CalcSlopeLight(FUINT lightnum, angle_t dir, fixed_t delta)
{
	INT16 finallight = lightnum;

	if (cv_glfakecontrast.value != 0 && cv_glslopecontrast.value != 0)
	{
		const UINT8 contrast = 8;
		fixed_t extralight = 0;

		if (cv_glfakecontrast.value == 2) // Smooth setting
		{
			fixed_t dirmul = abs(FixedDiv(AngleFixed(dir) - (180<<FRACBITS), 180<<FRACBITS));

			extralight = -(contrast<<FRACBITS) + (dirmul * (contrast * 2));

			extralight = FixedMul(extralight, delta*4) >> FRACBITS;
		}
		else
		{
			dir = ((dir + ANGLE_45) / ANGLE_90) * ANGLE_90;

			if (dir == ANGLE_180)
				extralight = -contrast;
			else if (dir == 0)
				extralight = contrast;

			if (delta >= FRACUNIT/2)
				extralight *= 2;
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
//                                   FLOOR/CEILING GENERATION FROM SUBSECTORS
// ==========================================================================

// -----------------+
// HWR_RenderPlane  : Render a floor or ceiling convex polygon
// -----------------+
void HWR_RenderPlane(subsector_t *subsector, extrasubsector_t *xsub, boolean isceiling, fixed_t fixedheight, FBITFIELD PolyFlags, INT32 lightlevel, levelflat_t *levelflat, sector_t *FOFsector, UINT8 alpha, extracolormap_t *planecolormap)
{
	polyvertex_t *  pv;
	float           height; //constant y for all points on the convex flat polygon
	FOutVector      *v3d;
	INT32             nrPlaneVerts;   //verts original define of convex flat polygon
	INT32             i;
	float           flatxref,flatyref;
	float fflatwidth = 64.0f, fflatheight = 64.0f;
	INT32 flatflag = 63;
	boolean texflat = false;
	float scrollx = 0.0f, scrolly = 0.0f, anglef = 0.0f;
	angle_t angle = 0;
	FSurfaceInfo    Surf;
	float tempxsow, tempytow;
	pslope_t *slope = NULL;

	static FOutVector *planeVerts = NULL;
	static UINT16 numAllocedPlaneVerts = 0;

	INT32 shader = -1;

	// no convex poly were generated for this subsector
	if (!xsub->planepoly)
		return;

	// Get the slope pointer to simplify future code
	if (FOFsector)
	{
		if (FOFsector->f_slope && !isceiling)
			slope = FOFsector->f_slope;
		else if (FOFsector->c_slope && isceiling)
			slope = FOFsector->c_slope;
	}
	else
	{
		if (gl_frontsector->f_slope && !isceiling)
			slope = gl_frontsector->f_slope;
		else if (gl_frontsector->c_slope && isceiling)
			slope = gl_frontsector->c_slope;
	}

	// Set fixedheight to the slope's height from our viewpoint, if we have a slope
	if (slope)
		fixedheight = P_GetSlopeZAt(slope, viewx, viewy);

	height = FIXED_TO_FLOAT(fixedheight);

	pv  = xsub->planepoly->pts;
	nrPlaneVerts = xsub->planepoly->numpts;

	if (nrPlaneVerts < 3)   //not even a triangle ?
		return;

	// Allocate plane-vertex buffer if we need to
	if (!planeVerts || nrPlaneVerts > numAllocedPlaneVerts)
	{
		numAllocedPlaneVerts = (UINT16)nrPlaneVerts;
		Z_Free(planeVerts);
		Z_Malloc(numAllocedPlaneVerts * sizeof (FOutVector), PU_LEVEL, &planeVerts);
	}

	// set texture for polygon
	if (levelflat != NULL)
	{
		if (levelflat->type == LEVELFLAT_FLAT)
		{
			size_t len = W_LumpLength(levelflat->u.flat.lumpnum);
			switch (len)
			{
				case 4194304: // 2048x2048 lump
					fflatwidth = fflatheight = 2048.0f;
					break;
				case 1048576: // 1024x1024 lump
					fflatwidth = fflatheight = 1024.0f;
					break;
				case 262144:// 512x512 lump
					fflatwidth = fflatheight = 512.0f;
					break;
				case 65536: // 256x256 lump
					fflatwidth = fflatheight = 256.0f;
					break;
				case 16384: // 128x128 lump
					fflatwidth = fflatheight = 128.0f;
					break;
				case 1024: // 32x32 lump
					fflatwidth = fflatheight = 32.0f;
					break;
				default: // 64x64 lump
					fflatwidth = fflatheight = 64.0f;
					break;
			}
			flatflag = ((INT32)fflatwidth)-1;
		}
		else
		{
			if (levelflat->type == LEVELFLAT_TEXTURE)
			{
				fflatwidth = textures[levelflat->u.texture.num]->width;
				fflatheight = textures[levelflat->u.texture.num]->height;
			}
			else if (levelflat->type == LEVELFLAT_PATCH || levelflat->type == LEVELFLAT_PNG)
			{
				fflatwidth = levelflat->width;
				fflatheight = levelflat->height;
			}
			texflat = true;
		}
	}
	else // set no texture
		HWR_SetCurrentTexture(NULL);

	// reference point for flat texture coord for each vertex around the polygon
	flatxref = (float)(((fixed_t)pv->x & (~flatflag)) / fflatwidth);
	flatyref = (float)(((fixed_t)pv->y & (~flatflag)) / fflatheight);

	// transform
	if (FOFsector != NULL)
	{
		if (!isceiling) // it's a floor
		{
			scrollx = FIXED_TO_FLOAT(FOFsector->floor_xoffs)/fflatwidth;
			scrolly = FIXED_TO_FLOAT(FOFsector->floor_yoffs)/fflatheight;
			angle = FOFsector->floorpic_angle;
		}
		else // it's a ceiling
		{
			scrollx = FIXED_TO_FLOAT(FOFsector->ceiling_xoffs)/fflatwidth;
			scrolly = FIXED_TO_FLOAT(FOFsector->ceiling_yoffs)/fflatheight;
			angle = FOFsector->ceilingpic_angle;
		}
	}
	else if (gl_frontsector)
	{
		if (!isceiling) // it's a floor
		{
			scrollx = FIXED_TO_FLOAT(gl_frontsector->floor_xoffs)/fflatwidth;
			scrolly = FIXED_TO_FLOAT(gl_frontsector->floor_yoffs)/fflatheight;
			angle = gl_frontsector->floorpic_angle;
		}
		else // it's a ceiling
		{
			scrollx = FIXED_TO_FLOAT(gl_frontsector->ceiling_xoffs)/fflatwidth;
			scrolly = FIXED_TO_FLOAT(gl_frontsector->ceiling_yoffs)/fflatheight;
			angle = gl_frontsector->ceilingpic_angle;
		}
	}

	if (angle) // Only needs to be done if there's an altered angle
	{
		tempxsow = flatxref;
		tempytow = flatyref;

		anglef = ANG2RAD(InvAngle(angle));

		flatxref = (tempxsow * cos(anglef)) - (tempytow * sin(anglef));
		flatyref = (tempxsow * sin(anglef)) + (tempytow * cos(anglef));
	}

#define SETUP3DVERT(vert, vx, vy) {\
		/* Hurdler: add scrolling texture on floor/ceiling */\
		if (texflat)\
		{\
			vert->s = (float)((vx) / fflatwidth) + scrollx;\
			vert->t = -(float)((vy) / fflatheight) + scrolly;\
		}\
		else\
		{\
			vert->s = (float)(((vx) / fflatwidth) - flatxref + scrollx);\
			vert->t = (float)(flatyref - ((vy) / fflatheight) + scrolly);\
		}\
\
		/* Need to rotate before translate */\
		if (angle) /* Only needs to be done if there's an altered angle */\
		{\
			tempxsow = vert->s;\
			tempytow = vert->t;\
			vert->s = (tempxsow * cos(anglef)) - (tempytow * sin(anglef));\
			vert->t = (tempxsow * sin(anglef)) + (tempytow * cos(anglef));\
		}\
\
		vert->x = (vx);\
		vert->y = height;\
		vert->z = (vy);\
\
		if (slope)\
		{\
			fixedheight = P_GetSlopeZAt(slope, FLOAT_TO_FIXED((vx)), FLOAT_TO_FIXED((vy)));\
			vert->y = FIXED_TO_FLOAT(fixedheight);\
		}\
}

	for (i = 0, v3d = planeVerts; i < nrPlaneVerts; i++,v3d++,pv++)
		SETUP3DVERT(v3d, pv->x, pv->y);

	if (slope)
		lightlevel = HWR_CalcSlopeLight(lightlevel, R_PointToAngle2(0, 0, slope->normal.x, slope->normal.y), abs(slope->zdelta));

	HWR_Lighting(&Surf, lightlevel, planecolormap);

	if (PolyFlags & (PF_Translucent|PF_Fog|PF_Additive|PF_Subtractive|PF_ReverseSubtract|PF_Multiplicative|PF_Environment))
	{
		Surf.PolyColor.s.alpha = (UINT8)alpha;
		PolyFlags |= PF_Modulated;
	}
	else
		PolyFlags |= PF_Masked|PF_Modulated;

	if (HWR_UseShader())
	{
		if (PolyFlags & PF_Fog)
			shader = SHADER_FOG;
		else if (PolyFlags & PF_Ripple)
			shader = SHADER_WATER;
		else
			shader = SHADER_FLOOR;

		PolyFlags |= PF_ColorMapped;
	}

	HWR_ProcessPolygon(&Surf, planeVerts, nrPlaneVerts, PolyFlags, shader, false);

	if (subsector)
	{
		// Horizon lines
		FOutVector horizonpts[6];
		float dist, vx, vy;
		float x1, y1, xd, yd;
		UINT8 numplanes, j;
		vertex_t v; // For determining the closest distance from the line to the camera, to split render planes for minimum distortion;

		const float renderdist = 27000.0f; // How far out to properly render the plane
		const float farrenderdist = 32768.0f; // From here, raise plane to horizon level to fill in the line with some texture distortion

		seg_t *line = &segs[subsector->firstline];

		for (i = 0; i < subsector->numlines; i++, line++)
		{
			if (!line->glseg && line->linedef->special == HORIZONSPECIAL && R_PointOnSegSide(viewx, viewy, line) == 0)
			{
				P_ClosestPointOnLine(viewx, viewy, line->linedef, &v);
				dist = FIXED_TO_FLOAT(R_PointToDist(v.x, v.y));

				x1 = ((polyvertex_t *)line->pv1)->x;
				y1 = ((polyvertex_t *)line->pv1)->y;
				xd = ((polyvertex_t *)line->pv2)->x - x1;
				yd = ((polyvertex_t *)line->pv2)->y - y1;

				// Based on the seg length and the distance from the line, split horizon into multiple poly sets to reduce distortion
				dist = sqrtf((xd*xd) + (yd*yd)) / dist / 16.0f;
				if (dist > 100.0f)
					numplanes = 100;
				else
					numplanes = (UINT8)dist + 1;

				for (j = 0; j < numplanes; j++)
				{
					// Left side
					vx = x1 + xd * j / numplanes;
					vy = y1 + yd * j / numplanes;
					SETUP3DVERT((&horizonpts[1]), vx, vy);

					dist = sqrtf(powf(vx - gl_viewx, 2) + powf(vy - gl_viewy, 2));
					vx = (vx - gl_viewx) * renderdist / dist + gl_viewx;
					vy = (vy - gl_viewy) * renderdist / dist + gl_viewy;
					SETUP3DVERT((&horizonpts[0]), vx, vy);

					// Right side
					vx = x1 + xd * (j+1) / numplanes;
					vy = y1 + yd * (j+1) / numplanes;
					SETUP3DVERT((&horizonpts[2]), vx, vy);

					dist = sqrtf(powf(vx - gl_viewx, 2) + powf(vy - gl_viewy, 2));
					vx = (vx - gl_viewx) * renderdist / dist + gl_viewx;
					vy = (vy - gl_viewy) * renderdist / dist + gl_viewy;
					SETUP3DVERT((&horizonpts[3]), vx, vy);

					// Horizon fills
					vx = (horizonpts[0].x - gl_viewx) * farrenderdist / renderdist + gl_viewx;
					vy = (horizonpts[0].z - gl_viewy) * farrenderdist / renderdist + gl_viewy;
					SETUP3DVERT((&horizonpts[5]), vx, vy);
					horizonpts[5].y = gl_viewz;

					vx = (horizonpts[3].x - gl_viewx) * farrenderdist / renderdist + gl_viewx;
					vy = (horizonpts[3].z - gl_viewy) * farrenderdist / renderdist + gl_viewy;
					SETUP3DVERT((&horizonpts[4]), vx, vy);
					horizonpts[4].y = gl_viewz;

					// Draw
					HWR_ProcessPolygon(&Surf, horizonpts, 6, PolyFlags, shader, true);
				}
			}
		}
	}

#ifdef ALAM_LIGHTING
	// add here code for dynamic lighting on planes
	HWR_PlaneLighting(planeVerts, nrPlaneVerts);
#endif
}

void HWR_RenderPolyObjectPlane(polyobj_t *polysector, boolean isceiling, fixed_t fixedheight,
								FBITFIELD blendmode, UINT8 lightlevel, levelflat_t *levelflat, sector_t *FOFsector,
								UINT8 alpha, extracolormap_t *planecolormap)
{
	FSurfaceInfo Surf;
	FOutVector *v3d;
	INT32 shader = -1;

	size_t nrPlaneVerts = polysector->numVertices;
	INT32 i;

	float height = FIXED_TO_FLOAT(fixedheight); // constant y for all points on the convex flat polygon
	float flatxref, flatyref;
	float fflatwidth = 64.0f, fflatheight = 64.0f;
	INT32 flatflag = 63;

	boolean texflat = false;

	float scrollx = 0.0f, scrolly = 0.0f;
	angle_t angle = 0;
	fixed_t tempxs, tempyt;

	static FOutVector *planeVerts = NULL;
	static UINT16 numAllocedPlaneVerts = 0;

	if (nrPlaneVerts < 3)   // Not even a triangle?
		return;
	else if (nrPlaneVerts > (size_t)UINT16_MAX) // FIXME: exceeds plVerts size
	{
		CONS_Debug(DBG_RENDER, "polygon size of %s exceeds max value of %d vertices\n", sizeu1(nrPlaneVerts), UINT16_MAX);
		return;
	}

	// Allocate plane-vertex buffer if we need to
	if (!planeVerts || nrPlaneVerts > numAllocedPlaneVerts)
	{
		numAllocedPlaneVerts = (UINT16)nrPlaneVerts;
		Z_Free(planeVerts);
		Z_Malloc(numAllocedPlaneVerts * sizeof (FOutVector), PU_LEVEL, &planeVerts);
	}

	// set texture for polygon
	if (levelflat != NULL)
	{
		if (levelflat->type == LEVELFLAT_FLAT)
		{
			size_t len = W_LumpLength(levelflat->u.flat.lumpnum);
			switch (len)
			{
				case 4194304: // 2048x2048 lump
					fflatwidth = fflatheight = 2048.0f;
					break;
				case 1048576: // 1024x1024 lump
					fflatwidth = fflatheight = 1024.0f;
					break;
				case 262144:// 512x512 lump
					fflatwidth = fflatheight = 512.0f;
					break;
				case 65536: // 256x256 lump
					fflatwidth = fflatheight = 256.0f;
					break;
				case 16384: // 128x128 lump
					fflatwidth = fflatheight = 128.0f;
					break;
				case 1024: // 32x32 lump
					fflatwidth = fflatheight = 32.0f;
					break;
				default: // 64x64 lump
					fflatwidth = fflatheight = 64.0f;
					break;
			}
			flatflag = ((INT32)fflatwidth)-1;
		}
		else
		{
			if (levelflat->type == LEVELFLAT_TEXTURE)
			{
				fflatwidth = textures[levelflat->u.texture.num]->width;
				fflatheight = textures[levelflat->u.texture.num]->height;
			}
			else if (levelflat->type == LEVELFLAT_PATCH || levelflat->type == LEVELFLAT_PNG)
			{
				fflatwidth = levelflat->width;
				fflatheight = levelflat->height;
			}
			texflat = true;
		}
	}
	else // set no texture
		HWR_SetCurrentTexture(NULL);

	// reference point for flat texture coord for each vertex around the polygon
	flatxref = FIXED_TO_FLOAT(polysector->origVerts[0].x);
	flatyref = FIXED_TO_FLOAT(polysector->origVerts[0].y);

	flatxref = (float)(((fixed_t)flatxref & (~flatflag)) / fflatwidth);
	flatyref = (float)(((fixed_t)flatyref & (~flatflag)) / fflatheight);

	// transform
	v3d = planeVerts;

	if (FOFsector != NULL)
	{
		if (!isceiling) // it's a floor
		{
			scrollx = FIXED_TO_FLOAT(FOFsector->floor_xoffs)/fflatwidth;
			scrolly = FIXED_TO_FLOAT(FOFsector->floor_yoffs)/fflatheight;
			angle = FOFsector->floorpic_angle;
		}
		else // it's a ceiling
		{
			scrollx = FIXED_TO_FLOAT(FOFsector->ceiling_xoffs)/fflatwidth;
			scrolly = FIXED_TO_FLOAT(FOFsector->ceiling_yoffs)/fflatheight;
			angle = FOFsector->ceilingpic_angle;
		}
	}
	else if (gl_frontsector)
	{
		if (!isceiling) // it's a floor
		{
			scrollx = FIXED_TO_FLOAT(gl_frontsector->floor_xoffs)/fflatwidth;
			scrolly = FIXED_TO_FLOAT(gl_frontsector->floor_yoffs)/fflatheight;
			angle = gl_frontsector->floorpic_angle;
		}
		else // it's a ceiling
		{
			scrollx = FIXED_TO_FLOAT(gl_frontsector->ceiling_xoffs)/fflatwidth;
			scrolly = FIXED_TO_FLOAT(gl_frontsector->ceiling_yoffs)/fflatheight;
			angle = gl_frontsector->ceilingpic_angle;
		}
	}

	if (angle) // Only needs to be done if there's an altered angle
	{
		angle = (InvAngle(angle))>>ANGLETOFINESHIFT;

		// This needs to be done so that it scrolls in a different direction after rotation like software
		/*tempxs = FLOAT_TO_FIXED(scrollx);
		tempyt = FLOAT_TO_FIXED(scrolly);
		scrollx = (FIXED_TO_FLOAT(FixedMul(tempxs, FINECOSINE(angle)) - FixedMul(tempyt, FINESINE(angle))));
		scrolly = (FIXED_TO_FLOAT(FixedMul(tempxs, FINESINE(angle)) + FixedMul(tempyt, FINECOSINE(angle))));*/

		// This needs to be done so everything aligns after rotation
		// It would be done so that rotation is done, THEN the translation, but I couldn't get it to rotate AND scroll like software does
		tempxs = FLOAT_TO_FIXED(flatxref);
		tempyt = FLOAT_TO_FIXED(flatyref);
		flatxref = (FIXED_TO_FLOAT(FixedMul(tempxs, FINECOSINE(angle)) - FixedMul(tempyt, FINESINE(angle))));
		flatyref = (FIXED_TO_FLOAT(FixedMul(tempxs, FINESINE(angle)) + FixedMul(tempyt, FINECOSINE(angle))));
	}

	for (i = 0; i < (INT32)nrPlaneVerts; i++,v3d++)
	{
		// Go from the polysector's original vertex locations
		// Means the flat is offset based on the original vertex locations
		if (texflat)
		{
			v3d->s = (float)(FIXED_TO_FLOAT(polysector->origVerts[i].x) / fflatwidth) + scrollx;
			v3d->t = -(float)(FIXED_TO_FLOAT(polysector->origVerts[i].y) / fflatheight) + scrolly;
		}
		else
		{
			v3d->s = (float)((FIXED_TO_FLOAT(polysector->origVerts[i].x) / fflatwidth) - flatxref + scrollx);
			v3d->t = (float)(flatyref - (FIXED_TO_FLOAT(polysector->origVerts[i].y) / fflatheight) + scrolly);
		}

		// Need to rotate before translate
		if (angle) // Only needs to be done if there's an altered angle
		{
			tempxs = FLOAT_TO_FIXED(v3d->s);
			tempyt = FLOAT_TO_FIXED(v3d->t);
			v3d->s = (FIXED_TO_FLOAT(FixedMul(tempxs, FINECOSINE(angle)) - FixedMul(tempyt, FINESINE(angle))));
			v3d->t = (FIXED_TO_FLOAT(FixedMul(tempxs, FINESINE(angle)) + FixedMul(tempyt, FINECOSINE(angle))));
		}

		v3d->x = FIXED_TO_FLOAT(polysector->vertices[i]->x);
		v3d->y = height;
		v3d->z = FIXED_TO_FLOAT(polysector->vertices[i]->y);
	}

	HWR_Lighting(&Surf, lightlevel, planecolormap);

	if (blendmode & PF_Translucent)
	{
		Surf.PolyColor.s.alpha = (UINT8)alpha;
		blendmode |= PF_Modulated|PF_Occlude;
	}
	else
		blendmode |= PF_Masked|PF_Modulated;

	if (HWR_UseShader())
	{
		shader = SHADER_FLOOR;
		blendmode |= PF_ColorMapped;
	}

	HWR_ProcessPolygon(&Surf, planeVerts, nrPlaneVerts, blendmode, shader, false);
}

#endif
