// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 1999-2022 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file hw_drawnodes.c
/// \brief Sorting and rendering translucent surfaces with gl drawnodes

#ifdef HWRENDER
#include "hw_drv.h"
#include "hw_glob.h"
#include "../i_system.h"
#include "../r_local.h"
#include "../z_zone.h"

#define ABS(x) ((x) < 0 ? -(x) : (x))

// A drawnode is something that points to a translucent wall or floor.

typedef struct
{
	FOutVector    wallVerts[4];
	FSurfaceInfo  Surf;
	INT32         texnum;
	FBITFIELD     blend;
	boolean fogwall;
	INT32 lightlevel;
	extracolormap_t *wallcolormap; // Doing the lighting in HWR_RenderWall now for correct fog after sorting
} wallinfo_t;

typedef struct
{
	extrasubsector_t *xsub;
	boolean isceiling;
	fixed_t fixedheight;
	INT32 lightlevel;
	levelflat_t *levelflat;
	INT32 alpha;
	sector_t *FOFSector;
	FBITFIELD blend;
	boolean fogplane;
	extracolormap_t *planecolormap;
} planeinfo_t;

typedef struct
{
	polyobj_t *polysector;
	boolean isceiling;
	fixed_t fixedheight;
	INT32 lightlevel;
	levelflat_t *levelflat;
	INT32 alpha;
	sector_t *FOFSector;
	FBITFIELD blend;
	extracolormap_t *planecolormap;
} polyplaneinfo_t;

typedef enum
{
	DRAWNODE_PLANE,
	DRAWNODE_POLYOBJECT_PLANE,
	DRAWNODE_WALL
} gl_drawnode_type_t;

typedef struct
{
	gl_drawnode_type_t type;
	union {
		planeinfo_t plane;
		polyplaneinfo_t polyplane;
		wallinfo_t wall;
	} u;
} gl_drawnode_t;

// initial size of drawnode array
#define DRAWNODES_INIT_SIZE 64

typedef struct
{
	gl_drawnode_t *drawnodes;
	INT32 numdrawnodes;
	INT32 alloceddrawnodes;
} gl_drawnode_state_t;

// portal rendering will push and pop this stack
// to keep multiple drawnode lists around until they're needed
static gl_drawnode_state_t state_stack[MAXPORTALS_CAP+1] = {0};
static int stack_level = 0;
static gl_drawnode_state_t *cst = &state_stack[0]; // current state

static void *HWR_CreateDrawNode(gl_drawnode_type_t type)
{
	gl_drawnode_t *drawnode;

	if (!cst->drawnodes)
	{
		cst->alloceddrawnodes = DRAWNODES_INIT_SIZE;
		cst->drawnodes = Z_Malloc(cst->alloceddrawnodes * sizeof(gl_drawnode_t), PU_LEVEL, &cst->drawnodes);
	}
	else if (cst->numdrawnodes >= cst->alloceddrawnodes)
	{
		cst->alloceddrawnodes *= 2;
		Z_Realloc(cst->drawnodes, cst->alloceddrawnodes * sizeof(gl_drawnode_t), PU_LEVEL, &cst->drawnodes);
	}

	drawnode = &cst->drawnodes[cst->numdrawnodes++];
	drawnode->type = type;

	// not sure if returning different pointers to a union is necessary
	switch (type)
	{
		case DRAWNODE_PLANE:
			return &drawnode->u.plane;
		case DRAWNODE_POLYOBJECT_PLANE:
			return &drawnode->u.polyplane;
		case DRAWNODE_WALL:
			return &drawnode->u.wall;
	}

	return NULL;
}

void HWR_AddTransparentWall(FOutVector *wallVerts, FSurfaceInfo *pSurf, INT32 texnum, FBITFIELD blend, boolean fogwall, INT32 lightlevel, extracolormap_t *wallcolormap)
{
	wallinfo_t *wallinfo = HWR_CreateDrawNode(DRAWNODE_WALL);

	M_Memcpy(wallinfo->wallVerts, wallVerts, sizeof (wallinfo->wallVerts));
	M_Memcpy(&wallinfo->Surf, pSurf, sizeof (FSurfaceInfo));
	wallinfo->texnum = texnum;
	wallinfo->blend = blend;
	wallinfo->fogwall = fogwall;
	wallinfo->lightlevel = lightlevel;
	wallinfo->wallcolormap = wallcolormap;
}

void HWR_AddTransparentFloor(levelflat_t *levelflat, extrasubsector_t *xsub, boolean isceiling, fixed_t fixedheight, INT32 lightlevel, INT32 alpha, sector_t *FOFSector, FBITFIELD blend, boolean fogplane, extracolormap_t *planecolormap)
{
	planeinfo_t *planeinfo = HWR_CreateDrawNode(DRAWNODE_PLANE);

	planeinfo->isceiling = isceiling;
	planeinfo->fixedheight = fixedheight;
	planeinfo->lightlevel = (planecolormap && (planecolormap->flags & CMF_FOG)) ? lightlevel : 255;
	planeinfo->levelflat = levelflat;
	planeinfo->xsub = xsub;
	planeinfo->alpha = alpha;
	planeinfo->FOFSector = FOFSector;
	planeinfo->blend = blend;
	planeinfo->fogplane = fogplane;
	planeinfo->planecolormap = planecolormap;
}

// Adding this for now until I can create extrasubsector info for polyobjects
// When that happens it'll just be done through HWR_AddTransparentFloor and HWR_RenderPlane
void HWR_AddTransparentPolyobjectFloor(levelflat_t *levelflat, polyobj_t *polysector, boolean isceiling, fixed_t fixedheight, INT32 lightlevel, INT32 alpha, sector_t *FOFSector, FBITFIELD blend, extracolormap_t *planecolormap)
{
	polyplaneinfo_t *polyplaneinfo = HWR_CreateDrawNode(DRAWNODE_POLYOBJECT_PLANE);

	polyplaneinfo->isceiling = isceiling;
	polyplaneinfo->fixedheight = fixedheight;
	polyplaneinfo->lightlevel = (planecolormap && (planecolormap->flags & CMF_FOG)) ? lightlevel : 255;
	polyplaneinfo->levelflat = levelflat;
	polyplaneinfo->polysector = polysector;
	polyplaneinfo->alpha = alpha;
	polyplaneinfo->FOFSector = FOFSector;
	polyplaneinfo->blend = blend;
	polyplaneinfo->planecolormap = planecolormap;
}

// pushes all drawnode rendering state to stack
void HWR_PushDrawNodeState(void)
{
	if (stack_level == MAXPORTALS_CAP)
		I_Error("HWR_PushDrawNodeState: State stack overflow");

	stack_level++;
	cst++;
}

void HWR_PopDrawNodeState(void)
{
	if (stack_level == 0)
		I_Error("HWR_PopDrawNodeState: State stack underflow");

	stack_level--;
	cst--;
}

static int CompareDrawNodePlanes(const void *p1, const void *p2)
{
	INT32 n1 = *(const INT32*)p1;
	INT32 n2 = *(const INT32*)p2;

	return ABS(cst->drawnodes[n2].u.plane.fixedheight - viewz) - ABS(cst->drawnodes[n1].u.plane.fixedheight - viewz);
}

// Sorts and renders the list of drawnodes for the scene being rendered.
void HWR_RenderDrawNodes(void)
{
	INT32 i = 0, run_start = 0;

	// Array for storing the rendering order.
	// A list of indices into the drawnodes array.
	INT32 *sortindex;

	if (!cst->numdrawnodes)
		return;

	ps_numdrawnodes.value.i = cst->numdrawnodes;

	PS_START_TIMING(ps_hw_nodesorttime);

	sortindex = Z_Malloc(sizeof(INT32) * cst->numdrawnodes, PU_STATIC, NULL);

	// Reversed order
	for (i = 0; i < cst->numdrawnodes; i++)
		sortindex[i] = cst->numdrawnodes - i - 1;

	// The order is correct apart from planes in the same subsector.
	// So scan the list and sort out these cases.
	// For each consecutive run of planes in the list, sort that run based on
	// plane height and view height.
	while (run_start < cst->numdrawnodes-1) // numdrawnodes-1 because a 1 plane run at the end of the list does not count
	{
		// locate run start
		if (cst->drawnodes[sortindex[run_start]].type == DRAWNODE_PLANE)
		{
			// found it, now look for run end
			INT32 run_end; // (inclusive)

			for (i = run_start+1; i < cst->numdrawnodes; i++)
			{
				if (cst->drawnodes[sortindex[i]].type != DRAWNODE_PLANE) break;
			}
			run_end = i-1;
			if (run_end > run_start) // if there are multiple consecutive planes, not just one
			{
				// consecutive run of planes found, now sort it
				qsort(sortindex + run_start, run_end - run_start + 1, sizeof(INT32), CompareDrawNodePlanes);
			}
			run_start = run_end + 1; // continue looking for runs coming right after this one
		}
		else
		{
			// this wasnt the run start, try next one
			run_start++;
		}
	}

	PS_STOP_TIMING(ps_hw_nodesorttime);

	PS_START_TIMING(ps_hw_nodedrawtime);

	// Okay! Let's draw it all! Woo!
	HWD.pfnSetTransform(&atransform);

	for (i = 0; i < cst->numdrawnodes; i++)
	{
		gl_drawnode_t *drawnode = &cst->drawnodes[sortindex[i]];

		if (drawnode->type == DRAWNODE_PLANE)
		{
			planeinfo_t *plane = &drawnode->u.plane;

			// We aren't traversing the BSP tree, so make gl_frontsector null to avoid crashes.
			gl_frontsector = NULL;

			if (!(plane->blend & PF_NoTexture))
				HWR_GetLevelFlat(plane->levelflat);
			HWR_RenderPlane(NULL, plane->xsub, plane->isceiling, plane->fixedheight, plane->blend, plane->lightlevel,
				plane->levelflat, plane->FOFSector, plane->alpha, plane->planecolormap);
		}
		else if (drawnode->type == DRAWNODE_POLYOBJECT_PLANE)
		{
			polyplaneinfo_t *polyplane = &drawnode->u.polyplane;

			// We aren't traversing the BSP tree, so make gl_frontsector null to avoid crashes.
			gl_frontsector = NULL;

			if (!(polyplane->blend & PF_NoTexture))
				HWR_GetLevelFlat(polyplane->levelflat);
			HWR_RenderPolyObjectPlane(polyplane->polysector, polyplane->isceiling, polyplane->fixedheight, polyplane->blend, polyplane->lightlevel,
				polyplane->levelflat, polyplane->FOFSector, polyplane->alpha, polyplane->planecolormap);
		}
		else if (drawnode->type == DRAWNODE_WALL)
		{
			wallinfo_t *wall = &drawnode->u.wall;

			if (!(wall->blend & PF_NoTexture))
				HWR_GetTexture(wall->texnum);
			HWR_RenderWall(wall->wallVerts, &wall->Surf, wall->blend, wall->fogwall,
				wall->lightlevel, wall->wallcolormap);
		}
	}

	PS_STOP_TIMING(ps_hw_nodedrawtime);

	cst->numdrawnodes = 0;

	Z_Free(sortindex);
}

#endif
