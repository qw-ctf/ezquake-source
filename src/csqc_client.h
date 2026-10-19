/*
csqc_client.h -- client CSQC VM (csprogs.dat on a PR1VM instance)

Entry points for client code and accessors used by the builtins in
csqc_builtins.c. Relies on the standard engine types (qbool/vec3_t); for
client translation units only.
*/

#ifndef CSQC_CLIENT_H
#define CSQC_CLIENT_H

#include <stddef.h>
struct usercmd_s; // usercmd_t, forward-declared to keep this header dependency-free
struct pr1vm_s;  // PR1 instance (pr1vm.h), used only as an opaque pointer

// Extended CSQC stats 32..255 live in a separate client store (the wire stat
// index is a byte, so 256 slots in all). Standard stats 0..31 stay in cl.stats[].
#define MAX_EXTENDED_CL_STATS 256

float CSQC_Client_GetStat(int idx);    // 0..31 -> cl.stats, 32..255 -> ext stats (int)
void CSQC_Client_SetStat(int idx, int value);  // int stat: 0..31 -> getstatf float cache, 32..255 -> ext stats (CL_SetStat)
// Float/string stats 32..255: received via svc_fte_updatestatfloat/string (79/78)
// and read by #331 getstatf / #332 getstats. They live in a single client store,
// not per player. GetStatInt returns the exact int value for #331 bit access: the
// float path would lose the low bits of large ints.
int CSQC_Client_GetStatInt(int idx);   // 0..31 -> cl.stats, 32..255 -> ext (int)
float CSQC_Client_GetStatFloat(int idx);   // 0..31 -> cl.stats, 32..255 -> statsf
const char *CSQC_Client_GetStatString(int idx); // 32..255 -> statss, otherwise ""
void CSQC_Client_SetStatFloat(int idx, float value);  // svc_fte_updatestatfloat (79)
void CSQC_Client_SetStatString(int idx, const char *s); // svc_fte_updatestatstring (78)
void CSQC_Client_ResetStatCache(void);    // invalidate the #79 float cache (track switch)

// Single download slot: queue module-initiated requests instead of overwriting the
// in-flight cls.downloadname/tempname before the first svc_download arrives.
qbool CSQC_Client_DownloadInProgress(void);  // any transfer or queued request
int CSQC_Client_RequestDownload(const char *name); // 1 = present, 0 = queued/sent
void CSQC_Client_DownloadFinished(const char *finished); // NULL = force (timeout)
qbool CSQC_Client_ValidateDownload(const char *tmp, const char *final);
void CSQC_Client_QueueDownload(const char *name);
void CSQC_Client_FlushDownloadQueue(void);  // per-frame: start one queued request
qbool CSQC_Client_AllowWireModelName(const char *name); // capped distinct-name table
qbool CSQC_Client_AllowModuleSound(const char *name);  // capped distinct-name table
qbool CSQC_Client_RegisterModuleCvar(const char *name); // validate/cap/track module cvar
void CSQC_Client_ClearModuleCvars(void);
void CSQC_Client_GetScreenSize(int *w, int *h); // vid.width/height (VF_SCREENVSIZE)
void CSQC_Client_DrawText(float x, float y, const char *text, int r, int g, int b, float alpha, float scale);
void CSQC_Client_RegisterCommand(const char *cmd); // registercommand -> console binding
qbool CSQC_Client_ConsoleCommand(const char *line); // #5: run CSQC_ConsoleCommand(line); true = handled
void CSQC_Client_Abort(const char *msg);   // fatal: disconnects the client
// Bounded string reads for the client VM (untrusted csprogs): a positive offset
// beyond numstrings returns NULL; also decodes the client temp-string range.
// Installed as vm->get_string.
char *CSQC_Client_GetString(struct pr1vm_s *vm, int num);

// Entity pool (slot != server number). entnum helpers work on pool slots;
// network numbers are kept in a number->slot map (svc 76/92). Slot 0 = world.
int CSQC_Client_EntAlloc(struct pr1vm_s *vm);   // first free pool slot (own); aborts the module when full
int CSQC_Client_EntNum(struct pr1vm_s *vm, int raw);  // raw (N*edict_size) -> pool slot; OOB -> world(0)
void CSQC_Client_EntFree(struct pr1vm_s *vm, int slot); // remove(): free; engine-owned network slots keep their number mapping
int CSQC_Client_NetAllocSlot(struct pr1vm_s *vm);  // slot without s_own (network receive; vm zeroes the slot)
void CSQC_Client_NetFreeSlot(int slot, int number);  // free slot + numslot
int CSQC_Client_NumToSlot(int number);     // number->slot map / 0
int CSQC_Client_MapNumber(int number, int slot);  // write map entry (returns slot)
int CSQC_Client_EntityEntNum(struct pr1vm_s *vm, int slot); // .entnum of an arena edict (ssqc number / 0)
// Pool/module field traversal and diagnostics.
qbool CSQC_Client_EntUsed(int slot);   // slot in use (network or spawn)
void CSQC_Client_LinkBounds(struct pr1vm_s *vm, int slot); // recompute .absmin/.absmax
int CSQC_Client_EntSpawnBase(void);   // first usable slot (1)
int CSQC_Client_EntUsedCount(void);
int CSQC_Client_FindField(struct pr1vm_s *vm, const char *name); // field offset in float words / -1
// Hot-path offset cache: resolved once when the module loads; builtins read the
// array instead of scanning globaldefs/fielddefs by name every frame.
typedef enum {
	CSQC_TRACEG_FRACTION = 0,
	CSQC_TRACEG_ALLSOLID,
	CSQC_TRACEG_STARTSOLID,
	CSQC_TRACEG_INOPEN,
	CSQC_TRACEG_INWATER,
	CSQC_TRACEG_PLANE_DIST,
	CSQC_TRACEG_ENDPOS,
	CSQC_TRACEG_PLANE_NORMAL,
	CSQC_TRACEG_ENT,
	CSQC_TRACEG_COUNT
} csqc_traceglobal_id_t;

typedef enum {
	CSQC_FLD_PREDRAW = 0,
	CSQC_FLD_MODELINDEX,
	CSQC_FLD_MODEL,
	CSQC_FLD_COLORMAP,
	CSQC_FLD_ORIGIN,
	CSQC_FLD_ANGLES,
	CSQC_FLD_FRAME,
	CSQC_FLD_FRAME2,
	CSQC_FLD_LERPFRAC,
	CSQC_FLD_SKIN,
	CSQC_FLD_EFFECTS,
	CSQC_FLD_ALPHA,
	CSQC_FLD_SCALE,
	CSQC_FLD_RENDERFLAGS,
	CSQC_FLD_SIZE,
	CSQC_FLD_MINS,
	CSQC_FLD_MAXS,
	CSQC_FLD_MODELFLAGS,
	CSQC_FLD_CHAIN,
	CSQC_FLD_SOLID,
	CSQC_FLD_FLAGS,
	CSQC_FLD_OWNER,
	CSQC_FLD_DRAWMASK,
	CSQC_FLD_ABSMIN,
	CSQC_FLD_ABSMAX,
	CSQC_FLD_ENTNUM,
	CSQC_FLD_GROUNDENTITY,
	CSQC_FLD_ENEMY,
	CSQC_FLD_IDEALPITCH,
	CSQC_FLD_PITCH_SPEED,
	CSQC_FLD_COUNT
} csqc_field_id_t;

int CSQC_Client_TraceGlobal(struct pr1vm_s *vm, int id); // global offset (or -1)
int CSQC_Client_FieldOfs(struct pr1vm_s *vm, int id);  // field offset (or -1)
int CSQC_Client_SelfGlobal(void);       // offset of the self global (or -1)
// Publishes player_localentnum every 2D frame before CSQC_UpdateView. Player
// entities are not fabricated.
void CSQC_Client_UpdateLocalEntnum(void);

// #371 deltalisten: registry of per-model entity update callbacks. The module
// callback runs as CSQC_Ent_Update: `self` and `.entnum` are set by the engine,
// PARM0 = isnew. name=="*" matches all models; func<=0 unregisters. The player
// listener receives the authoritative (no-lerp) player_state.
void CSQC_Client_DeltaListen(const char *model, int func, int flags);
// MASK_DELTA: a delta callback returning !=0 means the engine does not draw the
// entity (the module draws it via #301); getters for cl_ents.c.
qbool CSQC_Client_DeltaPlayerOwned(int pnum);
qbool CSQC_Client_DeltaEntityOwned(int number);

// `#351 setlistener` sets the module's audio listener, used by S_Update.
void CSQC_Client_SetListener(const float *origin, const float *forward, const float *right, const float *up);
qbool CSQC_Client_ListenerActive(void);
void CSQC_Client_GetListener(float *origin, float *forward, float *right, float *up);
// `#303 setproperty` (VF_* subset): applied immediately and re-applied in
// V_CalcRefdef. Returns whether the property was recognized.
qbool CSQC_Client_SetViewProperty(int prop, int argc, const float *args);
void CSQC_Client_ApplyViewProps(void);
void CSQC_Client_SnapshotEngineView(void); // engine camera, restored by clearscene
void CSQC_Client_ResetViewProps(void); // #300 clearscene: reset view properties
// Gates the engine sbar/HUD (#303 VF_DRAWENGINESBAR) and crosshair
// (VF_DRAWCROSSHAIR) behind the CSQC takeover; clearscene defaults both to false.
qbool CSQC_Client_DrawEngineSbar(void);
qbool CSQC_Client_DrawCrosshairFlag(void);
qbool CSQC_Client_Project(const float *world, float *sx, float *sy, float *sz);
qbool CSQC_Client_Unproject(float sx, float sy, float sz, float *world);
// `#504 getentity`: interpolated engine entity state by server number
// (cl_entities/lerp_origin + player-state). out[3] is filled (float -> out[0],
// vector -> all three); fields with no ezQuake source return a fixed default.
void CSQC_Client_GetEntity(int entnum, int fldnum, float out[3]);

// CSQC model index space: positive = server precache (cl.model_name/
// cl.model_precache), negative = csqc-only slot, 0 = none. Shared by
// #200/#333/#334 and the .modelindex field. IndexKnown only looks up; Index
// looks up or loads (allocating a csqc-only slot).
int CSQC_Client_ModelIndexKnown(const char *name); // 0 if not found
int CSQC_Client_ModelIndex(const char *name);  // 0 if empty name / registry exhausted (a missing file still yields a stable csqc slot)
struct model_s *CSQC_Client_ModelForIndex(int idx); // NULL if absent
const char *CSQC_Client_ModelNameForIndex(int idx); // #334: reverse lookup (NULL if absent)
void CSQC_Client_ModelReset(void);
// Reload CSQC model(s) whose name matches a downloaded file (downloadname =
// cls.downloadname "<gamedir>/<file>").
void CSQC_Client_ModelDownloadFinished(const char *downloadname);

// Lifecycle.
int CSQC_Client_Active(void);   // module loaded and not in error
// Signon complete, about to send `begin`: resolve the module (load / start the
// csprogs download). false = hold `begin`; CSQC_Client_SignonFrame sends it.
qbool CSQC_Client_PreBegin(void);
void CSQC_Client_SignonFrame(void); // every client frame: module and csprogs downloads
void CSQC_Client_Frame(void);   // every client frame: per-frame globals + deltalisten bridges
void CSQC_Client_ConnectCheck(void); // world entry: time base + fallback module load
void CSQC_Client_Disconnect(void);  // CSQC_Shutdown + unload + command removal
void CSQC_Client_Update(void);   // every 2D frame: WorldLoaded-once + UpdateView

// Renderscene takeover: an active module with CSQC_UpdateView owns the 3D
// scene. CSQC_UpdateView runs in the 3D phase, #300/#301 build cl_visents, and
// #304 renderscene calls R_RenderView(). Otherwise the engine path is used.
qbool CSQC_Client_SceneActive(void); // module active && has CSQC_UpdateView
void CSQC_Client_BeginScene(void);  // reset the "renderscene was called this frame" flag
void CSQC_Client_RenderScene(void); // #304 renderscene -> R_RenderView()
qbool CSQC_Client_SceneRendered(void);
// #301 mask&2: the module requested the engine viewmodel in the CSQC scene.
void CSQC_Client_LinkViewModel(void);
qbool CSQC_Client_SceneViewModel(void);
// A CSQCRF_VIEWMODEL entity; its origin/angles are relative to the view.
void CSQC_Client_AddViewModel(struct entity_s *ent);
void CSQC_Client_ClearViewModels(void);
void CSQC_Client_DrawViewModels(void);
// #301/#302: call .predraw on an arena edict (self=slot). Return = OFS_RETURN
// (PREDRAW_AUTOADD=0 -> add, !=0 -> skip); *removed is set when the edict was
// removed or execution failed inside predraw. The self context is restored.
float CSQC_Client_CallPredraw(int slot, int fidx, qbool *removed);

// CSQC_Input_Frame, run before sending each usercmd: fills the input_* globals
// from cmd, runs the module and writes the changes back to cmd.
void CSQC_Client_InputFrame(struct usercmd_s *cmd);

// #345: history of sent usercmd frames. seq mirrors cls.netchan.outgoing_sequence
// (the client message number at record time); servercommandframe = cl.parsecount
// (incoming_acknowledged) -- the prediction window is (servercommandframe,
// clientcommandframe] in one numbering (QW echoes the ack on netchan/frame).
void CSQC_Client_RecordInput(struct usercmd_s *cmd);
int CSQC_Client_ApplyInput(unsigned int seq);   // fill input_* by seq; 0/1
// FTE VectorAngles (up->roll, meshpitch param): shared helper for #51.
void CSQC_VectorAngles(const float *forward, const float *up, float *result, qbool meshpitch);
void CSQC_Client_RunPlayerPhysics(int entnum);   // #347 runstandardplayerphysics
// #1 makevectors: writes the module's v_forward/v_right/v_up from vector angles.
void CSQC_Client_MakeVectors(float *ang);
// #432 vectorvectors: writes the module's normalized v_forward and orthogonal
// v_right/v_up from a direction.
void CSQC_Client_VectorVectors(float *dir);

// #460-469 string-buffers (DP). handle = 1-based; strings are deep-copied.
int CSQC_Client_BufCreate(void);
void CSQC_Client_BufDel(int handle);
int CSQC_Client_BufGetSize(int handle);
int CSQC_Client_BufAdd(int handle, const char *s, int order);
const char *CSQC_Client_BufGet(int handle, int idx);
int CSQC_Client_BufSet(int handle, int idx, const char *s);
int CSQC_Client_BufFree(int handle, int idx);
int CSQC_Client_BufCopy(int from, int to);
int CSQC_Client_BufSort(int handle, int prefixlen, int backward);
char *CSQC_Client_BufImplode(int handle, const char *glue);
void CSQC_Client_BufReset(void);

// 2D graphics (draw.h/r_draw*; coordinates are raw video pixels, like DrawText).
// Helpers for csqc_builtins.c.
void CSQC_Client_DrawFill(float x, float y, float w, float h, int r, int g, int b, float alpha);
qbool CSQC_Client_DrawPic(float x, float y, float w, float h, const char *name, int r, int g, int b, float alpha); // returns: pic found (#322)
void CSQC_Client_DrawSubPic(float x, float y, float w, float h, const char *name, float srcx, float srcy, float srcw, float srch, int r, int g, int b, float alpha);
void CSQC_Client_DrawCharacter(float x, float y, int ch, int r, int g, int b, float alpha, float scale);
void CSQC_Client_DrawLine(float x1, float y1, float x2, float y2, float width, int r, int g, int b, float alpha);
void CSQC_Client_DrawRawText(float x, float y, const char *text, int r, int g, int b, float alpha, float scale); // no &c parsing (#321)
qbool CSQC_Client_IsCachedPic(const char *name);  // #316 iscachedpic
qbool CSQC_Client_PicSize(const char *name, float *w, float *h); // #318 drawgetimagesize
void CSQC_Client_SetClipArea(float x, float y, float w, float h); // #324
void CSQC_Client_ResetClipArea(void);    // #325
float CSQC_Client_StringWidth(const char *text, qbool usecolours, float fontsize_x);
// The active VM's `drawfontscale` (vector): the x component scales text for the
// draw/measure handlers. No global -> 1.0; guard `x==0 && y==0` (both zero =
// "scale disabled") -> 1.0. The y component is not applied (ezquake fonts are
// uniform).
float CSQC_Client_DrawFontScaleX(struct pr1vm_s *vm);
qbool CSQC_Client_PrecachePic(const char *name);
qbool CSQC_Client_PicSafe(const char *name); // .lmp header/size pre-check

// #343 setcursormode: module cursor state and drawing. While the CSQC cursor is
// active, the mouse grab logic honors CSQC_Client_CSQCCursor() and SCR_DrawCursor
// draws the module's cursor (image/hotspot/scale). #344 reads the pointer
// position.
void CSQC_Client_SetCursorMode(qbool usecursor, const char *image, float hotspot_x, float hotspot_y, float scale);
qbool CSQC_Client_CSQCCursor(void);  // usecursor=1 && module loaded && in game
void CSQC_Client_DrawCursor(void);   // draw the module cursor (SCR_DrawCursor)
void CSQC_Client_GetCursorPos(float *x, float *y); // pointer position (#344)
qbool CSQC_Client_CursorRequested(void); // module's setcursormode bit, regardless of key_dest
void CSQC_Client_AccumMouseDelta(float dx, float dy); // raw render-2D motion (in_sdl2.c)
void CSQC_Client_TakeMouseDelta(float *x, float *y); // motion since the last call, then reset
void CSQC_Client_ScaleCursorDelta(float *x, float *y); // render-2D -> vid.conwidth (mouse delta)
void CSQC_Client_SetSensitivityScale(float scale); // #346 setsensitivityscaler
float CSQC_Client_SensitivityScale(void); // sensitivity multiplier (1 when inactive)

// Input events to the module (CSQC_InputEvent): keys/mouse/wheel.
qbool CSQC_Client_HasInputEvent(void);  // module defines CSQC_InputEvent
int CSQC_Client_InputEvent(int evtype, float a, float b, float c); // returns handled

// notmenu for CSQC_UpdateView (#300) and translation between ezquake's internal
// keynum and QC/DP key codes. Applied to CSQC_InputEvent and the keynum builtins.
qbool CSQC_Client_NotMenu(void);
int CSQC_Client_KeynumToQC(int keynum);
int CSQC_Client_QCToKeynum(int code);

// Event types (csdefs.qc IE_*).
#ifndef IE_KEYDOWN
#define IE_KEYDOWN	0
#define IE_KEYUP	1
#define IE_MOUSEDELTA	2
#define IE_MOUSEABS	3
#define IE_ACCELEROMETER 4
#define IE_FOCUS		5
#define IE_JOYAXIS		6
#endif

// Runtime gate for the CSQC parsers: only the negotiated FTE_PEXT_CSQC bit is
// checked (the cl_pext_csqc cvar is sampled into the declare mask at connect, not
// read live). Without it, 76/92 must not be treated as CSQC.
qbool CSQC_Client_ParseAllowed(void);
// read* gate: true only inside the parse callbacks (CSQC_Ent_Update,
// CSQC_Parse_Event); a read* call anywhere else is fatal.
qbool CSQC_Client_MayRead(void);
// Parse svc_fte_csqcentities (76) / its sized variant (92).
void CSQC_Client_ParseEntities(qbool sized);
// Parse svc_fte_cgamepacket (83) / sized (90): the module reads the name + payload.
// sized: the caller drains any remainder by length. Unsized (83) carries no
// length, so with no live module/callback it is a protocol error.
void CSQC_Client_ParseEvent(qbool sized);

// A model or sound of svc_fte_precache (77), type and index split from the
// FTEPC_* code; other types are ignored.
void CSQC_Client_Precache(int ptype, int pidx, const char *name);
// Parser for an FTE message outside the CSQC extension; it reads from the
// global net_message.
void CSQC_Client_DrainTempEntSizedMsg(void);    // svc_fte_temp_entity_sized (91)
// Size guard shared by sized messages (90/91/92): align msg_readcount to
// payload_start+payload_len (rewind over-read, pad under-read).
void CSQC_Client_SizedRewind(int payload_start, int payload_len);

// Network print callbacks. ParsePrint is true if the module handled it (the
// engine suppresses its own print); ParseCenterPrint is true if the module
// returned != 0 (suppress the engine centerprint).
qbool CSQC_Client_ParsePrint(const char *msg, int level);
qbool CSQC_Client_ParseCenterPrint(const char *msg);

// Network damage callback. true if the module returned != 0 (suppress the engine's
// color-shift / view-kick).
qbool CSQC_Client_ParseDamage(float save, float take, const vec3_t source);

// Network sound callback. true if the module returned != 0 (the engine does not
// play the sound). self = the arena entity by entnum, or world.
qbool CSQC_Client_EventSound(int entnum, int channel, const char *name, float vol,
	float atten, const vec3_t pos, float pitchmod, float flags);

// Network forced-angles callback. true if the module returned != 0 (the engine does
// not apply its own angle).
qbool CSQC_Client_ParseSetAngles(const float *angles, float isdelta);

// Engine callback CSQC_RendererRestarted(string rendererdescription) -- invoked on
// renderer reinitialization (vid_restart/vid_reload) and on module load. The return
// value is ignored.
void CSQC_Client_RendererRestarted(const char *desc);

void CSQCVM_RegisterBuiltins(struct pr1vm_s *vm);

// Module statics (#69 makestatic). Link adds the
// visible ones to the frame's entity list from the world traversal; Clear drops
// them on module unload (map change/disconnect).
void CSQCVM_LinkStatics(void);
void CSQCVM_ClearStatics(void);
void CSQCVM_ClearParticleEffects(void); // #335 effect-name table
// Runs scripted trail type from start to end, continuing the trail of the entity
// in slot; the world and free slots have no trail state. False when nothing ran.
qbool CSQC_Client_EntTrail(int slot, vec3_t start, vec3_t end, int type);

// Engine-internal builtin slots for name-mapped `#0` builtins (the module declares
// `= #0`, resolved by name at load; these numbers never appear in csprogs). Above
// the highest module-visible number (742), below CSQC_MAX_BUILTINS (1024).
#define CSQC_BUILTIN_CHECKBUILTIN	1023
#define CSQC_BUILTIN_ARGC			1021
#define CSQC_BUILTIN_GETPLAYERKEYFLOAT	1020
#define CSQC_BUILTIN_SERVERKEYFLOAT	1019
// Aborting stub for an unknown `#0` name: calling it fails the module;
// checkbuiltin still reports 0.
#define CSQC_BUILTIN_NAMED_UNKNOWN	1022

// Resolve `#0` name-mapped builtins of freshly loaded csprogs: functions with
// first_statement == 0 (i > 0) get a numeric slot by name; unknown names map to
// the aborting stub above. Called by the loader after PR1VM_LoadData.
void CSQCVM_ResolveNamedBuiltins(struct pr1vm_s *vm);

// Client wrapper over PR1VM_SetString: strings in the module string area are
// stored as offsets without a copy; anything else becomes a garbage-collected
// temp string, freed after an outermost call once unreferenced. address is a
// string_t* (int*).
void PR1VM_ClientSetString(struct pr1vm_s *vm, int *address, char *s);

// Release every temp string of the instance pool (strzone results included).
// Called before the state memset on load/disconnect.
void CSQC_Client_FreeStringPool(void);

// Register the client CSQC debug commands (csqc_smoke etc.; CSQC_DEBUG only).
void CSQC_Client_RegisterCommands(void);

#endif /* CSQC_CLIENT_H */
