// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 1993-1996 by id Software, Inc.
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 1999-2000 by Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze, Andrey Budko (prboom)
// Copyright (C) 1999-2021 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  r_fps.h
/// \brief Uncapped framerate stuff.

#include "r_fps.h"

#include "info.h"
#include "p_local.h"
#include "p_polyobj.h"
#include "r_main.h"
#include "d_clisrv.h"
#include "g_game.h"
#include "i_video.h"
#include "r_plane.h"
#include "r_things.h"
#include "p_slopes.h"
#include "p_spec.h"
#include "r_state.h"
#include "hardware/hw_main.h"
#include "i_system.h"

static viewvars_t p1view_old;
static viewvars_t p1view_new;
static viewvars_t p2view_old;
static viewvars_t p2view_new;
static viewvars_t sky1view_old;
static viewvars_t sky1view_new;
static viewvars_t sky2view_old;
static viewvars_t sky2view_new;

static viewvars_t *oldview = &p1view_old;
viewvars_t *newview = &p1view_new;

#define ISA(_THINKNAME_) th->function.acp1 == (actionf_p1)_THINKNAME_
#define CAST(_NAME_,_TYPE_) _TYPE_ *_NAME_ = (_TYPE_ *)th

enum viewcontext_e viewcontext = VIEWCONTEXT_PLAYER1;

static void SetPolyobjOldState(polyobj_t *pobj)
{
	UINT32 i;

	for (i = 0; i < pobj->numVertices; i++)
	{
		vertex_t *vert = &pobj->newVerts[i];
		pobj->oldVerts[i].x = vert->x;
		pobj->oldVerts[i].y = vert->y;
	}
	pobj->oldCenterPt = pobj->newCenterPt;
}

static void SetPolyobjNewState(polyobj_t *pobj)
{
	UINT32 i;

	if (pobj->firstlerp != 1)
	{
		pobj->firstlerp = 1;
		for (i = 0; i < pobj->numVertices; i++)
		{
			vertex_t *vert = pobj->vertices[i];
			pobj->oldVerts[i].x = vert->x;
			pobj->oldVerts[i].y = vert->y;
		}
		pobj->oldCenterPt = pobj->centerPt;
	}

	for (i = 0; i < pobj->numVertices; i++)
	{
		vertex_t *vert = pobj->vertices[i];
		pobj->newVerts[i].x = vert->x;
		pobj->newVerts[i].y = vert->y;
	}
	pobj->newCenterPt = pobj->centerPt;
}

static void ResetPolyobjState(polyobj_t *pobj)
{
	UINT32 i;

	for (i = 0; i < pobj->numVertices; i++)
	{
		vertex_t *vert = &pobj->newVerts[i];
		pobj->vertices[i]->x = vert->x;
		pobj->vertices[i]->y = vert->y;
	}
	pobj->centerPt = pobj->newCenterPt;
}

static void LerpPolyobjState(polyobj_t *pobj, fixed_t frac)
{
	UINT32 i;

	for (i = 0; i < pobj->numVertices; i++)
	{
		vertex_t *oldVert = &pobj->oldVerts[i];
		vertex_t *newVert = &pobj->newVerts[i];
		pobj->vertices[i]->x = oldVert->x + R_LerpFixed(oldVert->x, newVert->x, frac);
		pobj->vertices[i]->y = oldVert->y + R_LerpFixed(oldVert->y, newVert->y, frac);
	}
	pobj->centerPt.x = pobj->oldCenterPt.x + R_LerpFixed(pobj->oldCenterPt.x, pobj->newCenterPt.x, frac);
	pobj->centerPt.y = pobj->oldCenterPt.y + R_LerpFixed(pobj->oldCenterPt.y, pobj->newCenterPt.y, frac);
}

// recalc necessary stuff for mouseaiming
// slopes are already calculated for the full possible view (which is 4*viewheight).
// 18/08/18: (No it's actually 16*viewheight, thanks Jimita for finding this out)
static void R_SetupFreelook(player_t *player, boolean skybox)
{
#ifndef HWRENDER
	(void)player;
	(void)skybox;
#endif

	// clip it in the case we are looking a hardware 90 degrees full aiming
	// (lmps, network and use F12...)
	if (rendermode == render_soft
#ifdef HWRENDER
		|| (rendermode == render_opengl
			&& (cv_glshearing.value == 1
			|| (cv_glshearing.value == 2 && R_IsViewpointThirdPerson(player, skybox))))
#endif
		)
	{
		G_SoftwareClipAimingPitch((INT32 *)&aimingangle);
	}

	centeryfrac = (viewheight/2)<<FRACBITS;

	if (rendermode == render_soft)
		centeryfrac += FixedMul(AIMINGTODY(aimingangle), FixedDiv(viewwidth<<FRACBITS, BASEVIDWIDTH<<FRACBITS));

	centery = FixedInt(FixedRound(centeryfrac));

	if (rendermode == render_soft)
		yslope = &yslopetab[viewheight*8 - centery];
}

//
// R_GetShadowZ(thing, shadowslope)
// Get the first visible floor below the object for shadows
// shadowslope is filled with the floor's slope, if provided
//
fixed_t R_GetShadowZ(mobj_t *thing, pslope_t **shadowslope)
{
	boolean isflipped = thing->eflags & MFE_VERTICALFLIP;
	fixed_t z, groundz = isflipped ? INT32_MAX : INT32_MIN;
	pslope_t *slope, *groundslope = NULL;
	msecnode_t *node;
	sector_t *sector;
	ffloor_t *rover;

#define CHECKZ (isflipped ? z > thing->z+thing->height/2 && z < groundz : z < thing->z+thing->height/2 && z > groundz)

	for (node = thing->touching_sectorlist; node; node = node->m_sectorlist_next)
	{
		sector = node->m_sector;

		slope = sector->heightsec != -1 ? NULL : (isflipped ? sector->c_slope : sector->f_slope);

		if (sector->heightsec != -1)
			z = isflipped ? sectors[sector->heightsec].ceilingheight : sectors[sector->heightsec].floorheight;
		else
			z = isflipped ? P_GetSectorCeilingZAt(sector, thing->x, thing->y) : P_GetSectorFloorZAt(sector, thing->x, thing->y);

		if CHECKZ
		{
			groundz = z;
			groundslope = slope;
		}

		if (sector->ffloors)
		{
			for (rover = sector->ffloors; rover; rover = rover->next)
			{
				if (!(rover->flags & FF_EXISTS) || !(rover->flags & FF_RENDERPLANES) || (rover->alpha < 90 && !(rover->flags & FF_SWIMMABLE)))
					continue;

				z = isflipped ? P_GetFFloorBottomZAt(rover, thing->x, thing->y) : P_GetFFloorTopZAt(rover, thing->x, thing->y);
				if CHECKZ
				{
					groundz = z;
					groundslope = isflipped ? *rover->b_slope : *rover->t_slope;
				}
			}
		}
	}

	if (!groundslope && !groundz)
	{
		if (isflipped ? (thing->ceilingz < groundz - (!groundslope ? 0 : FixedMul(abs(groundslope->zdelta), thing->radius*3/2)))
			: (thing->floorz > groundz + (!groundslope ? 0 : FixedMul(abs(groundslope->zdelta), thing->radius*3/2))))
		{
			groundz = isflipped ? thing->ceilingz : thing->floorz;
			groundslope = NULL;
		}
	}

#if 0 // Unfortunately, this drops CEZ2 down to sub-17 FPS on my i7.
	// NOTE: this section was not updated to reflect reverse gravity support
	// Check polyobjects and see if groundz needs to be altered, for rings only because they don't update floorz
	if (thing->type == MT_RING)
	{
		INT32 xl, xh, yl, yh, bx, by;

		xl = (unsigned)(thing->x - thing->radius - bmaporgx)>>MAPBLOCKSHIFT;
		xh = (unsigned)(thing->x + thing->radius - bmaporgx)>>MAPBLOCKSHIFT;
		yl = (unsigned)(thing->y - thing->radius - bmaporgy)>>MAPBLOCKSHIFT;
		yh = (unsigned)(thing->y + thing->radius - bmaporgy)>>MAPBLOCKSHIFT;

		BMBOUNDFIX(xl, xh, yl, yh);

		validcount++;

		for (by = yl; by <= yh; by++)
			for (bx = xl; bx <= xh; bx++)
			{
				INT32 offset;
				polymaplink_t *plink; // haleyjd 02/22/06

				if (bx < 0 || by < 0 || bx >= bmapwidth || by >= bmapheight)
					continue;

				offset = by*bmapwidth + bx;

				// haleyjd 02/22/06: consider polyobject lines
				plink = polyblocklinks[offset];

				while (plink)
				{
					polyobj_t *po = plink->po;

					if (po->validcount != validcount) // if polyobj hasn't been checked
					{
						po->validcount = validcount;

						if (!P_MobjInsidePolyobj(po, thing) || !(po->flags & POF_RENDERPLANES))
						{
							plink = (polymaplink_t *)(plink->link.next);
							continue;
						}

						// We're inside it! Yess...
						z = po->lines[0]->backsector->ceilingheight;

						if (z < thing->z+thing->height/2 && z > groundz)
						{
							groundz = z;
							groundslope = NULL;
						}
					}
					plink = (polymaplink_t *)(plink->link.next);
				}
			}
	}
#endif

	if (shadowslope != NULL)
		*shadowslope = groundslope;

	return groundz;
#undef CHECKZ
}

void R_InterpolateView(player_t *player, boolean skybox, fixed_t frac)
{
	if (frac < 0)
		frac = 0;
	if (frac > FRACUNIT)
		frac = FRACUNIT;

	if (R_IsViewpointThirdPerson(player, skybox))
	{
		viewx = oldview->x + R_LerpFixed(oldview->x, newview->x, frac);
		viewy = oldview->y + R_LerpFixed(oldview->y, newview->y, frac);
	}
	else // already interpolated by the player mobj
	{
		viewx = newview->x;
		viewy = newview->y;
	}

	viewz = oldview->z + R_LerpFixed(oldview->z, newview->z, frac);

	viewangle = oldview->angle + R_LerpAngle(oldview->angle, newview->angle, frac);
	aimingangle = oldview->aim + R_LerpAngle(oldview->aim, newview->aim, frac);

	viewsin = FINESINE(viewangle>>ANGLETOFINESHIFT);
	viewcos = FINECOSINE(viewangle>>ANGLETOFINESHIFT);

	// this is gonna create some interesting visual errors for long distance teleports...
	// might want to recalculate the view sector every frame instead...
	viewplayer = newview->player;
	viewsector = R_PointInSubsector(viewx, viewy)->sector;

	R_SetupFreelook(player, skybox);
}

void R_UpdateViewInterpolation(void)
{
	p1view_old = p1view_new;
	p2view_old = p2view_new;
	sky1view_old = sky1view_new;
	sky2view_old = sky2view_new;
}

void R_SetViewContext(enum viewcontext_e _viewcontext)
{
	I_Assert(_viewcontext == VIEWCONTEXT_PLAYER1
			|| _viewcontext == VIEWCONTEXT_PLAYER2
			|| _viewcontext == VIEWCONTEXT_SKY1
			|| _viewcontext == VIEWCONTEXT_SKY2);
	viewcontext = _viewcontext;

	switch (viewcontext)
	{
		case VIEWCONTEXT_PLAYER1:
			oldview = &p1view_old;
			newview = &p1view_new;
			break;
		case VIEWCONTEXT_PLAYER2:
			oldview = &p2view_old;
			newview = &p2view_new;
			break;
		case VIEWCONTEXT_SKY1:
			oldview = &sky1view_old;
			newview = &sky1view_new;
			break;
		case VIEWCONTEXT_SKY2:
			oldview = &sky2view_old;
			newview = &sky2view_new;
			break;
		default:
			I_Error("viewcontext value is invalid: we should never get here without an assert!!");
			break;
	}
}

void R_SetThinkerOldStates(void)
{
	thinker_t *th;
	thinklistnum_t i;

	for (i = 0; i <= NUM_THINKERLISTS-1; i++)
	{
		for (th = thlist[i].next; th != &thlist[i]; th = th->next)
		{
			if (!th)
				break;

			if (i == THINK_MOBJ)
			{
				CAST(mo, mobj_t);
				mo->old_x = mo->new_x;
				mo->old_y = mo->new_y;
				mo->old_z = mo->new_z;
			}
			else if (i == THINK_PRECIP)
			{
				if (ISA(P_RainThinker) || ISA(P_SnowThinker))
				{
					CAST(mo, precipmobj_t);
					mo->old_x = mo->new_x;
					mo->old_y = mo->new_y;
					mo->old_z = mo->new_z;
				}
			}
			/*else if (i == THINK_POLYOBJ)
			{
				if (ISA(T_PolyObjRotate))
				{
					CAST(p, polyrotate_t);
					polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
					if (pobj == NULL) continue;
					SetPolyobjOldState(pobj);
				}
				else if (ISA(T_PolyObjMove)
					|| ISA(T_PolyObjFlag))
				{
					CAST(p, polymove_t);
					polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
					if (pobj == NULL) continue;
					SetPolyobjOldState(pobj);
				}
				else if (ISA(T_PolyObjWaypoint))
				{
					CAST(p, polywaypoint_t);
					polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
					if (pobj == NULL) continue;
					SetPolyobjOldState(pobj);
				}
				else if (ISA(T_PolyDoorSlide))
				{
					CAST(p, polyslidedoor_t);
					polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
					if (pobj == NULL) continue;
					SetPolyobjOldState(pobj);
				}
				else if (ISA(T_PolyDoorSwing))
				{
					CAST(p, polyswingdoor_t);
					polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
					if (pobj == NULL) continue;
					SetPolyobjOldState(pobj);
				}
				else if (ISA(T_PolyObjDisplace))
				{
					CAST(p, polydisplace_t);
					polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
					p->old_dx = p->new_dx;
					p->old_dy = p->new_dy;	
					if (pobj == NULL) continue;
					SetPolyobjOldState(pobj);
				}
				else if (ISA(T_PolyObjRotDisplace))
				{
					CAST(p, polyrotdisplace_t);
					polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
					if (pobj == NULL) continue;
					SetPolyobjOldState(pobj);
				}
			}*/
			// Other thinkers
			//else
			{
				if (ISA(T_MoveCeiling) || ISA(T_CrushCeiling))
				{
					CAST(s, ceiling_t);
					s->old_ceilingheight = s->new_ceilingheight;
				}
				else if (ISA(T_MoveFloor))
				{
					CAST(s, floormove_t);
					s->old_floorheight = s->new_floorheight;
				}
				else if (ISA(T_LightningFlash))
				{
					CAST(l, lightflash_t);
					l->old_lightlevel = l->new_lightlevel;
				}
				else if (ISA(T_StrobeFlash))
				{
					CAST(s, strobe_t);
					s->old_lightlevel = s->new_lightlevel;
					s->old_lightlevel = s->new_lightlevel;
				}
				else if (ISA(T_Glow))
				{
					CAST(g, glow_t);
					g->old_lightlevel = g->new_lightlevel;
				}
				else if (ISA(T_FireFlicker))
				{
					CAST(f, fireflicker_t);
					f->old_lightlevel = f->new_lightlevel;
				}
				else if (ISA(T_MoveElevator)
					|| ISA(T_CameraScanner))
				{
					CAST(e, elevator_t);
					e->old_floorheight = e->new_floorheight;
					e->old_ceilingheight = e->new_ceilingheight;
				}
				else if (ISA(T_StartCrumble))
				{
					CAST(c, crumble_t);
					c->old_floorheight = c->new_floorheight;
					c->old_ceilingheight = c->new_ceilingheight;
				}
				else if (ISA(T_ContinuousFalling))
				{
					CAST(f, continuousfall_t);
					f->old_floorheight = f->new_floorheight;
					f->old_ceilingheight = f->new_ceilingheight;
				}
				else if (ISA(T_ThwompSector))
				{
					CAST(f, thwomp_t);
					f->old_floorheight = f->new_floorheight;
					f->old_ceilingheight = f->new_ceilingheight;
				}
				else if (ISA(T_RaiseSector))
				{
					CAST(f, raise_t);
					f->old_floorheight = f->new_floorheight;
					f->old_ceilingheight = f->new_ceilingheight;
				}
				else if (ISA(T_BounceCheese))
				{
					CAST(f, bouncecheese_t);
					f->old_floorheight = f->new_floorheight;
					f->old_ceilingheight = f->new_ceilingheight;
				}
				else if (ISA(T_MarioBlock))
				{
					CAST(f, mariothink_t);
					f->old_floorheight = f->new_floorheight;
					f->old_ceilingheight = f->new_ceilingheight;
				}
				else if (ISA(T_FloatSector))
				{
					CAST(f, floatthink_t);
					f->old_floorheight = f->new_floorheight;
					f->old_ceilingheight = f->new_ceilingheight;
				}
				/*if (ISA(T_LaserFlash))
				{
					//CAST(l, laserthink_t);
				}*/
				else if (ISA(T_LightFade))
				{
					CAST(l, lightlevel_t);
					l->old_lightlevel = l->new_lightlevel;
				}
				/*if (ISA(T_ExecutorDelay))
				{
					//CAST(e, executor_t);
				}*/
				/*if (ISA(T_Disappear))
				{
					//CAST(d, disappear_t);
				}*/
				else if (ISA(T_Scroll))
				{
					CAST(s, scroll_t);
					switch (s->type)
					{
						case sc_side:
							s->old_textureoffset = s->new_textureoffset;
							s->old_rowoffset = s->new_rowoffset;
							break;
						case sc_floor:
						case sc_ceiling:
							s->old_xoffs = s->new_xoffs;
							s->old_yoffs = s->new_yoffs;
							break;
						case sc_carry:
						case sc_carry_ceiling:
							break;
					}
				}
				/*if (ISA(T_Friction))
				{
					//CAST(f, friction_t);
				}*/
				/*if (ISA(T_Pusher))
				{
					//CAST(f, pusher_t);
				}*/
			}
		}
	}
}

void R_ResetFirstLerp(void)
{
	thinker_t *th;
	thinklistnum_t i;

	firsttics = 2;

	for (i = 0; i <= NUM_THINKERLISTS-1; i++)
	{
		for (th = thlist[i].next; th != &thlist[i]; th = th->next)
		{
			if (!th)
				break;

			if (i == THINK_MOBJ)
			{
				CAST(mo, mobj_t);
				mo->firstlerp = 0;
			}
			else if (i == THINK_PRECIP)
			{
				CAST(mo, precipmobj_t);
				mo->firstlerp = 0;
			}
			/*else if (i == THINK_POLYOBJ)
			{
				if (ISA(T_PolyObjRotate))
				{
					CAST(p, polyrotate_t);
					polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
					if (pobj == NULL) continue;
					pobj->firstlerp = 0;
				}
				else if (ISA(T_PolyObjMove)
					|| ISA(T_PolyObjFlag))
				{
					CAST(p, polymove_t);
					polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
					if (pobj == NULL) continue;
					pobj->firstlerp = 0;
				}
				else if (ISA(T_PolyObjWaypoint))
				{
					CAST(p, polywaypoint_t);
					polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
					if (pobj == NULL) continue;
					pobj->firstlerp = 0;
				}
				else if (ISA(T_PolyDoorSlide))
				{
					CAST(p, polyslidedoor_t);
					polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
					if (pobj == NULL) continue;
					pobj->firstlerp = 0;
				}
				else if (ISA(T_PolyDoorSwing))
				{
					CAST(p, polyswingdoor_t);
					polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
					if (pobj == NULL) continue;
					pobj->firstlerp = 0;
				}
				else if (ISA(T_PolyObjDisplace))
				{
					CAST(p, polydisplace_t);
					p->firstlerp = 0;
					polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
					if (pobj == NULL) continue;
					pobj->firstlerp = 0;
				}
				else if (ISA(T_PolyObjRotDisplace))
				{
					CAST(p, polyrotdisplace_t);
					polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
					if (pobj == NULL) continue;
					pobj->firstlerp = 0;
				}
			}*/
			// Other thinkers
			//else
			{
				if (ISA(T_MoveCeiling) || ISA(T_CrushCeiling))
				{
					CAST(s, ceiling_t);
					s->firstlerp = 0;
				}
				else if (ISA(T_MoveFloor))
				{
					CAST(s, floormove_t);
					s->firstlerp = 0;
				}
				else if (ISA(T_LightningFlash))
				{
					CAST(l, lightflash_t);
					l->firstlerp = 0;
				}
				else if (ISA(T_StrobeFlash))
				{
					CAST(s, strobe_t);
					s->firstlerp = 0;
				}
				else if (ISA(T_Glow))
				{
					CAST(g, glow_t);
					g->firstlerp = 0;
				}
				else if (ISA(T_FireFlicker))
				{
					CAST(f, fireflicker_t);
					f->firstlerp = 0;
				}
				else if (ISA(T_MoveElevator) || ISA(T_CameraScanner))
				{
					CAST(e, elevator_t);
					e->firstlerp = 0;
				}
				else if (ISA(T_StartCrumble))
				{
					CAST(c, crumble_t);
					c->firstlerp = 0;
				}
				else if (ISA(T_ContinuousFalling))
				{
					CAST(f, continuousfall_t);
					f->firstlerp = 0;
				}
				else if (ISA(T_ThwompSector))
				{
					CAST(f, thwomp_t);
					f->firstlerp = 0;
				}
				else if (ISA(T_RaiseSector))
				{
					CAST(f, raise_t);
					f->firstlerp = 0;
				}
				else if (ISA(T_BounceCheese))
				{
					CAST(f, bouncecheese_t);
					f->firstlerp = 0;
				}
				else if (ISA(T_MarioBlock))
				{
					CAST(f, mariothink_t);
					f->firstlerp = 0;
				}
				else if (ISA(T_FloatSector))
				{
					CAST(f, floatthink_t);
					f->firstlerp = 0;
				}
				/*if (ISA(T_LaserFlash))
				{
					//CAST(l, laserthink_t);
				}*/
				else if (ISA(T_LightFade))
				{
					CAST(l, lightlevel_t);
					l->firstlerp = 0;
				}
				/*if (ISA(T_ExecutorDelay))
				{
					//CAST(e, executor_t);
				}*/
				/*if (ISA(T_Disappear))
				{
					//CAST(d, disappear_t);
				}*/
				else if (ISA(T_Scroll))
				{
					CAST(s, scroll_t);
					s->firstlerp = 0;
				}
				/*if (ISA(T_Friction))
				{
					//CAST(f, friction_t);
				}*/
				/*if (ISA(T_Pusher))
				{
					//CAST(f, pusher_t);
				}*/
			}
		}
	}
}

void R_SetThinkerNewStates(void)
{
	thinker_t *th;
	thinklistnum_t i;

	for (i = 0; i <= NUM_THINKERLISTS-1; i++)
	{
		for (th = thlist[i].next; th != &thlist[i]; th = th->next)
		{
			if (!th)
				break;

			if (i == THINK_MOBJ)
			{
				CAST(mo, mobj_t);
				if (!mo->firstlerp)
				{
					mo->firstlerp = 1;
					mo->old_x = mo->x;
					mo->old_y = mo->y;
					mo->old_z = mo->z;
				}
				mo->new_x = mo->x;
				mo->new_y = mo->y;
				mo->new_z = mo->z;

			}
			else if (i == THINK_PRECIP)
			{
				if (ISA(P_RainThinker) || ISA(P_SnowThinker))
				{
					CAST(mo, precipmobj_t);
					if (!mo->firstlerp)
					{
						mo->firstlerp = 1;
						mo->old_x = mo->x;
						mo->old_y = mo->y;
						mo->old_z = mo->z;
					}
					mo->new_x = mo->x;
					mo->new_y = mo->y;
					mo->new_z = mo->z;
				}
			}
			/*else if (i == THINK_POLYOBJ)
			{
				if (ISA(T_PolyObjRotate))
				{
					CAST(p, polyrotate_t);
					polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
					if (pobj == NULL) continue;
					SetPolyobjNewState(pobj);
				}
				else if (ISA(T_PolyObjMove)
					|| ISA(T_PolyObjFlag))
				{
					CAST(p, polymove_t);
					polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
					if (pobj == NULL) continue;
					SetPolyobjNewState(pobj);
				}
				else if (ISA(T_PolyObjWaypoint))
				{
					CAST(p, polywaypoint_t);
					polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
					if (pobj == NULL) continue;
					SetPolyobjNewState(pobj);
				}
				else if (ISA(T_PolyDoorSlide))
				{
					CAST(p, polyslidedoor_t);
					polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
					if (pobj == NULL) continue;
					SetPolyobjNewState(pobj);
				}
				else if (ISA(T_PolyDoorSwing))
				{
					CAST(p, polyswingdoor_t);
					polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
					if (pobj == NULL) continue;
					SetPolyobjNewState(pobj);
				}
				else if (ISA(T_PolyObjDisplace))
				{
					CAST(p, polydisplace_t);
					if (!p->firstlerp)
					{
						p->firstlerp = 1;
						p->old_dx = p->old_dx;
						p->old_dy = p->old_dy;
					}
					p->new_dx = p->dx;
					p->new_dy = p->dy;
					polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
					if (pobj == NULL) continue;
					pobj->firstlerp = p->firstlerp;
					SetPolyobjNewState(pobj);
				}
				else if (ISA(T_PolyObjRotDisplace))
				{
					CAST(p, polyrotdisplace_t);
					polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
					if (pobj == NULL) continue;
					SetPolyobjNewState(pobj);
				}
			}*/
			// Other thinkers
			//else
			{
				if (ISA(T_MoveCeiling) || ISA(T_CrushCeiling))
				{
					CAST(s, ceiling_t);
					if (!s->firstlerp)
					{
						s->firstlerp = 1;
						s->old_ceilingheight = s->sector->ceilingheight;
					}
					s->new_ceilingheight = s->sector->ceilingheight;
				}
				else if (ISA(T_MoveFloor))
				{
					CAST(s, floormove_t);
					if (!s->firstlerp)
					{
						s->firstlerp = 1;
						s->old_floorheight = s->sector->floorheight;
					}
					s->new_floorheight = s->sector->floorheight;
				}
				else if (ISA(T_LightningFlash))
				{
					CAST(l, lightflash_t);
					if (!l->firstlerp)
					{
						l->firstlerp = 1;
						l->old_lightlevel = l->sector->lightlevel;
					}
					l->new_lightlevel = l->sector->lightlevel;
				}
				else if (ISA(T_StrobeFlash))
				{
					CAST(s, strobe_t);
					if (!s->firstlerp)
					{
						s->firstlerp = 1;
						s->old_lightlevel = s->sector->lightlevel;
					}
					s->new_lightlevel = s->sector->lightlevel;
				}
				else if (ISA(T_Glow))
				{
					CAST(g, glow_t);
					if (!g->firstlerp)
					{
						g->firstlerp = 1;
						g->old_lightlevel = g->sector->lightlevel;
					}
					g->new_lightlevel = g->sector->lightlevel;
				}
				else if (ISA(T_FireFlicker))
				{
					CAST(f, fireflicker_t);
					if (!f->firstlerp)
					{
						f->firstlerp = 1;
						f->old_lightlevel = f->sector->lightlevel;
					}
					f->new_lightlevel = f->sector->lightlevel;
				}
				else if (ISA(T_MoveElevator) || ISA(T_CameraScanner))
				{
					CAST(e, elevator_t);
					if (!e->firstlerp)
					{
						e->firstlerp = 1;
						e->old_floorheight = e->sector->floorheight;
						e->old_ceilingheight = e->sector->ceilingheight;
					}
					e->new_floorheight = e->sector->floorheight;
					e->new_ceilingheight = e->sector->ceilingheight;
				}
				else if (ISA(T_StartCrumble))
				{
					CAST(c, crumble_t);
					if (!c->firstlerp)
					{
						c->firstlerp = 1;
						c->old_floorheight = c->sector->floorheight;
						c->old_ceilingheight = c->sector->ceilingheight;
					}
					c->new_floorheight = c->sector->floorheight;
					c->new_ceilingheight = c->sector->ceilingheight;
				}
				else if (ISA(T_ContinuousFalling))
				{
					CAST(f, continuousfall_t);
					if (!f->firstlerp)
					{
						f->firstlerp = 1;
						f->old_floorheight = f->sector->floorheight;
						f->old_ceilingheight = f->sector->ceilingheight;
					}
					f->new_floorheight = f->sector->floorheight;
					f->new_ceilingheight = f->sector->ceilingheight;
				}
				else if (ISA(T_ThwompSector))
				{
					CAST(f, thwomp_t);
					if (!f->firstlerp)
					{
						f->firstlerp = 1;
						f->old_floorheight = f->sector->floorheight;
						f->old_ceilingheight = f->sector->ceilingheight;
					}
					f->new_floorheight = f->sector->floorheight;
					f->new_ceilingheight = f->sector->ceilingheight;
				}
				else if (ISA(T_RaiseSector))
				{
					CAST(f, raise_t);
					if (!f->firstlerp)
					{
						f->firstlerp = 1;
						f->old_floorheight = f->sector->floorheight;
						f->old_ceilingheight = f->sector->ceilingheight;
					}
					f->new_floorheight = f->sector->floorheight;
					f->new_ceilingheight = f->sector->ceilingheight;
				}
				else if (ISA(T_BounceCheese))
				{
					CAST(f, bouncecheese_t);
					if (!f->firstlerp)
					{
						f->firstlerp = 1;
						f->old_floorheight = f->sector->floorheight;
						f->old_ceilingheight = f->sector->ceilingheight;
					}
					f->new_floorheight = f->sector->floorheight;
					f->new_ceilingheight = f->sector->ceilingheight;
				}
				else if (ISA(T_MarioBlock))
				{
					CAST(f, mariothink_t);
					if (!f->firstlerp)
					{
						f->firstlerp = 1;
						f->old_floorheight = f->sector->floorheight;
						f->old_ceilingheight = f->sector->ceilingheight;
					}
					f->new_floorheight = f->sector->floorheight;
					f->new_ceilingheight = f->sector->ceilingheight;
				}
				else if (ISA(T_FloatSector))
				{
					CAST(f, floatthink_t);
					if (!f->firstlerp)
					{
						f->firstlerp = 1;
						f->old_floorheight = f->sector->floorheight;
						f->old_ceilingheight = f->sector->ceilingheight;
					}
					f->new_floorheight = f->sector->floorheight;
					f->new_ceilingheight = f->sector->ceilingheight;
				}
				/*if (ISA(T_LaserFlash))
				{
					//CAST(l, laserthink_t);
				}*/
				else if (ISA(T_LightFade))
				{
					CAST(l, lightlevel_t);
					if (!l->firstlerp)
					{
						l->firstlerp = 1;
						l->old_lightlevel = l->sector->lightlevel;
					}
					l->new_lightlevel = l->sector->lightlevel;
				}
				/*if (ISA(T_ExecutorDelay))
				{
					//CAST(e, executor_t);
				}*/
				/*if (ISA(T_Disappear))
				{
					//CAST(d, disappear_t);
				}*/
				else if (ISA(T_Scroll))
				{
					CAST(s, scroll_t);
					switch (s->type)
					{
						case sc_side:
						{
							side_t *side;
							side = sides + s->affectee;
							if (!s->firstlerp)
							{
								s->firstlerp = 1;
								s->old_textureoffset = side->textureoffset;
								s->old_rowoffset = side->rowoffset;
							}
							s->new_textureoffset = side->textureoffset;
							s->new_rowoffset = side->rowoffset;
							break;
						}
						case sc_floor:
						{
							sector_t *sec;
							sec = sectors + s->affectee;
							if (!s->firstlerp)
							{
								s->firstlerp = 1;
								s->old_xoffs = sec->floor_xoffs;
								s->old_yoffs = sec->floor_yoffs;
							}
							s->new_xoffs = sec->floor_xoffs;
							s->new_yoffs = sec->floor_yoffs;
							break;
						}
						case sc_ceiling:
						{
							sector_t *sec;
							sec = sectors + s->affectee;
							if (!s->firstlerp)
							{
								s->firstlerp = 1;
								s->old_xoffs = sec->ceiling_xoffs;
								s->old_yoffs = sec->ceiling_yoffs;
							}
							s->new_xoffs = sec->ceiling_xoffs;
							s->new_yoffs = sec->ceiling_yoffs;
							break;
						}
						case sc_carry:
						case sc_carry_ceiling:
							break;
					}
				}
				/*if (ISA(T_Friction))
				{
					//CAST(f, friction_t);
				}*/
				/*if (ISA(T_Pusher))
				{
					//CAST(f, pusher_t);
				}*/
			}
		}
	}
}

void R_DoThinkerLerp(fixed_t frac)
{
	thinker_t *th;
	thinklistnum_t i;

	for (i = 0; i <= NUM_THINKERLISTS-1; i++)
	{
		for (th = thlist[i].next; th != &thlist[i]; th = th->next)
		{
			if (!th)
				break;

			if (i == THINK_MOBJ)
			{
				CAST(mo, mobj_t);
				if (!mo->firstlerp) continue;
				if (mo->interpmode == MI_NOINTERP2) continue;
				mo->x = mo->old_x + R_LerpFixed(mo->old_x, mo->new_x, frac);
				mo->y = mo->old_y + R_LerpFixed(mo->old_y, mo->new_y, frac);
				mo->z = mo->old_z + R_LerpFixed(mo->old_z, mo->new_z, frac);	
			}
			else if (i == THINK_PRECIP)
			{
				if (ISA(P_RainThinker) || ISA(P_SnowThinker))
				{
					CAST(mo, precipmobj_t);
					if (!mo->firstlerp) continue;
					mo->x = R_LerpFixed(mo->old_x, mo->new_x, frac);
					mo->y = R_LerpFixed(mo->old_y, mo->new_y, frac);
					mo->z = R_LerpFixed(mo->old_z, mo->new_z, frac);
				}
			}
			if (cv_interpmovingplatforms.value)
			{
				/*if (i == THINK_POLYOBJ)
				{
					if (ISA(T_PolyObjRotate))
					{
						CAST(p, polyrotate_t);
						polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
						if (pobj == NULL) continue;
						LerpPolyobjState(pobj, frac);
					}
					else if (ISA(T_PolyObjMove)
						|| ISA(T_PolyObjFlag))
					{
						CAST(p, polymove_t);
						polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
						if (pobj == NULL) continue;
						LerpPolyobjState(pobj, frac);
					}
					else if (ISA(T_PolyObjWaypoint))
					{
						CAST(p, polywaypoint_t);
						polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
						if (pobj == NULL) continue;
						LerpPolyobjState(pobj, frac);
					}
					else if (ISA(T_PolyDoorSlide))
					{
						CAST(p, polyslidedoor_t);
						polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
						if (pobj == NULL) continue;
						LerpPolyobjState(pobj, frac);
					}
					else if (ISA(T_PolyDoorSwing))
					{
						CAST(p, polyswingdoor_t);
						polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
						if (pobj == NULL) continue;
						LerpPolyobjState(pobj, frac);
					}
					else if (ISA(T_PolyObjDisplace))
					{
						CAST(p, polydisplace_t);
						if (!p->firstlerp) continue;
						p->dx = R_LerpFixed(p->old_dx, p->new_dx, frac);
						p->dy = R_LerpFixed(p->old_dy, p->new_dy, frac);
						polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
						if (pobj == NULL) continue;
						LerpPolyobjState(pobj, frac);
					}
					else if (ISA(T_PolyObjRotDisplace))
					{
						CAST(p, polyrotdisplace_t);
						polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
						if (pobj == NULL) continue;
						LerpPolyobjState(pobj, frac);
					}
				}*/
				// Other thinkers
				//else
				{
					if (ISA(T_MoveCeiling) || ISA(T_CrushCeiling))
					{
						CAST(s, ceiling_t);
						if (!s->firstlerp) continue;
						s->sector->ceilingheight = s->old_ceilingheight + R_LerpFixed(s->old_ceilingheight, s->new_ceilingheight, frac);
					}
					else if (ISA(T_MoveFloor))
					{
						CAST(s, floormove_t);
						if (!s->firstlerp) continue;
						s->sector->floorheight = s->old_floorheight + R_LerpFixed(s->old_floorheight, s->new_floorheight, frac);
					}
					else if (ISA(T_LightningFlash))
					{
						CAST(l, lightflash_t);
						if (!l->firstlerp) continue;
						l->sector->lightlevel = l->old_lightlevel + (INT16) R_LerpInt32(l->old_lightlevel, l->new_lightlevel, frac);
					}
					else if (ISA(T_StrobeFlash))
					{
						CAST(s, strobe_t);
						if (!s->firstlerp) continue;
						s->sector->lightlevel = s->old_lightlevel + (INT16) R_LerpInt32(s->old_lightlevel, s->new_lightlevel, frac);
					}
					else if (ISA(T_Glow))
					{
						CAST(g, glow_t);
						if (!g->firstlerp) continue;
						g->sector->lightlevel = g->old_lightlevel + (INT16) R_LerpInt32(g->old_lightlevel, g->new_lightlevel, frac);
					}
					else if (ISA(T_FireFlicker))
					{
						CAST(f, fireflicker_t);
						if (!f->firstlerp) continue;
						f->sector->lightlevel = f->old_lightlevel + (INT16) R_LerpInt32(f->old_lightlevel, f->new_lightlevel, frac);
					}
					else if (ISA(T_MoveElevator) || ISA(T_CameraScanner))
					{
						CAST(e, elevator_t);
						if (!e->firstlerp) continue;
						e->sector->ceilingheight = e->old_ceilingheight + R_LerpFixed(e->old_ceilingheight, e->new_ceilingheight, frac);
						e->sector->floorheight = e->old_floorheight + R_LerpFixed(e->old_floorheight, e->new_floorheight, frac);

					}
					else if (ISA(T_StartCrumble))
					{
						CAST(c, crumble_t);
						if (!c->firstlerp) continue;
						c->sector->ceilingheight = c->old_ceilingheight + R_LerpFixed(c->old_ceilingheight, c->new_ceilingheight, frac);
						c->sector->floorheight = c->old_floorheight + R_LerpFixed(c->old_floorheight, c->new_floorheight, frac);
					}
					else if (ISA(T_ContinuousFalling))
					{
						CAST(f, continuousfall_t);
						if (!f->firstlerp) continue;
						f->sector->ceilingheight = f->old_ceilingheight + R_LerpFixed(f->old_ceilingheight, f->new_ceilingheight, frac);
						f->sector->floorheight = f->old_floorheight + R_LerpFixed(f->old_floorheight, f->new_floorheight, frac);
					}
					else if (ISA(T_ThwompSector))
					{
						CAST(f, thwomp_t);
						if (!f->firstlerp) continue;
						f->sector->ceilingheight = f->old_ceilingheight + R_LerpFixed(f->old_ceilingheight, f->new_ceilingheight, frac);
						f->sector->floorheight = f->old_floorheight + R_LerpFixed(f->old_floorheight, f->new_floorheight, frac);
					}
					else if (ISA(T_RaiseSector))
					{
						CAST(f, raise_t);
						if (!f->firstlerp) continue;
						f->sector->ceilingheight = f->old_ceilingheight + R_LerpFixed(f->old_ceilingheight, f->new_ceilingheight, frac);
						f->sector->floorheight = f->old_floorheight + R_LerpFixed(f->old_floorheight, f->new_floorheight, frac);
					}
					else if (ISA(T_BounceCheese))
					{
						CAST(f, bouncecheese_t);
						if (!f->firstlerp) continue;
						f->sector->ceilingheight = f->old_ceilingheight + R_LerpFixed(f->old_ceilingheight, f->new_ceilingheight, frac);
						f->sector->floorheight = f->old_floorheight + R_LerpFixed(f->old_floorheight, f->new_floorheight, frac);
					}
					else if (ISA(T_MarioBlock))
					{
						CAST(f, mariothink_t);
						if (!f->firstlerp) continue;
						f->sector->ceilingheight = f->old_ceilingheight + R_LerpFixed(f->old_ceilingheight, f->new_ceilingheight, frac);
						f->sector->floorheight = f->old_floorheight + R_LerpFixed(f->old_floorheight, f->new_floorheight, frac);
					}
					else if (ISA(T_FloatSector))
					{
						CAST(f, floatthink_t);
						if (!f->firstlerp) continue;
						f->sector->ceilingheight = f->old_ceilingheight + R_LerpFixed(f->old_ceilingheight, f->new_ceilingheight, frac);
						f->sector->floorheight = f->old_floorheight + R_LerpFixed(f->old_floorheight, f->new_floorheight, frac);
					}
					/*if (ISA(T_LaserFlash))
					{
						//CAST(l, laserthink_t);
					}*/
					else if (ISA(T_LightFade))
					{
						CAST(l, lightlevel_t);
						if (!l->firstlerp) continue;
						l->sector->lightlevel = l->old_lightlevel + (INT16) R_LerpInt32(l->old_lightlevel, l->new_lightlevel, frac);
					}
					/*if (ISA(T_ExecutorDelay))
					{
						//CAST(e, executor_t);
					}*/
					/*if (ISA(T_Disappear))
					{
						//CAST(d, disappear_t);
					}*/
					else if (ISA(T_Scroll))
					{
						CAST(s, scroll_t);
						switch (s->type)
						{
							case sc_side:
							{
								side_t *side;
								side = sides + s->affectee;
								if (!s->firstlerp) break;
								side->textureoffset = s->old_textureoffset + R_LerpFixed(s->old_textureoffset, s->new_textureoffset, frac);
								side->rowoffset = s->old_rowoffset + R_LerpFixed(s->old_rowoffset, s->new_rowoffset, frac);
								break;
							}
							case sc_floor:
							{
								sector_t *sec;
								sec = sectors + s->affectee;
								if (!s->firstlerp) break;
								sec->floor_xoffs = s->old_xoffs + R_LerpFixed(s->old_xoffs, s->new_xoffs, frac);
								sec->floor_yoffs = s->old_yoffs + R_LerpFixed(s->old_yoffs, s->new_yoffs, frac);
								break;
							}
							case sc_ceiling:
							{
								sector_t *sec;
								sec = sectors + s->affectee;
								if (!s->firstlerp) break;
								sec->ceiling_xoffs = s->old_xoffs + R_LerpFixed(s->old_xoffs, s->new_xoffs, frac);
								sec->ceiling_yoffs = s->old_yoffs + R_LerpFixed(s->old_yoffs, s->new_yoffs, frac);
								break;
							}
							case sc_carry:
							case sc_carry_ceiling:
								break;
						}
					}
					/*if (ISA(T_Friction))
					{
						//CAST(f, friction_t);
					}*/
					/*if (ISA(T_Pusher))
					{
						//CAST(f, pusher_t);
					}*/
				}
			}
		}
	}
}

void R_ResetThinkerLerp(void)
{
	thinker_t *th;
	thinklistnum_t i;

	for (i = 0; i <= NUM_THINKERLISTS-1; i++)
	{
		for (th = thlist[i].next; th != &thlist[i]; th = th->next)
		{
			if (!th)
				break;

			if (i == THINK_MOBJ)
			{
				CAST(mo, mobj_t);
				if (!mo->firstlerp) continue;
				mo->x = mo->new_x;
				mo->y = mo->new_y;
				mo->z = mo->new_z;
			}
			else if (i == THINK_PRECIP)
			{
				
				if (ISA(P_RainThinker) || ISA(P_SnowThinker))
				{
					CAST(mo, precipmobj_t);
					if (!mo->firstlerp) continue;
					mo->x = mo->new_x;
					mo->y = mo->new_y;
					mo->z = mo->new_z;
				}
			}
			/*else if (i == THINK_POLYOBJ)
			{
				if (ISA(T_PolyObjRotate))
				{
					CAST(p, polyrotate_t);
					polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
					if (pobj == NULL) continue;
					ResetPolyobjState(pobj);
				}
				else if (ISA(T_PolyObjMove)
					|| ISA(T_PolyObjFlag))
				{
					CAST(p, polymove_t);
					polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
					if (pobj == NULL) continue;
					ResetPolyobjState(pobj);
				}
				else if (ISA(T_PolyObjWaypoint))
				{
					CAST(p, polywaypoint_t);
					polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
					if (pobj == NULL) continue;
					ResetPolyobjState(pobj);
				}
				else if (ISA(T_PolyDoorSlide))
				{
					CAST(p, polyslidedoor_t);
					polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
					if (pobj == NULL) continue;
					ResetPolyobjState(pobj);
				}
				else if (ISA(T_PolyDoorSwing))
				{
					CAST(p, polyswingdoor_t);
					polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
					if (pobj == NULL) continue;
					ResetPolyobjState(pobj);
				}
				else if (ISA(T_PolyObjDisplace))
				{
					CAST(p, polydisplace_t);
					if (!p->firstlerp) continue;
					p->dx = p->new_dx;
					p->dy = p->new_dy;
					polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
					if (pobj == NULL) continue;
					ResetPolyobjState(pobj);
				}
				else if (ISA(T_PolyObjRotDisplace))
				{
					CAST(p, polyrotdisplace_t);
					polyobj_t *pobj = Polyobj_GetForNum(p->polyObjNum);
					if (pobj == NULL) continue;
					ResetPolyobjState(pobj);
				}
			}*/
			// Other thinkers
			//else
			{
				if (ISA(T_MoveCeiling) || ISA(T_CrushCeiling))
				{
					CAST(s, ceiling_t);
					if (!s->firstlerp) continue;
					s->sector->ceilingheight = s->new_ceilingheight;
				}
				else if (ISA(T_MoveFloor))
				{
					CAST(s, floormove_t);
					if (!s->firstlerp) continue;
					s->sector->floorheight = s->new_floorheight;
				}
				else if (ISA(T_LightningFlash))
				{
					CAST(l, lightflash_t);
					if (!l->firstlerp) continue;
					l->sector->lightlevel = l->new_lightlevel;
				}
				else if (ISA(T_StrobeFlash))
				{
					CAST(s, strobe_t);
					if (!s->firstlerp) continue;
					s->sector->lightlevel = s->new_lightlevel;
				}
				else if (ISA(T_Glow))
				{
					CAST(g, glow_t);
					if (!g->firstlerp) continue;
					g->sector->lightlevel = g->new_lightlevel;
				}
				else if (ISA(T_FireFlicker))
				{
					CAST(f, fireflicker_t);
					if (!f->firstlerp) continue;
					f->sector->lightlevel = f->new_lightlevel;
				}
				else if (ISA(T_MoveElevator) || ISA(T_CameraScanner))
				{
					CAST(e, elevator_t);
					if (!e->firstlerp) continue;
					e->sector->ceilingheight = e->new_ceilingheight;
					e->sector->floorheight = e->new_floorheight;
				}
				else if (ISA(T_StartCrumble))
				{
					CAST(c, crumble_t);
					if (!c->firstlerp) continue;
					c->sector->ceilingheight = c->new_ceilingheight;
					c->sector->floorheight = c->new_floorheight;
				}
				else if (ISA(T_ContinuousFalling))
				{
					CAST(f, continuousfall_t);
					if (!f->firstlerp) continue;
					f->sector->ceilingheight = f->new_ceilingheight;
					f->sector->floorheight = f->new_floorheight;
				}
				else if (ISA(T_ThwompSector))
				{
					CAST(f, thwomp_t);
					if (!f->firstlerp) continue;
					f->sector->ceilingheight = f->new_ceilingheight;
					f->sector->floorheight = f->new_floorheight;
				}
				else if (ISA(T_RaiseSector))
				{
					CAST(f, raise_t);
					if (!f->firstlerp) continue;
					f->sector->ceilingheight = f->new_ceilingheight;
					f->sector->floorheight = f->new_floorheight;
				}
				else if (ISA(T_BounceCheese))
				{
					CAST(f, bouncecheese_t);
					if (!f->firstlerp) continue;
					f->sector->ceilingheight = f->new_ceilingheight;
					f->sector->floorheight = f->new_floorheight;
				}
				else if (ISA(T_MarioBlock))
				{
					CAST(f, mariothink_t);
					if (!f->firstlerp) continue;
					f->sector->ceilingheight = f->new_ceilingheight;
					f->sector->floorheight = f->new_floorheight;
				}
				else if (ISA(T_FloatSector))
				{
					CAST(f, floatthink_t);
					if (!f->firstlerp) continue;
					f->sector->ceilingheight = f->new_ceilingheight;
					f->sector->floorheight = f->new_floorheight;
				}
				/*if (ISA(T_LaserFlash))
				{
					//CAST(l, laserthink_t);
				}*/
				else if (ISA(T_LightFade))
				{
					CAST(l, lightlevel_t);
					if (!l->firstlerp) continue;
					l->sector->lightlevel = l->new_lightlevel;
				}
				/*if (ISA(T_ExecutorDelay))
				{
					//CAST(e, executor_t);
				}*/
				/*if (ISA(T_Disappear))
				{
					//CAST(d, disappear_t);
				}*/
				else if (ISA(T_Scroll))
				{
					CAST(s, scroll_t);
					switch (s->type)
					{
						case sc_side:
						{
							side_t *side;
							side = sides + s->affectee;
							if (!s->firstlerp) break;
							side->textureoffset = s->new_textureoffset;
							side->rowoffset = s->new_rowoffset;
							break;
						}
						case sc_floor:
						{
							sector_t *sec;
							sec = sectors + s->affectee;
							if (!s->firstlerp) break;
							sec->floor_xoffs = s->new_xoffs;
							sec->floor_yoffs = s->new_yoffs;
							break;
						}
						case sc_ceiling:
						{
							sector_t *sec;
							sec = sectors + s->affectee;
							if (!s->firstlerp) break;
							sec->ceiling_xoffs = s->new_xoffs;
							sec->ceiling_yoffs = s->new_yoffs;
							break;
						}
						case sc_carry:
						case sc_carry_ceiling:
							break;
					}
				}
				/*if (ISA(T_Friction))
				{
					//CAST(f, friction_t);
				}*/
				/*if (ISA(T_Pusher))
				{
					//CAST(f, pusher_t);
				}*/
			}
		}
	}
}


void R_StashThinkerLerp(void)
{
	thinker_t *th;

	for (th = thlist[THINK_MOBJ].next; th != &thlist[THINK_MOBJ]; th = th->next)
	{
		if (!th)
			break;
		if (ISA(P_MobjThinker))
		{
			CAST(mo, mobj_t);
			if (!mo->firstlerp)
				continue;
			mo->lerp_x = mo->x;
			mo->lerp_y = mo->y;
			mo->lerp_z = mo->z;
			mo->x = mo->new_x;
			mo->y = mo->new_y;
			mo->z = mo->new_z;
		}
	}
}

void R_RestoreThinkerLerp(void)
{
	thinker_t *th;

	for (th = thlist[THINK_MOBJ].next; th != &thlist[THINK_MOBJ]; th = th->next)
	{
		if (!th)
			break;
		if (ISA(P_MobjThinker))
		{
			CAST(mo, mobj_t);
			if (!mo->firstlerp)
				continue;
			mo->x = mo->lerp_x;
			mo->y = mo->lerp_y;
			mo->z = mo->lerp_z;
		}
	}
}

void R_DoLuaThinkerLerp(void)
{
	thinker_t *th;

	if (!cv_frameinterpolation.value || !cv_interpluaobjects.value)
		return;

	for (th = thlist[THINK_MOBJ].next; th != &thlist[THINK_MOBJ]; th = th->next)
	{
		if (!th)
			break;
		CAST(mo, mobj_t);
		if (mo->lua_shouldinterp && !mo->target && !mo->player && viewplayer->mo)
		{
			mo->firstlerp = 1;
			mo->old_x += FixedMul(viewplayer->mo->new_x - viewplayer->mo->old_x, rendertimefrac - FRACUNIT);
			mo->old_y += FixedMul(viewplayer->mo->new_y - viewplayer->mo->old_y, rendertimefrac - FRACUNIT);
			mo->old_z += FixedMul(viewplayer->mo->new_z - viewplayer->mo->old_z, rendertimefrac - FRACUNIT);
		}
	}
}

fixed_t R_LerpFixed(fixed_t from, fixed_t to, fixed_t frac)
{
	return FixedMul(frac, to - from);
}

INT32 R_LerpInt32(INT32 from, INT32 to, fixed_t frac)
{
	return FixedInt(FixedMul(frac, (to*FRACUNIT) - (from*FRACUNIT)));
}

angle_t R_LerpAngle(angle_t from, angle_t to, fixed_t frac)
{
	return FixedMul(frac, to - from);
}
