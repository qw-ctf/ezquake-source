/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/
// cl_tent.h -- client side temporary entities, antilag 1 fake projectiles

#ifndef __CL_TENT_H__
#define __CL_TENT_H__

// Temp entity effects without a network message; each runs the same
// scripted effect, particles, light and sound the message would.

// Position-only types and TE_LIGHTNINGBLOOD; false for an unknown type.
qbool CL_TempEntityEffect(int type, vec3_t pos);
// TE_SPIKE, TE_SUPERSPIKE, TEDP_SPIKEQUAD and TEDP_SUPERSPIKEQUAD without the ricochet sound; false for another type
qbool CL_TE_SpikeImpact(int type, vec3_t pos);
// TE_GUNSHOT
void CL_TE_Gunshot(vec3_t pos, int count);
// TE_BLOOD
void CL_TE_Blood(vec3_t pos, int count);
// TE_EXPLOSION2 tinted with a palette range
void CL_TE_Explosion2(vec3_t pos, int color_start, int color_length);
// TEDP_BLOOD, TEDP_SPARK, TEDP_FLAMEJET, TEDP_SMOKE, TEDP_TEI_PLASMAHIT; false when nothing was drawn
qbool CL_TE_DirectedEffect(int type, vec3_t pos, vec3_t vel, int count);
// TEDP_BLOODSHOWER, TEDP_PARTICLECUBE, TEDP_PARTICLERAIN, TEDP_PARTICLESNOW; false when nothing was drawn
qbool CL_TE_ParticleBox(int type, vec3_t mins, vec3_t maxs, vec3_t dir, int count, int color, qbool gravity);
// TEDP_EXPLOSIONRGB
void CL_TE_ExplosionRGB(vec3_t pos, const byte *color);
// TEDP_CUSTOMFLASH; nothing for a lifetime that is not positive
void CL_TE_CustomFlash(vec3_t pos, float radius, float lifetime, const byte *color);
// TEQW_RAILTRAIL
void CL_TE_RailTrail(vec3_t start, vec3_t end);
// TEDP_TEI_G3
void CL_TE_TeiG3(vec3_t start, vec3_t end);
// TE_LIGHTNING1-3 and TEQW_BEAM as beam types 1-4
void CL_CreateBeam(int type, int ent, vec3_t start, vec3_t end);

struct model_s;
// The model at org, animated from startframe for framecount frames (0 = through
// the model's last frame) at framerate frames per second, then removed. Sprites
// stay opaque, other models fade out over the effect's life. Nothing for a NULL
// model, a rate that is not positive, a negative frame count or a start frame
// outside the model.
void CL_ModelEffect(vec3_t org, struct model_s *model, int startframe, int framecount, float framerate);

// Particle effects the server numbered with svc_fte_precache, 1 to 1023.
// An out-of-range index or an empty name is ignored.
void CL_SetServerParticleEffect(int index, const char *name);
// The number of the effect with exactly this name, 0 when there is none.
int CL_FindServerParticleEffect(const char *name);
// NULL when the number is not set.
const char *CL_ServerParticleEffectName(int index);
// The scripted effect type, P_INVALID when the number is not set or the
// loaded particle sets do not have the effect.
int CL_ServerParticleEffectType(int index);

// client-side "fake" projectiles: spawned locally by weapon prediction
// and/or driven by server-sent simple projectile states
#if defined(MVD_PEXT1_WEAPONPREDICTION) || defined(MVD_PEXT1_SIMPLEPROJECTILE)
#define EZQ_FAKEPROJ
#endif

#ifdef EZQ_FAKEPROJ

#define MAX_FAKEPROJ 96

typedef struct fproj_s {
	int modelindex, dl_key;
	int type, effects, flags, partcount, index;
	float starttime, endtime, parttime;
	vec3_t start, vel, avel, angs, org, partorg;
	centity_t cent;
#ifdef MVD_PEXT1_SIMPLEPROJECTILE
	int entnum;
	int owner;
#endif
} fproj_t;

extern fproj_t cl_fakeprojectiles[MAX_FAKEPROJ];

void Fproj_Physics_Bounce(fproj_t *proj, float dt);
void CL_MatchFakeProjectile(centity_t *cent);
fproj_t *CL_AllocFakeProjectile(void);

#ifdef MVD_PEXT1_WEAPONPREDICTION
fproj_t *CL_CreateFakeNail(void);
fproj_t *CL_CreateFakeSuperNail(void);
fproj_t *CL_CreateFakeGrenade(void);
fproj_t *CL_CreateFakeRocket(void);
#endif

#ifdef MVD_PEXT1_SIMPLEPROJECTILE
// per-edict mirror of the server's simple projectile state
typedef struct cs_sprojectile_s {
	int active;

	int fproj_number;
	int owner;
	vec3_t origin;
	vec3_t velocity;
	vec3_t angles;
	int modelindex;
	float time_offset;
} cs_sprojectile_t;
#endif

#endif // EZQ_FAKEPROJ

#endif // __CL_TENT_H__
