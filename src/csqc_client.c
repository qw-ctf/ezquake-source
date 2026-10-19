/*
csqc_client.c -- client-side PR1VM wrapper for our csprogs.dat (CSQC).

Loads the local csprogs.dat into a static client PR1VM instance when the server
offers CSQC (*csprogs / *csprogssize + FTE_PEXT_CSQC), registers the client
builtins and runs CSQC_Init; then CSQC_WorldLoaded on entering the world and
CSQC_UpdateView(w,h,menushown) each frame. registercommand -> Cmd_AddCommand;
executing a command -> CSQC_ConsoleCommand; on disconnect -> CSQC_Shutdown +
PR1VM_UnLoad and removal of the commands.
*/

#include "quakedef.h"
#include "keys.h"
#include "pr1vm.h"
#include "csqc_client.h"
#include "pmove.h"
#include "common_draw.h"
#include "r_texture.h"
#include "r_matrix.h"
#include "gl_model.h"
#include "input.h"
#include "qtv.h"
#include "version.h"
#include "hash.h"

// CSQC API level the engine reports to the module in CSQC_Init.
#ifndef CSQC_API_VERSION
#define CSQC_API_VERSION	1.0f
#endif

// Entity pool (slot != server number): CSQC_MAX_NUM is the upper bound of server
// numbers (number->slot map), CSQC_MAX_EDICTS is the arena edict-slot pool size
// (slot 0 = world, not managed). The module's .entnum field = server number;
// module-spawned entities have no number (.entnum=0). CSQC_MAX_NUM covers the full
// 15-bit wire domain (removeflag uses bit 15), so #76/#92 never hit an unmappable
// number and the parse loop is never cut short.
#define CSQC_MAX_NUM	32768
#define CSQC_MAX_EDICTS	4096

// Validator caps for untrusted csprogs. CSQC_MAX_BUILTINS bounds the client builtin
// numbers (first_statement == -num; the highest registered builtin is 742).
// CSQC_MAX_ENTITYFIELDS bounds entityfields so edict_size (entityfields*4) and the
// CSQC_MAX_EDICTS-slot arena (~16 MB at 1024 fields) cannot overflow.
#define CSQC_MAX_BUILTINS		1024
#define CSQC_MAX_ENTITYFIELDS	1024

// Client string tables + temp-string table of the client VM instance. Kept outside
// the shared pr1vm_t (the core only holds pointers to them in vm->) so the shared
// core carries no client data/logic.
//
// Temp strings are garbage-collected: every string result is an exact-size heap
// copy in a growable table, encoded as -(CSQC_TEMP_BASE + index) - a range the
// shared PR1VM_GetString never resolves (it returns NULL for num <= -2*MAX_PRSTR),
// so only CSQC_Client_GetString decodes it. Strings are not overwritten by later
// results; unreferenced ones are freed by CSQC_Client_StringGC after an outermost
// VM call (conservative scan of the globals and the edict arena). strtbl/numstr
// stay bound for the shared core but the client registers no strings in them.
#define CSQC_TEMP_BASE			(2 * MAX_PRSTR)
#define CSQC_TEMP_MAX			(1 << 20)	// hard cap on live temp strings (allocation failure -> null string)
#define CSQC_TEMP_GC_MIN		4096		// do not collect below this many live strings
typedef struct csqc_strpool_s {
	char *strtbl[MAX_PRSTR];
	char *newstrtbl[MAX_PRSTR];
	int numstr;
	// Temp-string table: tmp[0..tmphigh) slots, NULL = free (index on tmpfree).
	char **tmp;
	byte *tmpmark;  // GC mark per slot (sized maxtmp)
	int *tmpfree;  // free-slot stack below tmphigh
	int numtmpfree;
	int tmphigh;  // slots ever handed out (high-water mark)
	int maxtmp;   // allocated capacity of tmp/tmpmark/tmpfree
	int numtmp;   // live temp strings
	int gc_threshold; // collect when numtmp exceeds this
} csqc_strpool_t;

typedef struct csqc_client_state_s {
	pr1vm_t vm;
	qbool loaded;  // module loaded into the instance
	qbool inited;  // progs loaded without load-time error (CSQC_Init is optional)
	qbool errored; // PR_RunError on the client instance (frames disabled)
	qbool mayread; // module may read the net message - parse callbacks only
				   // (CSQC_Ent_Update/CSQC_Parse_Event)
	qbool world_done; // CSQC_WorldLoaded called
	qbool enable_sent; // enablecsqc/disablecsqc already sent to the server
	qbool enable_value; // last sent state (true=enablecsqc)
	// Keys whose key-down was delivered to CSQC_InputEvent: a key-up is delivered to
	// the module only if its down was delivered, then the entry is cleared; unmatched
	// ups (and reserved keys, which never reach the module) are dropped. Single-device
	// (devid is always 0 in ezq); zeroed by the memset on load/disconnect.
	byte csqckeysdown[UNKNOWN];
	int func_init, func_world, func_update, func_console, func_shutdown;
	int func_entupdate, func_entremove, func_parseevent, func_parsetempent;
	int func_entspawn; // CSQC_Ent_Spawn (or -1)
	int func_input;  // CSQC_Input_Frame (or -1)
	int func_inputevent; // CSQC_InputEvent (or -1)
	int func_startframe; // CSQC StartFrame (or -1)
	int func_endframe;  // CSQC EndFrame (or -1)
	int global_time; // offset of the time global (or -1)
	int global_gamespeed; // offset of the gamespeed global (or -1)
	int global_self; // offset of the self global (or -1)
	int global_other; // offset of the other global (or -1; think-loop)
	int global_physics_mode; // offset of the physics_mode global (or -1)
	int field_entnum; // float word of the .entnum field in entvars (or -1)
	// #347: standard physics field offsets (or -1).
	int f_origin, f_velocity, f_angles, f_mins, f_maxs;
	int f_movetype, f_flags, f_gravity, f_pmove_flags;
	int f_modelindex, f_skin; // #371 player/delta bridge (raw state fields)
	int f_frame, f_effects, f_colormap, f_drawmask; // #371 bridge (raw state fields)
	int f_think, f_nextthink; // think-loop: .think/.nextthink fields (or -1)
	// No player entities are fabricated for the module; only player_localentnum
	// is published.
	int g_localentnum; // module global player_localentnum (or -1)
	// input_* globals for CSQC_Input_Frame (or -1 if the module did not declare them).
	int in_timelength, in_angles, in_movevalues, in_buttons, in_impulse;
	int in_sequence; // input_sequence (#345) or -1
	// Prediction window globals (or -1).
	int g_ccframe;  // clientcommandframe
	int g_scframe;  // servercommandframe
	// Deprecated pmove_org/pmove_vel/pmove_onground globals (or -1; #347 writes them).
	int p_org, p_vel, p_onground;
	// #1 makevectors: module v_forward/v_right/v_up globals (or -1).
	int g_vfwd, g_vright, g_vup;
	int g_view_angles; // view_angles global (or -1)
	// Engine-maintained globals (or -1): frametime/cltime/maxclients/
	// player_localnum/intermission.
	int g_frametime, g_cltime, g_maxclients, g_player_localnum, g_intermission;
	// csprogs download (no valid local file): download csprogs.dat from the
	// server into csprogsvers/<crc>.dat; load once a valid file
	// appears (see CSQC_Client_Update).
	unsigned csprogs_crc; // *csprogs (md4 Com_BlockChecksum) / 0 if absent
	int csprogs_size; // *csprogssize
	char csprogs_dl_path[MAX_QPATH]; // local file after download
	int numcmds; // number of module-registered commands
	int maxcmds; // cmds capacity (dynamic)
	char **cmds;  // Q_malloc: module command names (removed on unload)
	// Edict arena of the client instance. Q_malloc; freed in Disconnect/Load-start;
	// bound into vm->edicts/game_edicts (entity opcodes).
	edict_t *edicts;
	byte *game_edicts;
	// Heap-owned csprogs.dat buffer (FS_LoadHeapFile). vm->progs/strings/globals
	// reference it, so it must outlive the module; freed on unload (Disconnect)
	// and on load failure.
	byte *module_data;
	// Client-owned copy of the globals lump with two zero words appended, so
	// OP_RETURN/OP_DONE may read a..a+2 for a == numglobals-1. Freed on unload.
	float *globals_owned;
	// Client string tables of the instance (see csqc_strpool_t): on load
	// vm->strtbl/newstrtbl/numstr point here.
	csqc_strpool_t strpool;
} csqc_client_state_t;

static csqc_client_state_t s_csqc;

// Engine download bookkeeping of the client CSQC layer. Kept outside s_csqc on
// purpose: loading a module memsets the module instance, but an engine transfer
// that has already been requested must survive that (the request belongs to the
// connection, not to the module instance).
typedef struct csqc_dl_state_s {
	qbool csprogs_dl_pending;
	double csprogs_dl_lastprogress; // time of last downloadpercent growth
	int csprogs_dl_percent;   // last seen cls.downloadpercent
	qbool csprogs_dl_started;   // our download has opened (cls.download)
	qbool csprogs_dl_requested;  // the download request was sent (slot was free)
	// A module-initiated request was sent but no svc_download block has arrived
	// yet: the shared cls.download is still NULL in that window, so it cannot be
	// used as the only in-flight marker.
	qbool dl_inflight;
	double dl_inflight_time;
	char dl_inflight_localname[MAX_OSPATH]; // cls.downloadname of our request
	char csprogs_dl_localname[MAX_OSPATH]; // cls.downloadname of our file (gate)
} csqc_dl_state_t;

static csqc_dl_state_t s_dl;

// CL_Download_Accept returns false when the file is already present locally (or
// the name is not downloadable); the file-existence check must run before a
// module request is queued behind an active transfer.
qbool CL_Download_Accept(const char *filename);

static qbool PR1VM_LoadClientV6(pr1vm_t *vm, const byte *data, int filesize);
#ifdef CSQC_DEBUG
static void PR1VM_CSQCSmoke_f(void);
static void CSQC_Client_ProgsCheck_f(void);
static void CSQC_Client_NetProbe_f(void);
static void CSQC_Client_DlCheck_f(void);
#endif

/*
=================
CSQC_Client_AllocTemp

Copy s into a free temp-table slot and return its encoded string_t, or 0 (null
string) when the table is at CSQC_TEMP_MAX.
=================
*/
static int CSQC_Client_AllocTemp(csqc_strpool_t *pool, const char *s)
{
	size_t len = strlen(s) + 1;
	int idx;

	if (pool->numtmpfree > 0) {
		idx = pool->tmpfree[--pool->numtmpfree];
	}
	else {
		if (pool->tmphigh >= pool->maxtmp) {
			int newmax;

			if (pool->maxtmp >= CSQC_TEMP_MAX) {
				return 0;
			}
			newmax = pool->maxtmp ? pool->maxtmp * 2 : CSQC_TEMP_GC_MIN;
			if (newmax > CSQC_TEMP_MAX) {
				newmax = CSQC_TEMP_MAX;
			}
			pool->tmp = (char **)Q_realloc(pool->tmp, (size_t)newmax * sizeof(*pool->tmp));
			pool->tmpmark = (byte *)Q_realloc(pool->tmpmark, (size_t)newmax);
			pool->tmpfree = (int *)Q_realloc(pool->tmpfree, (size_t)newmax * sizeof(*pool->tmpfree));
			pool->maxtmp = newmax;
		}
		idx = pool->tmphigh++;
	}

	pool->tmp[idx] = (char *)Q_malloc(len);
	memcpy(pool->tmp[idx], s, len);
	pool->numtmp++;
	return -(CSQC_TEMP_BASE + idx);
}

// Temp string for an encoded value, or NULL (not a temp value / freed slot).
static char *CSQC_Client_TempString(const csqc_strpool_t *pool, int num)
{
	unsigned int idx;

	if (!pool || num > -CSQC_TEMP_BASE || num == INT_MIN) {
		return NULL;
	}
	idx = (unsigned int)(-(num + CSQC_TEMP_BASE));
	if (idx >= (unsigned int)pool->tmphigh) {
		return NULL;
	}
	return pool->tmp[idx];
}

// Free every temp string and the table itself (module load / unload / disconnect).
static void CSQC_Client_FreeTempStrings(csqc_strpool_t *pool)
{
	int i;

	for (i = 0; i < pool->tmphigh; i++) {
		Q_free(pool->tmp[i]);
	}
	Q_free(pool->tmp);
	Q_free(pool->tmpmark);
	Q_free(pool->tmpfree);
	pool->tmp = NULL;
	pool->tmpmark = NULL;
	pool->tmpfree = NULL;
	pool->numtmpfree = pool->tmphigh = pool->maxtmp = pool->numtmp = 0;
	pool->gc_threshold = 0;
}

/*
=================
PR1VM_ClientSetString

Client wrapper over the shared PR1VM_SetString (core): strings from the module's
string area are passed to the core without a copy (offset); anything else becomes a
GC'd temp string (CSQC_Client_AllocTemp). Allocation failure writes a null string.
=================
*/
void PR1VM_ClientSetString(pr1vm_t *vm, int *address, char *s)
{
	csqc_strpool_t *pool;

	if (!address) {
		return;
	}

	if (!s || !s[0]) {
		*address = 0;
		return;
	}

	pool = (csqc_strpool_t *)vm->host_udata;
	if (!pool || !vm->strings || !vm->strtbl || !vm->numstr) {
		// Uninitialized string machinery: write a null string rather than leave
		// a stale OFS_RETURN.
		*address = 0;
		return;
	}

	// Already inside the module string area - the core writes the offset itself.
	if (s >= vm->strings && s < vm->strings + vm->progs->numstrings) {
		PR1VM_SetString(vm, (string_t *)address, s);
		return;
	}

	// Lives until the GC finds no reference to it (CSQC_Client_StringGC).
	*address = CSQC_Client_AllocTemp(pool, s);
}

// Free every temp string of the instance pool (strzone results included). Must run
// before the state memset on module load / disconnect.
void CSQC_Client_FreeStringPool(void)
{
	CSQC_Client_FreeTempStrings(&s_csqc.strpool);
}

// #345: ring buffer of sent usercmds (written from CL_SendCmd).
// seq = mirror of cls.netchan.outgoing_sequence (the client message number at
// write time; Netchan_Transmit increments AFTER writing the header, so during
// CL_SendCmd outgoing_sequence still equals the current cmd's number).
// Size 64 = UPDATE_BACKUP (prediction window).
#define CSQC_INHIST	64
typedef struct {
	unsigned int seq;
	usercmd_t cmd;
} csqc_inrec_t;
static csqc_inrec_t s_inhist[CSQC_INHIST];
static unsigned int s_last_seq; // seq of the last write (0 = none yet)
// The live clientcommandframe = seq of the last built cmd, not the next
// outgoing_sequence: Netchan_Transmit increments after sending, so in the render
// phase outgoing_sequence is already N+1 while clientcommandframe stays N.
// Updated in CSQC_Client_InputFrame.
static unsigned int s_ccframe; // 0 = no cmd built yet

// #460-469: string-buffer pool (DP). Growable, handle = slot + 1. str[i] == NULL is
// a hole (bufstr_free); num is the length including holes (buf_getsize), cap is
// the allocated length.
typedef struct
{
	qbool inuse;
	int num;
	int cap;
	char **str;
} csqc_buf_t;
static csqc_buf_t *s_bufs;
static int s_numbufs;

// #346 setsensitivityscaler: temporary mouse-sensitivity multiplier (zoom).
// Stored by the module; applied in in_sdl2.c.
static float s_sens_scale = 1;

// #343 setcursormode: the module's cursor state. While usecursor=1 and the module
// is active in-game (CSQC_Client_CSQCCursor), the mouse is free (vid_sdl2 does not
// hand it to the OS cursor) and SCR_DrawCursor draws the module's cursor.
typedef struct
{
	qbool usecursor;
	char cursorimage[MAX_QPATH];
	float hotspot[2];
	float scale;
} csqc_cursormode_t;
static csqc_cursormode_t s_cursormode;

// Client edict arena: an arbitrary slot pool; the server number lives in .entnum
// (map s_numslot: number->slot). Slot 0 = world. s_own - entity owned by the
// module (spawn, or a network entity handed over on remove / self change). A
// remove() of an owned slot drops its mapping; a remove() of an engine-owned
// network slot frees it but keeps the mapping (see CSQC_Client_EntFree).
static qbool s_used[CSQC_MAX_EDICTS];
static qbool s_own[CSQC_MAX_EDICTS];
static int s_numslot[CSQC_MAX_NUM];
// Reverse map slot->number. Invariant: s_numslot[N] must not outlive the slot's
// reuse - otherwise the engine would take a stale slot. (A network slot freed by
// the module's remove() stays mapped until AllocSlot reuses it.)
static int s_slotnum[CSQC_MAX_EDICTS];
// Module time (CSQC_Client_TimeNow) at which a slot was freed, 0 = never/instantly
// freed; drives the delayed reuse in CSQC_Client_AllocSlot.
static double s_freetime[CSQC_MAX_EDICTS];
// Bumped on every allocation of a slot, so a caller can tell the entity it set as
// self from an unrelated one that reused the slot (CSQC_Client_RemapAfterUpdate).
static unsigned int s_allocseq[CSQC_MAX_EDICTS];

// Module-initiated downloads (precache_model/sound/file). ezq has a single download
// slot whose name/tempname are assigned when the request is sent, while cls.download
// only becomes non-NULL on the first svc_download; checking cls.download alone lets
// two back-to-back CSQC_Init precaches overwrite each other. Queue requests and
// start one per frame.
#define CSQC_DL_QUEUE_MAX 32
// Upper bound on the "request sent, no block yet" window before the slot is
// considered lost (the engine has no such timeout of its own).
#define CSQC_DL_INFLIGHT_TIMEOUT 10.0
static char s_dl_queue[CSQC_DL_QUEUE_MAX][MAX_QPATH];
static int s_dl_queue_head;
static int s_dl_queue_count;

// Caps on distinct module/wire resource names: every new name costs a mod_known or
// sfx_t slot that is never freed, and both tables Sys_Error on exhaustion (4096/512).
// The caps are applied CSQC-side so the shared loaders stay unchanged.
#define CSQC_MAX_WIRE_MODELS 1024
#define CSQC_MAX_MODULE_SOUNDS 256
static char s_wire_model_names[CSQC_MAX_WIRE_MODELS][MAX_QPATH];
static int s_wire_model_count;
static char s_module_sound_names[CSQC_MAX_MODULE_SOUNDS][MAX_QPATH];
static int s_module_sound_count;

// Returns true when the name is already known or can be added without exceeding the
// cap; false means "treat as a stub / skip".
qbool CSQC_Client_AllowWireModelName(const char *name)
{
	int i;

	if (!name || !name[0]) {
		return false;
	}
	for (i = 0; i < s_wire_model_count; i++) {
		if (!strcmp(s_wire_model_names[i], name)) {
			return true;
		}
	}
	if (s_wire_model_count >= CSQC_MAX_WIRE_MODELS) {
		return false;
	}
	strlcpy(s_wire_model_names[s_wire_model_count], name, MAX_QPATH);
	s_wire_model_count++;
	return true;
}

// Cvars created by the module (#93/#72): validate the name, mark them so they are
// never written to config.cfg (CVAR_MOD_CREATED), cap the count and delete them on
// module unload. A hostile module could otherwise create unbounded cvars or names
// with metacharacters that end up in the user config.
#define CSQC_MAX_MODULE_CVARS 256
static char s_module_cvars[CSQC_MAX_MODULE_CVARS][128];
static int s_module_cvar_count;

qbool CSQC_Client_RegisterModuleCvar(const char *name)
{
	static const char *bad = "\" ;$\\^";
	int i;

	if (!name || !name[0] || strlen(name) >= 128) {
		return false;
	}
	if (strchr(name, '\r') || strchr(name, '\n') || strchr(name, '\t')) {
		return false;
	}
	for (i = 0; bad[i]; i++) {
		if (strchr(name, bad[i])) {
			return false;
		}
	}
	for (i = 0; i < s_module_cvar_count; i++) {
		if (!strcmp(s_module_cvars[i], name)) {
			return true;
		}
	}
	if (s_module_cvar_count >= CSQC_MAX_MODULE_CVARS) {
		return false;
	}
	strlcpy(s_module_cvars[s_module_cvar_count], name, 128);
	s_module_cvar_count++;
	return true;
}

void CSQC_Client_ClearModuleCvars(void)
{
	int i;

	for (i = 0; i < s_module_cvar_count; i++) {
		Cvar_Delete(s_module_cvars[i]);
	}
	s_module_cvar_count = 0;
}

qbool CSQC_Client_AllowModuleSound(const char *name)
{
	int i;

	if (!name || !name[0]) {
		return false;
	}
	for (i = 0; i < s_module_sound_count; i++) {
		if (!strcmp(s_module_sound_names[i], name)) {
			return true;
		}
	}
	if (s_module_sound_count >= CSQC_MAX_MODULE_SOUNDS) {
		return false;
	}
	strlcpy(s_module_sound_names[s_module_sound_count], name, MAX_QPATH);
	s_module_sound_count++;
	return true;
}

void CSQC_Client_GetScreenSize(int *w, int *h)
{
	if (w) {
		*w = vid.width;
	}
	if (h) {
		*h = vid.height;
	}
}

void CSQC_Client_DrawText(float x, float y, const char *text, int r, int g, int b, float alpha, float scale)
{
	extern cvar_t scr_coloredText;
	static char buf[16384];
	int prefix;
	float saved;
	if (!text) {
		return;
	}
	// Font scale from size.x (scale=size.x/8; 0 => 1). The module's color is passed
	// as an &cRGB code to the engine. To make it independent of the user's
	// scr_coloredText, temporarily enable it for the draw.
	saved = scr_coloredText.value;
	Cvar_SetValue(&scr_coloredText, 1);
	// &cRGB is 3 hex digits (channel*16), not &cRRGGBB.
	prefix = snprintf(buf, sizeof(buf), "&c%X%X%X", (bound(0, r, 255)) / 16, (bound(0, g, 255)) / 16, (bound(0, b, 255)) / 16);
	if (prefix < 0) {
		prefix = 0;
	}
	strlcpy(buf + prefix, text, sizeof(buf) - prefix);
	// FTE drawcolouredstring parity: alpha is applied; color=NULL -> color comes
	// from the &c codes.
	Draw_SColoredAlphaString(x, y, buf, NULL, 0, 0, (scale > 0) ? scale : 1, bound(0, alpha, 1), true);
	Cvar_SetValue(&scr_coloredText, saved);
}

// Color for the 2D draw helpers (rgb 0..255 bytes, alpha 0..1).
static color_t CSQC_Client_Color(int r, int g, int b, float alpha)
{
	return RGBA_TO_COLOR((byte)bound(0, r, 255), (byte)bound(0, g, 255),
		(byte)bound(0, b, 255), (byte)bound(0, (int)(alpha * 255.0f + 0.5f), 255));
}

/*
#324 drawsetcliparea / #325 drawresetcliparea - geometric clipping (GL scissor
does not fit ezq's deferred 2D pipeline). The clip rectangle is stored in CSQC
draw coordinates; rectangular primitives (pic/subpic/fill) are intersected with
it, text/lines are only skipped if fully outside (they are not cut).
*/
static qbool s_clip_on = false;
static float s_clip_x, s_clip_y, s_clip_w, s_clip_h;

// Intersects the dest-rect (x,y,w,h) with the active clip. false = empty/outside.
static qbool CSQC_Client_ClipDest(float *x, float *y, float *w, float *h)
{
	float x0, y0, x1, y1;
	if (!s_clip_on) {
		return true;
	}
	x0 = *x;
	y0 = *y;
	x1 = *x + *w;
	y1 = *y + *h;
	if (x1 <= s_clip_x || x0 >= s_clip_x + s_clip_w || y1 <= s_clip_y || y0 >= s_clip_y + s_clip_h) {
		return false;
	}
	x0 = (x0 > s_clip_x) ? x0 : s_clip_x;
	y0 = (y0 > s_clip_y) ? y0 : s_clip_y;
	x1 = (x1 < s_clip_x + s_clip_w) ? x1 : s_clip_x + s_clip_w;
	y1 = (y1 < s_clip_y + s_clip_h) ? y1 : s_clip_y + s_clip_h;
	*x = x0;
	*y = y0;
	*w = x1 - x0;
	*h = y1 - y0;
	return true;
}

/*
=================
2D graphics. Coordinates/sizes are raw video pixels (as DrawText).
scale = size / native size.
=================
*/
void CSQC_Client_DrawFill(float x, float y, float w, float h, int r, int g, int b, float alpha)
{
	if (w <= 0 || h <= 0) {
		return;
	}
	if (!CSQC_Client_ClipDest(&x, &y, &w, &h)) {
		return;
	}
	Draw_AlphaRectangleRGB(x, y, w, h, 1, true, CSQC_Client_Color(r, g, b, alpha));
}

qbool CSQC_Client_DrawPic(float x, float y, float w, float h, const char *name, int r, int g, int b, float alpha)
{
	mpic_t *pic;
	float sx, sy, a = bound(0, alpha, 1);
	float dx, dy, dw, dh, srcx, srcy, srcw, srch;
	if (!name || !name[0]) {
		return false;
	}
	pic = Draw_CachePicSafe(name, false, false);
	if (!pic) {
		return false;
	}
	// #322: a negative size means mirroring. Signed scale_x/y flips the texture;
	// clip is not applied (a negative dest breaks the intersection).
	if (w < 0 || h < 0) {
		sx = w / (float)pic->width;
		sy = h / (float)pic->height;
		if (r == 255 && g == 255 && b == 255) {
			Draw_SAlphaSubPic2(x, y, pic, 0, 0, pic->width, pic->height, sx, sy, a);
		}
		else {
			Draw_SColoredSubPic2(x, y, pic, 0, 0, pic->width, pic->height, sx, sy,
				bound(0, r, 255), bound(0, g, 255), bound(0, b, 255), a);
		}
		return true;
	}
	if (w == 0 || h == 0) {
		return true;
	}
	// Clip: intersect dest with the active area, recompute the source (the
	// "whole pic -> dest" property is preserved).
	dx = x;
	dy = y;
	dw = w;
	dh = h;
	if (!CSQC_Client_ClipDest(&dx, &dy, &dw, &dh)) {
		return true;
	}
	sx = w / (float)pic->width;
	sy = h / (float)pic->height;
	srcx = (dx - x) / sx;
	srcy = (dy - y) / sy;
	srcw = dw / sx;
	srch = dh / sy;
	if (r == 255 && g == 255 && b == 255) {
		Draw_SAlphaSubPic2(dx, dy, pic, (int)srcx, (int)srcy, (int)srcw, (int)srch, sx, sy, a);
	}
	else {
		Draw_SColoredSubPic2(dx, dy, pic, (int)srcx, (int)srcy, (int)srcw, (int)srch, sx, sy,
			bound(0, r, 255), bound(0, g, 255), bound(0, b, 255), a);
	}
	return true;
}

void CSQC_Client_DrawSubPic(float x, float y, float w, float h, const char *name, float srcx, float srcy, float srcw, float srch, int r, int g, int b, float alpha)
{
	mpic_t *pic;
	float a = bound(0, alpha, 1);
	float dx, dy, dw, dh, nsx, nsy, nsw, nsh, ssx, ssy;
	if (!name || !name[0] || srcw <= 0 || srch <= 0 || !CSQC_Client_PicSafe(name)) {
		return;
	}
	pic = Draw_CachePicSafe(name, false, false);
	if (!pic) {
		return;
	}
	// #328: srcpos/srcsize are normalized (0..1) texture coordinates.
	// Draw_SAlphaSubPic2 takes the source rect in pixels, so scale it up first.
	srcx *= pic->width;
	srcy *= pic->height;
	srcw *= pic->width;
	srch *= pic->height;
	// A negative size on one axis means mirroring (signed scale).
	if (w < 0 || h < 0) {
		ssx = w / srcw;
		ssy = h / srch;
		if (r == 255 && g == 255 && b == 255) {
			Draw_SAlphaSubPic2(x, y, pic, (int)srcx, (int)srcy, (int)srcw, (int)srch, ssx, ssy, a);
		}
		else {
			Draw_SColoredSubPic2(x, y, pic, (int)srcx, (int)srcy, (int)srcw, (int)srch, ssx, ssy,
				bound(0, r, 255), bound(0, g, 255), bound(0, b, 255), a);
		}
		return;
	}
	if (w == 0 || h == 0) {
		return;
	}
	// Clip as in DrawPic: dest is intersected, source via affine mapping.
	dx = x;
	dy = y;
	dw = w;
	dh = h;
	if (!CSQC_Client_ClipDest(&dx, &dy, &dw, &dh)) {
		return;
	}
	ssx = w / srcw;
	ssy = h / srch;
	nsx = srcx + ((dx - x) / w) * srcw;
	nsy = srcy + ((dy - y) / h) * srch;
	nsw = (dw / w) * srcw;
	nsh = (dh / h) * srch;
	if (r == 255 && g == 255 && b == 255) {
		Draw_SAlphaSubPic2(dx, dy, pic, (int)nsx, (int)nsy, (int)nsw, (int)nsh, ssx, ssy, a);
	}
	else {
		Draw_SColoredSubPic2(dx, dy, pic, (int)nsx, (int)nsy, (int)nsw, (int)nsh, ssx, ssy,
			bound(0, r, 255), bound(0, g, 255), bound(0, b, 255), a);
	}
}

void CSQC_Client_DrawCharacter(float x, float y, int ch, int r, int g, int b, float alpha, float scale)
{
	extern cvar_t scr_coloredText;
	static char buf[8];
	float saved;
	int c = ch & 0xff;
	if (c <= 0) {
		return;
	}
	// One default-font character with an &cRGB color (as DrawText).
	saved = scr_coloredText.value;
	Cvar_SetValue(&scr_coloredText, 1);
	snprintf(buf, sizeof(buf), "&c%X%X%X%c", (bound(0, r, 255)) / 16, (bound(0, g, 255)) / 16, (bound(0, b, 255)) / 16, c);
	// alpha as in FTE drawcharacter.
	Draw_SColoredAlphaString(x, y, buf, NULL, 0, 0, (scale > 0) ? scale : 1, bound(0, alpha, 1), true);
	Cvar_SetValue(&scr_coloredText, saved);
}

void CSQC_Client_DrawLine(float x1, float y1, float x2, float y2, float width, int r, int g, int b, float alpha)
{
	if (width <= 0) {
		return;
	}
	Draw_AlphaLineRGB(x1, y1, x2, y2, width, CSQC_Client_Color(r, g, b, alpha));
}

float CSQC_Client_StringWidth(const char *text, qbool usecolours, float fontsize_x)
{
	float scale;
	if (!text) {
		return 0;
	}
	// Same scale and metric as DrawText.
	scale = (fontsize_x > 0) ? fontsize_x / 8.0f : 1;
	if (usecolours) {
		return Draw_StringLengthColors(text, -1, scale, true);
	}
	// usecolours=0: FTE #327 keeps the text (keepmarkup) and counts its characters.
	return Draw_StringLength(text, -1, scale, true);
}

/*
=================
CSQC_Client_DrawFontScaleX

x multiplier of the `drawfontscale` (vector) global of the active VM - shared by
CSQC and MenuQC. The offset is cached per VM and re-resolved when the VM changes.

No global, or both components zero, means no scale (text is not squashed to 0).
The y component is ignored (the ezq font scales uniformly).
=================
*/
static pr1vm_t *s_drawfontscale_vm = NULL;
static int s_drawfontscale_ofs = -1;

// Invalidate the per-VM drawfontscale offset cache. The client VM address is stable
// across module reloads, so a cached offset from a previous module would read a
// foreign/out-of-range global; re-resolve on the next text call.
static void CSQC_Client_DrawFontScaleReset(void)
{
	s_drawfontscale_vm = NULL;
	s_drawfontscale_ofs = -1;
}

float CSQC_Client_DrawFontScaleX(pr1vm_t *vm)
{
	float x, y;

	if (!vm) {
		return 1.0f;
	}
	if (s_drawfontscale_vm != vm) {
		s_drawfontscale_vm = vm;
		s_drawfontscale_ofs = PR1VM_FindGlobal(vm, "drawfontscale");
	}
	if (s_drawfontscale_ofs < 0) {
		return 1.0f;
	}
	x = vm->globals[s_drawfontscale_ofs + 0];
	y = vm->globals[s_drawfontscale_ofs + 1];
	if (x == 0.0f && y == 0.0f) {
		return 1.0f;
	}
	return x;
}

qbool CSQC_Client_PrecachePic(const char *name)
{
	if (!name || !name[0] || !CSQC_Client_PicSafe(name)) {
		return false;
	}
	return Draw_CachePicSafe(name, false, false) != NULL;
}

// Additional 2D graphics (#316/#318/#319/#321/#324/#325/#329).

// Per-name cache of resolved image sizes: a module may call #318
// drawgetimagesize for the same pic every frame, and resolving it means reading
// the .lmp header or decoding the image file. The table maps the name to an
// entry in s_picsize. It is flushed on module load and unload and when a
// download completes (the file behind a name may have changed), and when it is
// full, so a module with many pics re-reads them rather than losing the cache.
#define CSQC_PICSIZE_CACHE		1024
#define CSQC_PICSIZE_BUCKETS	256
typedef struct {
	int w, h;
} csqc_picsize_t;
static csqc_picsize_t s_picsize[CSQC_PICSIZE_CACHE];
static int s_picsize_count;
static hashtable_t *s_picsize_table;

static void CSQC_Client_PicSizeReset(void)
{
	if (s_picsize_table) {
		Hash_Flush(s_picsize_table);
	}
	s_picsize_count = 0;
}

static qbool CSQC_Client_PicSizeCached(const char *name, int *w, int *h)
{
	const csqc_picsize_t *size;

	if (!s_picsize_table) {
		return false;
	}
	size = Hash_Get(s_picsize_table, (char *)name);
	if (!size) {
		return false;
	}
	*w = size->w;
	*h = size->h;
	return true;
}

static void CSQC_Client_PicSizeStore(const char *name, int w, int h)
{
	if (!s_picsize_table) {
		s_picsize_table = Hash_InitTable(CSQC_PICSIZE_BUCKETS);
	}
	if (s_picsize_count >= CSQC_PICSIZE_CACHE) {
		CSQC_Client_PicSizeReset();
	}
	s_picsize[s_picsize_count].w = w;
	s_picsize[s_picsize_count].h = h;
	Hash_Add(s_picsize_table, (char *)name, &s_picsize[s_picsize_count]);
	s_picsize_count++;
}

/*
=================
CSQC_Client_PicSafe

Pre-check for .lmp paths: Draw_CachePicSafe trusts the 8-byte header
(width/height) without checking the file length, so naming any non-lmp file
"x.lmp" (e.g. gfx/palette.lmp) makes the draw path read/copy width*height bytes
past a short buffer. Validate the lump first via the #318 size check; other
extensions pass through to the image loaders.
=================
*/
qbool CSQC_Client_PicSafe(const char *name)
{
	const char *ext;
	float w = 0, h = 0;

	if (!name || !name[0]) {
		return false;
	}
	ext = COM_FileExtension(name);
	if (!ext || strcasecmp(ext, "lmp")) {
		return true;
	}
	return CSQC_Client_PicSize(name, &w, &h) && w > 0 && h > 0;
}

qbool CSQC_Client_IsCachedPic(const char *name)
{
	if (!name || !name[0]) {
		return false;
	}
	return CachePic_Find(name, false) != NULL;
}

qbool CSQC_Client_PicSize(const char *name, float *w, float *h)
{
	mpic_t *pic;
	const char *ext;
	char path[MAX_QPATH];
	int cw, ch;
	if (!name || !name[0] || !CSQC_Client_PicSafe(name)) {
		return false;
	}
	// #318: a bare name defaults to ".lmp". A .lmp size is read from its header
	// directly (Draw_CachePicSafe on a .lmp path returns an unrelated size).
	ext = COM_FileExtension(name);
	if (!ext || !ext[0]) {
		strlcpy(path, name, sizeof(path));
		COM_DefaultExtension(path, ".lmp", sizeof(path));
		name = path;
		ext = COM_FileExtension(name);
	}
	if (CSQC_Client_PicSizeCached(name, &cw, &ch)) {
		if (w) {
			*w = (float)cw;
		}
		if (h) {
			*h = (float)ch;
		}
		return true;
	}
	if (!strcasecmp(ext, "lmp")) {
		// .lmp header (qpic_t): two LE ints, read directly (wad.h requires
		// texture_t). Validate the lump before trusting w/h (size >= 8 and
		// size == 8 + w*h).
		byte *data;
		int len = 0, iw, ih;
		data = FS_LoadTempFile((char *)name, &len);
		if (!data || len < 8) {
			return false;
		}
		memcpy(&iw, data, sizeof(iw));
		memcpy(&ih, data + sizeof(iw), sizeof(ih));
		iw = LittleLong(iw);
		ih = LittleLong(ih);
		if (iw <= 0 || ih <= 0 || (long long)len != 8LL + (long long)iw * (long long)ih) {
			return false;
		}
		if (w) {
			*w = (float)iw;
		}
		if (h) {
			*h = (float)ih;
		}
		CSQC_Client_PicSizeStore(name, iw, ih);
		return true;
	}
	pic = R_LoadPicImage(name, NULL, 0, 0, TEX_ALPHA);
	if (!pic) {
		return false;
	}
	if (w) {
		*w = (float)pic->width;
	}
	if (h) {
		*h = (float)pic->height;
	}
	CSQC_Client_PicSizeStore(name, pic->width, pic->height);
	return true;
}

void CSQC_Client_DrawRawText(float x, float y, const char *text, int r, int g, int b, float alpha, float scale)
{
	char one[2];
	const char *p;
	float xx;
	if (!text) {
		return;
	}
	// "Raw" output: each character is drawn as a single colored glyph - there is no
	// place to build an &cRGB within one string, so an & in the module text is
	// output literally (as FTE drawrawstring). Color and alpha are applied.
	xx = x;
	for (p = text; *p; p++) {
		one[0] = *p;
		one[1] = 0;
		CSQC_Client_DrawCharacter(xx, y, (int)(unsigned char)*p, r, g, b, alpha, scale);
		xx += Draw_StringLength(one, 1, (scale > 0) ? scale : 1, true);
	}
}

/*
#324 drawsetcliparea / #325 drawresetcliparea (see CSQC_Client_ClipDest). The clip
state is per frame: CSQC_Client_Update clears it before the module runs.
*/
void CSQC_Client_SetClipArea(float x, float y, float w, float h)
{
	s_clip_x = x;
	s_clip_y = y;
	s_clip_w = (w > 0) ? w : 0;
	s_clip_h = (h > 0) ? h : 0;
	s_clip_on = true;
}

void CSQC_Client_ResetClipArea(void)
{
	s_clip_on = false;
}

void CSQC_Client_SetCursorMode(qbool usecursor, const char *image, float hotspot_x, float hotspot_y, float scale)
{
	// Only stored here; CSQC_Client_CSQCCursor() decides when it takes effect.
	s_cursormode.usecursor = usecursor;
	// Without an image argument the previous image/hotspot/scale are kept.
	if (!image) {
		return;
	}
	strlcpy(s_cursormode.cursorimage, image, sizeof(s_cursormode.cursorimage));
	s_cursormode.hotspot[0] = hotspot_x;
	s_cursormode.hotspot[1] = hotspot_y;
	s_cursormode.scale = (scale > 0) ? scale : 1;
}

qbool CSQC_Client_CursorRequested(void)
{
	return s_cursormode.usecursor;
}

// #344: raw mouse motion since the last getmousepos, in vid.conwidth units.
static float s_mousemove_x, s_mousemove_y;

void CSQC_Client_AccumMouseDelta(float dx, float dy)
{
	CSQC_Client_ScaleCursorDelta(&dx, &dy);
	s_mousemove_x += dx;
	s_mousemove_y += dy;
}

void CSQC_Client_TakeMouseDelta(float *x, float *y)
{
	*x = s_mousemove_x;
	*y = s_mousemove_y;
	s_mousemove_x = s_mousemove_y = 0;
}

qbool CSQC_Client_CSQCCursor(void)
{
	// The module cursor only applies in the game frame (key_game): when the engine
	// console/menu is open their own cursor/mouse take priority.
	return s_cursormode.usecursor && s_csqc.loaded && !s_csqc.errored && key_dest == key_game;
}

// Mouse position/delta are returned in vid.conwidth units (as Draw_* and
// IE_MOUSEABS), while cursor_x/y and mx/my are in render-2D (VID_RenderWidth2D).
// Same conversion as SCR_UpdateCursor.
static float CSQC_Client_CursorScaleX(void)
{
	int rw = VID_RenderWidth2D();
	return (rw > 0) ? (float)vid.conwidth / (float)rw : 1.0f;
}

static float CSQC_Client_CursorScaleY(void)
{
	int rh = VID_RenderHeight2D();
	return (rh > 0) ? (float)vid.conheight / (float)rh : 1.0f;
}

void CSQC_Client_ScaleCursorDelta(float *x, float *y)
{
	if (x) {
		*x *= CSQC_Client_CursorScaleX();
	}
	if (y) {
		*y *= CSQC_Client_CursorScaleY();
	}
}

void CSQC_Client_GetCursorPos(float *x, float *y)
{
	extern double cursor_x, cursor_y; // pointer coordinates in render-2D
	if (x) {
		*x = (float)cursor_x * CSQC_Client_CursorScaleX();
	}
	if (y) {
		*y = (float)cursor_y * CSQC_Client_CursorScaleY();
	}
}

void CSQC_Client_SetSensitivityScale(float scale)
{
	// May be 0.
	s_sens_scale = scale;
}

float CSQC_Client_SensitivityScale(void)
{
	// Inactive module - no effect (default 1).
	if (!s_csqc.loaded || s_csqc.errored) {
		return 1;
	}
	return s_sens_scale;
}

void CSQC_Client_DrawCursor(void)
{
	extern double cursor_x, cursor_y;
	mpic_t *pic;
	float scale, x, y;

	if (!CSQC_Client_CSQCCursor()) {
		return;
	}
	// scale <= 0 -> 1; hotspot is the cursor "tip" in image pixels (multiplied by
	// the scale), so the position points at the click point. The cursor position
	// is in vid.conwidth units (as Draw_*), while cursor_x/y is in render-2D.
	scale = (s_cursormode.scale > 0) ? s_cursormode.scale : 1;
	x = (float)cursor_x * CSQC_Client_CursorScaleX() - s_cursormode.hotspot[0] * scale;
	y = (float)cursor_y * CSQC_Client_CursorScaleY() - s_cursormode.hotspot[1] * scale;

	if (s_cursormode.cursorimage[0] && CSQC_Client_PicSafe(s_cursormode.cursorimage)) {
		pic = Draw_CachePicSafe(s_cursormode.cursorimage, false, false);
		if (!pic) {
			pic = Draw_CachePicSafe(s_cursormode.cursorimage, false, true); // tga/png
		}
		if (pic) {
			Draw_SColoredSubPic2(x, y, pic, 0, 0, pic->width, pic->height, scale, scale, 255, 255, 255, 1);
			return;
		}
	}
	// No image - default crosshair (visually like the ezquake cursor).
	{
		color_t c = RGBA_TO_COLOR(0, 255, 0, 255);
		float s = scale;
		Draw_AlphaLineRGB(x + 4 * s, y + 4 * s, x + 16 * s, y + 16 * s, 2 * s, c);
		Draw_AlphaLineRGB(x, y, x + 8 * s, y, 2 * s, c);
		Draw_AlphaLineRGB(x, y, x, y + 8 * s, 2 * s, c);
		Draw_AlphaLineRGB(x + 8 * s, y, x, y + 8 * s, 2 * s, c);
	}
}

static void CSQC_Client_ConsoleCommand_f(void);

void CSQC_Client_RegisterCommand(const char *cmd)
{
	int i;
	char *name;

	if (!cmd || !cmd[0]) {
		return;
	}
	for (i = 0; i < s_csqc.numcmds; i++) {
		if (!strcmp(s_csqc.cmds[i], cmd)) {
			return;     // already registered
		}
	}

	// Reject a name that collides with an existing cvar: commands resolve before
	// cvars (Cmd_ExecuteString), so a module could otherwise shadow e.g.
	// rcon_password and intercept the user's cvar input.
	if (Cvar_Find(cmd)) {
		Con_Printf("CSQC: registercommand: '%s' is a cvar, refused\n", cmd);
		return;
	}

	// Bounded: each command is an engine hash entry that is only freed on unload;
	// an unbounded module could exhaust memory.
	if (s_csqc.numcmds >= 256) {
		Con_DPrintf("CSQC: registercommand: module command cap reached\n");
		return;
	}
	if (s_csqc.numcmds >= s_csqc.maxcmds) {
		int newmax = s_csqc.maxcmds ? s_csqc.maxcmds * 2 : 16;
		s_csqc.cmds = (char **)Q_realloc(s_csqc.cmds, newmax * sizeof(s_csqc.cmds[0]));
		s_csqc.maxcmds = newmax;
	}

	// Cmd_AddRemCommand copies the name into a Q_malloc block (unlike
	// Cmd_AddCommand, which holds a pointer to the name and allocates the node in
	// the hunk). The node/name survive Host_ClearMemory and are removed correctly by
	// RemoveCommand. Our own copy is needed to remove the command on module unload.
	name = Q_strdup(cmd);
	if (!Cmd_AddRemCommand(name, CSQC_Client_ConsoleCommand_f)) {
		Q_free(name);
		return;
	}
	s_csqc.cmds[s_csqc.numcmds] = name;
	s_csqc.numcmds++;
}

/*
=================
CSQC_Client_RegisterCommands

Registers the CSQC_DEBUG console commands.
=================
*/
void CSQC_Client_RegisterCommands(void)
{
#ifdef CSQC_DEBUG
	Cmd_AddCommand("csqc_smoke", PR1VM_CSQCSmoke_f);
	Cmd_AddCommand("csqc_progscheck", CSQC_Client_ProgsCheck_f);
	Cmd_AddCommand("csqc_netprobe", CSQC_Client_NetProbe_f);
	Cmd_AddCommand("csqc_dlcheck", CSQC_Client_DlCheck_f);
#endif
}

static void CSQC_Client_HostPrint(pr1vm_t *vm, const char *msg)
{
	(void)vm;
	Con_Printf("%s\n", msg);
}

static void CSQC_Client_NotifyCSQC(qbool enable);

static void CSQC_Client_HostError(pr1vm_t *vm, const char *msg)
{
	Con_Printf("CSQC (PR1VM) program error: %s\n", msg);
	s_csqc.errored = true;
	// Tell the server to stop the CSQC stream: frames stay disabled until the next
	// ConnectCheck, but the server keeps sending #76/#83 otherwise.
	CSQC_Client_NotifyCSQC(false);
	// Do not return into the interpreter - unwind back to the outermost active
	// setjmp in PR1VM_ExecuteProgram (the client VM always has abortbuf_valid set).
	if (vm && vm->abortbuf_valid && vm->abortbuf) {
		longjmp(*vm->abortbuf, 1);
	}
	// No abort buffer: return; the calling PR_RunError takes the fatal path.
}

/*
=================
CSQC_Client_Abort

Fatal module error: print the reason and disconnect the client from the server
(back to the menu), then Host_Abort (longjmp
into Host_Frame) - do not return into the executing VM. errored is set BEFORE
CL_Disconnect so CSQC_Client_Disconnect does not call func_shutdown re-entrantly
(we are inside the executing VM).

CSQC_Shutdown is the exception: it runs while the client is already being torn
down, which on quit can be outside Host_Frame, where Host_Abort has no frame to
jump to. There the error only ends the module and the teardown carries on.
=================
*/
static qbool s_in_shutdown; // CSQC_Shutdown is executing (CSQC_Client_Disconnect)

void CSQC_Client_Abort(const char *msg)
{
	Con_Printf("CSQC (PR1VM) fatal: %s\n", msg ? msg : "fatal");
	s_csqc.errored = true;
	if (s_in_shutdown) {
		pr1vm_t *vm = &s_csqc.vm;

		// Unwind to PR1VM_ExecuteProgram's entry; without a target, return to
		// the builtin (the module is disabled either way).
		if (vm->abortbuf_valid && vm->abortbuf) {
			longjmp(*vm->abortbuf, 1);
		}
		return;
	}
	CL_Disconnect();
	Host_Abort();
}

/*
=================
CSQC_Client_GetString

Bounded string reads of the client VM (untrusted downloaded csprogs.dat): a
positive offset must lie inside the module's string block; anything beyond is a
forged value and returns NULL (callers map it to ""/skip). The shared PR1VM_GetString
stays unbounded (server/map strings live past numstrings), so the client bound
lives here and is installed as vm->get_string.
=================
*/
char *CSQC_Client_GetString(pr1vm_t *vm, int num)
{
	// GC'd temp strings live in a client-only range below the core's tables.
	if (num <= -CSQC_TEMP_BASE) {
		return vm ? CSQC_Client_TempString((const csqc_strpool_t *)vm->host_udata, num) : NULL;
	}
	if (num >= 0 && (!vm || !vm->progs || num >= vm->progs->numstrings)) {
		return NULL;
	}
	return PR1VM_GetString(vm, num);
}

// Client map-uptime and previous cl.time, for frametime =
// bound(0, cl.time - prev, 0.1) and cltime = realtime - mapstarttime.
static double s_mapstarttime;
static double s_prev_cltime;

// Module time: cl.servertime, or client map-uptime (the cltime domain) when the
// server sends no STAT_TIME/svc_time.
static double CSQC_Client_TimeNow(void)
{
	if (cl.servertime_works) {
		return cl.servertime;
	}
	return cls.realtime - s_mapstarttime;
}

static void CSQC_Client_SetTime(void)
{
	pr1vm_t *vm = &s_csqc.vm;
	if (s_csqc.global_time >= 0) {
		vm->globals[s_csqc.global_time] = (float)CSQC_Client_TimeNow();
	}
}

// OP_STATE for the client module: uses the module's own field/global offsets
// (f_nextthink/f_frame/f_think, global_time), not the fixed entvars_t/globalvars_t.
// self is an arena slot (raw value = slot*edict_size).
static void CSQC_Client_StateOp(pr1vm_t *vm, float frame, func_t func)
{
	float *v;
	int slot = 0;

	if (s_csqc.global_self >= 0) {
		slot = CSQC_Client_EntNum(vm, *(int *)&vm->globals[s_csqc.global_self]);
	}
	if (slot < 0 || slot >= vm->max_edicts) {
		return;
	}
	// OP_STATE writes fields directly, bypassing OP_ADDRESS/OP_STOREP: world is
	// read-only outside the trusted init/world-loaded callbacks.
	if (slot == 0 && vm->abortbuf_valid && !vm->world_writable) {
		PR_RunError("assignment to world entity");
	}
	v = (float *)((byte *)vm->game_edicts + (size_t)slot * vm->edict_size);

	if (s_csqc.f_nextthink >= 0) {
		v[s_csqc.f_nextthink] = ((s_csqc.global_time >= 0) ? vm->globals[s_csqc.global_time] : 0.0f) + 0.1f;
	}
	if (s_csqc.f_frame >= 0 && frame != v[s_csqc.f_frame]) {
		v[s_csqc.f_frame] = frame;
	}
	if (s_csqc.f_think >= 0) {
		*(int *)&v[s_csqc.f_think] = (int)func;
	}
}

static qbool CSQC_Client_Exec(int fidx)
{
	pr1vm_t *vm = &s_csqc.vm;
	// A module that already raised a runtime error is disabled: a caller that is
	// halfway through a wire message must not run it again.
	if (s_csqc.errored) {
		return false;
	}
	if (fidx <= 0 || fidx >= vm->progs->numfunctions) {
		return false;
	}
	CSQC_Client_SetTime();
	PR1VM_ExecuteProgram(vm, (func_t)fidx);
	return !s_csqc.errored;
}

/* As CSQC_Client_Exec, but returns the module's G_FLOAT(OFS_RETURN) (for the
 * delta callback: return != 0 = "the engine does not draw the entity"). */
static qbool CSQC_Client_ExecRet(int fidx, float *ret)
{
	pr1vm_t *vm = &s_csqc.vm;
	if (ret) {
		*ret = 0;
	}
	if (s_csqc.errored) {
		return false;
	}
	if (fidx <= 0 || fidx >= vm->progs->numfunctions) {
		return false;
	}
	CSQC_Client_SetTime();
	PR1VM_ExecuteProgram(vm, (func_t)fidx);
	if (ret) {
		*ret = vm->globals[OFS_RETURN];
	}
	return !s_csqc.errored;
}

static void CSQC_Client_ClearCommands(void)
{
	int i;
	for (i = 0; i < s_csqc.numcmds; i++) {
		Cmd_RemoveCommand(s_csqc.cmds[i]);
		Q_free(s_csqc.cmds[i]);
	}
	s_csqc.numcmds = 0;
	if (s_csqc.cmds) {
		Q_free(s_csqc.cmds);
		s_csqc.cmds = NULL;
	}
	s_csqc.maxcmds = 0;
}

// Re-entrancy guard for the console catch-all (and the module's own registered
// commands): the module may issue localcmd, which can hit an unknown command and
// would otherwise recurse into CSQC_ConsoleCommand. Cleared on module load.
static qbool s_console_executing;

/*
=================
CSQC_Client_ConsoleCommand

Run CSQC_ConsoleCommand(string cmd) and return the module's result (non-zero =
handled). Used by the registered command wrapper and by the cmd.c catch-all for
unknown commands.
=================
*/
qbool CSQC_Client_ConsoleCommand(const char *line)
{
	pr1vm_t *vm = &s_csqc.vm;
	qbool handled;

	if (!line || !line[0]) {
		return false;
	}
	if (!s_csqc.loaded || !s_csqc.inited || s_csqc.errored) {
		return false;
	}
	if (s_csqc.func_console <= 0) {
		return false;
	}
	if (s_console_executing) {
		return false;
	}

	s_console_executing = true;
	CSQC_Client_SetTime();
	PR1VM_ClientSetString(vm, (string_t *)&vm->globals[OFS_PARM0], (char *)line);
	vm->globals[OFS_RETURN] = 0;
	PR1VM_ExecuteProgram(vm, (func_t)s_csqc.func_console);
	handled = (vm->globals[OFS_RETURN] != 0);
	s_console_executing = false;
	return handled;
}

/*
=================
CSQC_Client_ConsoleCommand_f

A command registered by the module via registercommand. Rebuild the full line
("name arg1 arg2 ...") and run CSQC_ConsoleCommand.
=================
*/
static void CSQC_Client_ConsoleCommand_f(void)
{
	const char *line;

	if (Cmd_Argc() > 1) {
		line = va("%s %s", Cmd_Argv(0), Cmd_Args());
	}
	else {
		line = Cmd_Argv(0);
	}

	CSQC_Client_ConsoleCommand(line);
}

int CSQC_Client_Active(void)
{
	return s_csqc.loaded && !s_csqc.errored;
}

/*
=================
renderscene takeover

The module owns the 3D scene: when the module is active, CSQC_UpdateView
is called in the 3D phase (SCR_UpdateScreenPlayerView) instead of R_RenderView();
#300 clearscene / #301 addentities fill cl_visents; #304 renderscene runs
R_RenderView(). The s_scene_rendered flag guards against a black screen: if the
module did not call renderscene, the engine draws the frame itself (fallback).
=================
*/
static qbool s_scene_rendered = false;
static qbool s_scene_viewmodel = false; // #301 mask&MASK_STDVIEWMODEL requested

qbool CSQC_Client_SceneActive(void)
{
	return s_csqc.loaded && s_csqc.inited && !s_csqc.errored && s_csqc.func_update > 0;
}

void CSQC_Client_BeginScene(void)
{
	s_scene_rendered = false;
	s_scene_viewmodel = false;
}

// #301 mask&2: the module requested the engine viewmodel in the scene.
void CSQC_Client_LinkViewModel(void)
{
	s_scene_viewmodel = true;
}

qbool CSQC_Client_SceneViewModel(void)
{
	return s_scene_viewmodel;
}

// Declared here: render.h is renderer-private and redefines types already
// pulled in.
extern void R_CSQC_ApplyModuleView(void);
extern void R_CSQC_ResetCull(void);

void CSQC_Client_RenderScene(void)
{
	s_scene_rendered = true;
	// Derive the render/cull view from r_refdef (the module camera) here too: if
	// the module never called addentities, R_CSQC_BeginCull has not run.
	R_CSQC_ApplyModuleView();
	R_RenderView();
	// Re-arm the once-per-frame arena cull guard so a second renderscene in the
	// same frame culls by its own camera.
	R_CSQC_ResetCull();
}

qbool CSQC_Client_SceneRendered(void)
{
	return s_scene_rendered;
}

/*
=================
CSQC_Client_CallPredraw

Call the arena edict's .predraw on #301/#302 with self = the slot and return its
result: 0 (PREDRAW_AUTOADD) adds the entity, non-zero skips it. If predraw removed
the edict or execution failed, *removed is set (do not add). self is restored;
.entnum is left untouched (unlike SetContextSlot).
=================
*/
float CSQC_Client_CallPredraw(int slot, int fidx, qbool *removed)
{
	pr1vm_t *vm = &s_csqc.vm;
	float ret = 0;
	int oldself = 0;

	if (removed) {
		*removed = false;
	}
	if (!vm || !vm->game_edicts || fidx <= 0) {
		return 0;
	}
	if (!CSQC_Client_EntUsed(slot)) {
		if (removed) {
			*removed = true;
		}
		return 0;
	}

	if (s_csqc.global_self >= 0) {
		oldself = *(int *)&vm->globals[s_csqc.global_self];
		*(int *)&vm->globals[s_csqc.global_self] = (int)slot * vm->edict_size;
	}

	if (CSQC_Client_Exec(fidx)) {
		ret = vm->globals[OFS_RETURN];
	}
	else if (removed) {
		*removed = true;
	}

	if (s_csqc.global_self >= 0) {
		*(int *)&vm->globals[s_csqc.global_self] = oldself;
	}

	if (removed && !CSQC_Client_EntUsed(slot)) {
		*removed = true;
	}
	return ret;
}

/*
=================
CSQC model registry

The module-visible model index is sign-split:
- positive `i`  -> server resource (`cl.model_name[i]` / `cl.model_precache[i]`);
- negative `-i` -> csqc-only slot in this registry (backing store `s_models` /
  `s_modelnames`, 1-based so slot i is `[-i-1]`);
- `0`           -> no model.

Lookup searches the csqc layer first, then the server precache.
#20/#75 precache_model / #200 getmodelindex / #333 setmodelindex / #334
modelnameforindex and the `.modelindex` field all share this domain. The index is
opaque to the module (`!= 0` / round-trip).

The name is registered even when the file is missing (Mod_ForName returns NULL):
the slot holds a NULL placeholder, but the index stays stable. After a successful
download the placeholder is filled in CSQC_Client_ModelDownloadFinished, without
the module re-precaching.

Capacity is CSQC_MAX_MODELS; exhaustion returns 0 rather than ending the game.
=================
*/
#define CSQC_MAX_MODELS 512
static struct model_s *s_models[CSQC_MAX_MODELS];
static char s_modelnames[CSQC_MAX_MODELS][MAX_QPATH];
static int s_nmodels;

int CSQC_Client_ModelIndexKnown(const char *name)
{
	int i;
	if (!name || !name[0]) {
		return 0;
	}
	// csqc layer first
	for (i = 0; i < s_nmodels; i++) {
		if (!strcmp(s_modelnames[i], name)) {
			return -(i + 1);
		}
	}
	// server precache (positive)
	for (i = 1; i < MAX_MODELS; i++) {
		if (!cl.model_name[i][0]) {
			break;
		}
		if (!strcmp(cl.model_name[i], name)) {
			return i;
		}
	}
	return 0;
}

int CSQC_Client_ModelIndex(const char *name)
{
	struct model_s *m;
	int idx = CSQC_Client_ModelIndexKnown(name);
	if (idx || !name || !name[0]) {
		return idx;
	}
	if (s_nmodels >= CSQC_MAX_MODELS) {
		return 0;
	}
	// Registered even if the file is missing (NULL placeholder, see above), so
	// "!= 0" means "registered", not "loaded". Tolerant: a found non-model file
	// becomes a stub instead of Host_Error.
	m = Mod_ForNameTolerant(name, false);
	strlcpy(s_modelnames[s_nmodels], name, MAX_QPATH);
	s_models[s_nmodels] = m;
	return -(++s_nmodels);
}

struct model_s *CSQC_Client_ModelForIndex(int idx)
{
	if (idx > 0 && idx < MAX_MODELS) {
		return cl.model_precache[idx];
	}
	if (idx < 0 && -idx <= s_nmodels) {
		return s_models[-idx - 1];
	}
	return NULL;
}

/* #334 modelnameforindex: reverse lookup across both index spaces. */
const char *CSQC_Client_ModelNameForIndex(int idx)
{
	if (idx > 0 && idx < MAX_MODELS && cl.model_name[idx][0]) {
		return cl.model_name[idx];
	}
	if (idx < 0 && -idx <= s_nmodels) {
		return s_modelnames[-idx - 1];
	}
	return NULL;
}

void CSQC_Client_ModelReset(void)
{
	memset(s_modelnames, 0, sizeof(s_modelnames));
	memset(s_models, 0, sizeof(s_models));
	s_nmodels = 0;
	CSQC_Client_PicSizeReset(); // #318 size cache (per module load)
}

/*
=================
CSQC_Client_ModelDownloadFinished

Called from CL_FinishDownload after a successful download. `downloadname` is
cls.downloadname (="<gamedir>/<file>"), so the gamedir prefix is stripped and the
rest matched against the CSQC model registry. A matching slot is (re)loaded via
Mod_ForName: a NULL placeholder becomes the loaded model, so its stable index turns
render-usable without the module re-calling precache_model. A no-op when the registry
is empty (no CSQC module / nothing precached).
=================
*/
void CSQC_Client_ModelDownloadFinished(const char *downloadname)
{
	const char *raw;
	int i, prefix;

	if (!downloadname || !downloadname[0]) {
		return;
	}

	// The file behind a cached image size may just have been replaced.
	CSQC_Client_PicSizeReset();

	prefix = (int)strlen(cls.gamedir);
	if (prefix <= 0 || strncmp(downloadname, cls.gamedir, prefix) || downloadname[prefix] != '/') {
		return;
	}
	raw = downloadname + prefix + 1;

	// csqc-only registry (negative index space)
	for (i = 0; i < s_nmodels; i++) {
		if (strcmp(s_modelnames[i], raw)) {
			continue;
		}
		s_models[i] = Mod_ForNameTolerant(s_modelnames[i], false);
	}

	// Server precache (#77 / svc_precache): refill cl.model_precache for a matching
	// server name (a missing file was left as a non-NULL stub placeholder).
	for (i = 1; i < MAX_MODELS; i++) {
		if (!cl.model_name[i][0]) {
			continue;
		}
		if (!strcmp(cl.model_name[i], raw)) {
			cl.model_precache[i] = Mod_ForNameTolerant(cl.model_name[i], false);
		}
	}
}

/*
=================
CSQC_Client_ValidateFile

Validates a local csprogs file against the server keys: size == *csprogssize and
(if *csprogs is set) Com_BlockChecksum == crc (the same md4 as mvdsv's
Com_BlockChecksum).
=================
*/
static qbool CSQC_Client_ValidateData(byte *data, int filesize, int size, unsigned crc)
{
	if (!data) {
		return false;
	}
	if (size > 0 && filesize != size) {
		return false;
	}
	if (crc && Com_BlockChecksum(data, filesize) != crc) {
		return false;
	}
	return true;
}

static qbool CSQC_Client_ValidateFile(const char *path, int size, unsigned crc)
{
	byte *data;
	int filesize;
	qbool ok;

	if (!path || !path[0]) {
		return false;
	}
	// Heap buffer + Q_free, not the low hunk. ValidateFile is also called every
	// frame in CSQC_Client_Update while a download is in progress; Hunk_AllocName
	// would accumulate csprogs copies until the next map.
	data = (byte *)FS_LoadHeapFile(path, &filesize);
	ok = CSQC_Client_ValidateData(data, filesize, size, crc);
	Q_free(data);
	return ok;
}

// Reads an absolute OS path (cls.downloadtempname): the FS search path builds
// paths from com_homedir, which is not the root CL_CheckOrDownloadFile wrote to
// (cls.gamedir), so the generic loader must not be used here.
static qbool CSQC_Client_ValidateTmp(const char *path, int size, unsigned crc)
{
	FILE *f;
	long len;
	byte *data;
	qbool ok;

	if (!path || !path[0]) {
		return false;
	}
	f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return false;
	}
	data = (byte *)Q_malloc((size_t)len + 1);
	ok = (fread(data, 1, (size_t)len, f) == (size_t)len);
	fclose(f);
	if (ok) {
		ok = CSQC_Client_ValidateData(data, (int)len, size, crc);
	}
	Q_free(data);
	return ok;
}

/*
=================
CSQC_Client_ValidateDownload

Called by CL_FinishDownload before publishing the downloaded file. Only the
csprogs transfer is checked; all other downloads keep the engine's normal rename.
A failed check removes the temp file and tells the server we have no module, so a
poisoned crc slot is never published.
=================
*/
qbool CSQC_Client_ValidateDownload(const char *tmp, const char *final)
{
	if (!s_dl.csprogs_dl_pending || !final || !final[0] || strcmp(final, s_dl.csprogs_dl_localname)) {
		return true;
	}
	if (!s_csqc.csprogs_crc) {
		return true; // anycsqc: the server keys are not authoritative
	}

	if (CSQC_Client_ValidateTmp(tmp, s_csqc.csprogs_size, s_csqc.csprogs_crc)) {
		return true;
	}

	Con_Printf("CSQC: downloaded csprogs failed validation, discarding\n");
	remove(tmp);
	s_dl.csprogs_dl_pending = false;
	s_dl.csprogs_dl_requested = false;
	CSQC_Client_NotifyCSQC(false);
	return false;
}

/*
=================
CSQC_Client_FindMainProgs

Find a valid local csprogs: 1) the csprogsvers/<crc>.dat cache, 2) *csprogsname
(+ fallback to csprogs.dat). When a valid name-file is found and crc is set, write
a copy into the csprogsvers/<crc>.dat cache so later connects take the cache
instead of re-downloading. Returns true and fills pathbuf with the path for
CSQC_Client_Load.

anycsqc: promiscuous mode - do not check size/crc of the local candidate (server
with anycsqc/corrupt *csprogs, or demoplayback); no write-back to the crc cache.
=================
*/
static qbool CSQC_Client_FindMainProgs(char *pathbuf, size_t bufsz, const char *name, int sizep, unsigned crc, qbool anycsqc)
{
	extern void Sys_mkdir(const char *path);
	char buf[MAX_QPATH];
	const char *cands[3];
	int nc = 0;
	int i;

	extern int FS_Remove(char *fname, int relativeto);

	// *csprogsname is server-controlled and must not steer a filesystem lookup:
	// accept only a plain basename (no separators, drive or ".." traversal);
	// Sys_PathProtection only rewrites backslashes.
	if (name && (strchr(name, '/') || strchr(name, '\\') || strchr(name, ':') || !strcmp(name, "..") || strstr(name, ".."))) {
		name = NULL;
	}

	if (crc) {
		snprintf(buf, sizeof(buf), "csprogsvers/%x.dat", crc);
		if (CSQC_Client_ValidateFile(buf, sizep, crc)) {
			strlcpy(pathbuf, buf, bufsz);
			return true;
		}
		// The engine renames the .tmp into the crc-keyed cache before this check
		// (CL_FinishDownload); delete a poisoned entry so a later legitimate server
		// using the same crc is not forced to re-download forever.
		FS_Remove(buf, FS_GAME_OS);
	}

	if (name && name[0]) {
		cands[nc++] = name;
	}
	if (!name || !name[0] || strcmp(name, "csprogs.dat")) {
		cands[nc++] = "csprogs.dat";
	}

	for (i = 0; i < nc; i++) {
		byte *data;
		int len;

		// One heap buffer serves both validation and write-back.
		data = (byte *)FS_LoadHeapFile(cands[i], &len);
		if (!CSQC_Client_ValidateData(data, len, anycsqc ? 0 : sizep, anycsqc ? 0 : crc)) {
			Q_free(data);
			continue;
		}

		strlcpy(pathbuf, cands[i], bufsz);
		// Write-back into the crc cache. With anycsqc/demo the crc is not
		// confirmed - do not write the cache.
		if (crc && !anycsqc && !cls.demoplayback) {
			char dest[MAX_OSPATH], dir[MAX_OSPATH];
			char *slash;
			FILE *f;
			snprintf(dest, sizeof(dest), "%s/csprogsvers/%x.dat", cls.gamedir, crc);
			strlcpy(dir, dest, sizeof(dir));
			slash = strrchr(dir, '/');
			if (slash && slash != dir) {
				*slash = 0;
				Sys_mkdir(dir);
			}
			f = fopen(dest, "wb");
			if (f) {
				fwrite(data, 1, len, f);
				fclose(f);
				Con_Printf("CSQC: cached csprogsvers/%x.dat\n", crc);
			}
		}
		Q_free(data);
		return true;
	}
	return false;
}

/*
=================
CSQC_Client_StartDownload

Requests the csprogs download from the server under the fixed remote name
"csprogs.dat", saving it into the crc-keyed cache csprogsvers/<crc>.dat so
different servers do not overwrite each other. CL_CheckOrDownloadFile cannot
separate the remote and local names, so its startup steps are repeated here with
a different local path.
=================
*/
static qbool CSQC_Client_StartDownload(const char *remote, const char *localrel)
{
	extern void Sys_mkdir(const char *path);
	char dir[MAX_OSPATH];
	char *slash;

	// Do NOT clobber a download already in flight (manual "download", QTV, an
	// engine signon request whose first block has not arrived yet, or a module
	// request): the shared cls.downloadname/tempname belong to the running
	// transfer and the completion rename (CL_FinishDownload) would move that file
	// into our crc cache. The caller defers and retries once the slot is free
	// (CSQC_Client_Update).
	if (cls.state < ca_connected || cls.demoplayback || cls.download || cls.downloadrequested || s_dl.dl_inflight) {
		return false;
	}

	snprintf(cls.downloadname, sizeof(cls.downloadname), "%s/%s", cls.gamedir, localrel);
	// Our own transfer must not resume the signon download list on completion.
	cls.downloadtype = dl_single;
	cls.downloadmethod = DL_QW;
	cls.downloadstarttime = Sys_DoubleTime();
	COM_StripExtension(cls.downloadname, cls.downloadtempname, sizeof(cls.downloadtempname));
	strlcat(cls.downloadtempname, ".tmp", sizeof(cls.downloadtempname));

	// the destination directory (e.g. csprogsvers/) must exist
	strlcpy(dir, cls.downloadname, sizeof(dir));
	slash = strrchr(dir, '/');
	if (slash && slash != dir) {
		*slash = 0;
		Sys_mkdir(dir);
	}

	Com_Printf("CSQC: downloading %s -> %s\n", remote, localrel);
	MSG_WriteByte(&cls.netchan.message, clc_stringcmd);
	MSG_WriteString(&cls.netchan.message, va("download \"%s\"", remote));
	cls.downloadnumber++;
	// The request is out but no block has arrived yet: the slot stays occupied
	// until CL_FinishDownload clears the flag.
	cls.downloadrequested = true;
	// Start the no-progress window and remember our downloadname (cls.download is
	// shared by all downloads - count progress only against our own file).
	s_dl.csprogs_dl_lastprogress = Sys_DoubleTime();
	s_dl.csprogs_dl_percent = 0;
	s_dl.csprogs_dl_started = false;
	s_dl.csprogs_dl_requested = true;
	strlcpy(s_dl.csprogs_dl_localname, cls.downloadname, sizeof(s_dl.csprogs_dl_localname));

	return true;
}

/*
=================
CSQC_Client_DlPath

Local path of a downloaded csprogs: always the crc-keyed cache file
"csprogsvers/<crc>.dat". The server's *csprogsname is not an
input, so it can never steer the write outside <gamedir>/csprogsvers/.
=================
*/
static void CSQC_Client_DlPath(char *buf, size_t bufsz, unsigned crc)
{
	snprintf(buf, bufsz, "csprogsvers/%x.dat", crc);
}

/*
=================
CSQC_Client_CsprogsDlValid

A csprogs download is only allowed when the server's *csprogs value is non-empty
and fully numeric. endptr must be the strtoul end pointer for csprogs (the caller
parses once in ConnectCheck).
=================
*/
static qbool CSQC_Client_CsprogsDlValid(const char *csprogs, const char *endptr)
{
	return csprogs[0] != '\0' && *endptr == '\0';
}

// A transfer occupying the single download slot. Queue depth is deliberately not
// part of this predicate: the queue drain checks it to decide whether to start
// the next request, and including it would deadlock the drain.
static qbool CSQC_Client_TransferActive(void)
{
	return cls.download != NULL || cls.downloadrequested || s_dl.dl_inflight || s_dl.csprogs_dl_pending;
}

qbool CSQC_Client_DownloadInProgress(void)
{
	return CSQC_Client_TransferActive() || s_dl_queue_count > 0;
}

void CSQC_Client_DownloadFinished(const char *finished)
{
	// Our csprogs transfer ended (completed, or refused by the server before any
	// data arrived): CSQC_Client_SignonFrame then loads the file or gives up
	// right away instead of waiting out the no-progress window.
	if (s_dl.csprogs_dl_pending && s_dl.csprogs_dl_requested && finished && !strcmp(finished, s_dl.csprogs_dl_localname)) {
		s_dl.csprogs_dl_started = true;
	}

	if (!s_dl.dl_inflight) {
		return;
	}
	// A foreign transfer (manual "download", signon list) may complete while our
	// request is still pending on the server; only clear for our own file.
	if (finished && strcmp(finished, s_dl.dl_inflight_localname)) {
		return;
	}
	s_dl.dl_inflight = false;
	s_dl.dl_inflight_time = 0;
	s_dl.dl_inflight_localname[0] = 0;
}

/*
=================
CSQC_Client_RequestDownload

Single entry point for a module-initiated download. A module transfer must use
dl_single: with dl_model/dl_sound the engine's completion path runs
Model_NextDownload/Sound_NextDownload, which end in CL_Prespawn and re-send the
spawn mid-game.
Returns 1 when the file is present (or downloading is unavailable), 0 when the
request was queued or sent.
=================
*/
int CSQC_Client_RequestDownload(const char *name)
{
	if (!name || !name[0]) {
		return 1;
	}

	// An already-present file must not occupy a queue slot behind an active
	// transfer.
	if (!CL_Download_Accept(name)) {
		return 1;
	}

	if (CSQC_Client_TransferActive()) {
		CSQC_Client_QueueDownload(name);
		return 0;
	}
	if (cls.state < ca_connected) {
		return 1;
	}
	// Demo playback has no downloads, except QTV playback that negotiated the
	// ezquake download extension (CL_CheckOrDownloadFile handles that branch).
	if (cls.demoplayback && !(cls.mvdplayback == QTV_PLAYBACK && (cls.qtv_ezquake_ext & QTV_EZQUAKE_EXT_DOWNLOAD))) {
		return 1;
	}

	if (CL_CheckOrDownloadFile((char *)name)) {
		return 1;
	}

	// Recorded only for a transfer that actually started; CL_FinishDownload uses
	// the name to tell our completion from a foreign one.
	cls.downloadtype = dl_single;
	s_dl.dl_inflight = true;
	s_dl.dl_inflight_time = Sys_DoubleTime();
	strlcpy(s_dl.dl_inflight_localname, cls.downloadname, sizeof(s_dl.dl_inflight_localname));
	return 0;
}

void CSQC_Client_QueueDownload(const char *name)
{
	if (!name || !name[0]) {
		return;
	}
	if (s_dl_queue_count >= CSQC_DL_QUEUE_MAX) {
		Con_DPrintf("CSQC: download queue full, dropping %s\n", name);
		return;
	}
	strlcpy(s_dl_queue[(s_dl_queue_head + s_dl_queue_count) % CSQC_DL_QUEUE_MAX], name, MAX_QPATH);
	s_dl_queue_count++;
}

void CSQC_Client_FlushDownloadQueue(void)
{
	char name[MAX_QPATH];

	// A request sent but never answered (server refused, lost datagram) must not
	// wedge the queue forever: the engine only clears cls.download on completion.
	if (s_dl.dl_inflight && !cls.download && Sys_DoubleTime() - s_dl.dl_inflight_time > CSQC_DL_INFLIGHT_TIMEOUT) {
		cls.downloadrequested = false;
		CSQC_Client_DownloadFinished(NULL);
	}

	if (CSQC_Client_TransferActive() || s_dl_queue_count <= 0) {
		return;
	}
	if (cls.state < ca_connected || cls.demoplayback) {
		return;
	}

	strlcpy(name, s_dl_queue[s_dl_queue_head], sizeof(name));
	s_dl_queue_head = (s_dl_queue_head + 1) % CSQC_DL_QUEUE_MAX;
	s_dl_queue_count--;

	CSQC_Client_RequestDownload(name);
}

/*
=================
CSQC_Client_FreeArena / AllocArena

Client edict arena (PR1 entity value = slot*edict_size). Q_malloc (not hunk);
freed in Disconnect and at the start of Load.
=================
*/
static void CSQC_Client_FreeArena(void)
{
	if (s_csqc.edicts) {
		Q_free(s_csqc.edicts);
		s_csqc.edicts = NULL;
	}
	if (s_csqc.game_edicts) {
		Q_free(s_csqc.game_edicts);
		s_csqc.game_edicts = NULL;
	}
}

static void CSQC_Client_AllocArena(pr1vm_t *vm)
{
	int i;

	CSQC_Client_FreeArena();
	if (!vm || vm->edict_size <= 0) {
		return;
	}

	memset(s_used, 0, sizeof(s_used));
	memset(s_own, 0, sizeof(s_own));
	memset(s_numslot, 0, sizeof(s_numslot));
	memset(s_slotnum, 0, sizeof(s_slotnum));
	memset(s_freetime, 0, sizeof(s_freetime));
	memset(s_allocseq, 0, sizeof(s_allocseq));

	s_csqc.game_edicts = (byte *)Q_malloc((size_t)CSQC_MAX_EDICTS * vm->edict_size);
	s_csqc.edicts = (edict_t *)Q_malloc(sizeof(edict_t) * CSQC_MAX_EDICTS);
	memset(s_csqc.game_edicts, 0, (size_t)CSQC_MAX_EDICTS * vm->edict_size);
	memset(s_csqc.edicts, 0, sizeof(edict_t) * CSQC_MAX_EDICTS);
	for (i = 0; i < CSQC_MAX_EDICTS; i++) {
		s_csqc.edicts[i].v = (entvars_t *)(s_csqc.game_edicts + (size_t)i * vm->edict_size);
	}

	vm->edicts = s_csqc.edicts;
	vm->game_edicts = s_csqc.game_edicts;
	vm->num_edicts = CSQC_MAX_EDICTS;
	vm->max_edicts = CSQC_MAX_EDICTS;
	vm->state = 0; // client instance; world writes are gated by world_writable
	vm->fieldofs_patch = NULL; // csprogs uses raw field offsets.
}

/*
=================
CSQC_Client_StringGC

Free temp strings nothing refers to. Installed as vm->host_postcall, so it runs
only when an outermost VM call returns (depth 0): the module's locals live in the
globals area and no builtin is mid-flight, so the globals and the edict arena are
the complete root set. Conservative: every 32-bit word is treated as a possible
reference, and a word that only looks
like one just keeps that string alive (negative floats decode to indices far past
the table). Slots that were ever allocated are scanned, freed ones included: a
stale entity reference may still read their fields until the slot is reused.
Runs only once the live count exceeds twice the survivors of the previous pass
(at least CSQC_TEMP_GC_MIN), so the scan is rare.
=================
*/
static void CSQC_Client_MarkWords(csqc_strpool_t *pool, const int *w, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		unsigned int idx;

		if (w[i] > -CSQC_TEMP_BASE || w[i] == INT_MIN) {
			continue;
		}
		idx = (unsigned int)(-(w[i] + CSQC_TEMP_BASE));
		if (idx < (unsigned int)pool->tmphigh) {
			pool->tmpmark[idx] = 1;
		}
	}
}

// Free every unmarked temp string and set the next collection threshold.
static void CSQC_Client_SweepTemp(csqc_strpool_t *pool)
{
	int i;

	for (i = 0; i < pool->tmphigh; i++) {
		if (!pool->tmp[i] || pool->tmpmark[i]) {
			continue;
		}
		Q_free(pool->tmp[i]);
		pool->tmpfree[pool->numtmpfree++] = i;
		pool->numtmp--;
	}
	pool->gc_threshold = 2 * pool->numtmp;
}

static void CSQC_Client_StringGC(pr1vm_t *vm)
{
	csqc_strpool_t *pool;
	int i;

	if (!vm || vm != &s_csqc.vm || vm->depth != 0) {
		return;
	}
	pool = (csqc_strpool_t *)vm->host_udata;
	if (!pool || pool->numtmp <= max(CSQC_TEMP_GC_MIN, pool->gc_threshold)) {
		return;
	}

	memset(pool->tmpmark, 0, (size_t)pool->tmphigh);
	if (vm->globals && vm->progs) {
		CSQC_Client_MarkWords(pool, (const int *)vm->globals, (size_t)vm->progs->numglobals + 2);
	}
	if (s_csqc.game_edicts && vm->edict_size > 0) {
		size_t words = (size_t)vm->edict_size / sizeof(int);

		for (i = 0; i < CSQC_MAX_EDICTS; i++) {
			if (i != 0 && !s_allocseq[i]) {
				continue; // never allocated: still zero-filled
			}
			CSQC_Client_MarkWords(pool, (const int *)(s_csqc.game_edicts + (size_t)i * vm->edict_size), words);
		}
	}

	CSQC_Client_SweepTemp(pool);
}

/*
=================
CSQC_Client_FindField

Finds a module field by name in fielddefs (see PR1VM_FindFunction). Returns the
field offset in float words from the start of entvars (ddef_t.ofs) or -1.
=================
*/
int CSQC_Client_FindField(pr1vm_t *vm, const char *name)
{
	int i;

	if (!vm || !vm->fielddefs || !name) {
		return -1;
	}
	for (i = 0; i < vm->progs->numfielddefs; i++) {
		const char *s = CSQC_Client_GetString(vm, vm->fielddefs[i].s_name);
		if (s && s[0] && !strcmp(s, name)) {
			return vm->fielddefs[i].ofs;
		}
	}
	return -1;
}

/*
=================
CSQC_Client_FindFieldVec / CSQC_Client_FindGlobalVec

Type-aware lookups for the defs the engine writes as vectors. A hostile module can
declare such a name with a smaller width (float), so the engine would write three
words through a one-word def and corrupt the neighbouring globals/fields. The
validator bounds a def by its declared width only, and a downloaded v6 lump
cannot be padded, so the engine refuses a non-vector def for its own writes.
Returns the offset or -1.
=================
*/
static int CSQC_Client_FindFieldVec(pr1vm_t *vm, const char *name)
{
	int i;

	if (!vm || !vm->fielddefs || !name) {
		return -1;
	}
	for (i = 0; i < vm->progs->numfielddefs; i++) {
		const char *s = CSQC_Client_GetString(vm, vm->fielddefs[i].s_name);
		if (s && s[0] && !strcmp(s, name)) {
			int type = (int)vm->fielddefs[i].type & ~DEF_SAVEGLOBAL;
			return (type == ev_vector) ? vm->fielddefs[i].ofs : -1;
		}
	}
	return -1;
}

static int CSQC_Client_FindGlobalVec(pr1vm_t *vm, const char *name)
{
	int i;

	if (!vm || !vm->globaldefs || !name) {
		return -1;
	}
	for (i = 0; i < vm->progs->numglobaldefs; i++) {
		const char *s = CSQC_Client_GetString(vm, vm->globaldefs[i].s_name);
		if (s && s[0] && !strcmp(s, name)) {
			int type = (int)vm->globaldefs[i].type & ~DEF_SAVEGLOBAL;
			return (type == ev_vector) ? vm->globaldefs[i].ofs : -1;
		}
	}
	return -1;
}

/*
=================
Hot-path offset cache

The trace globals and entity fields read every frame (CSQC_Store_Trace,
CSQC_Add_One_Entity, CSQC_Addentities) are resolved once at module load
(CSQC_Client_OffsetCacheResolve) instead of a linear scan of globaldefs/fielddefs
per call.
=================
*/
static const char *s_traceg_names[CSQC_TRACEG_COUNT] = {
	"trace_fraction", "trace_allsolid", "trace_startsolid", "trace_inopen",
	"trace_inwater", "trace_plane_dist", "trace_endpos", "trace_plane_normal",
	"trace_ent"
};

// Designated initializers: the table is indexed by csqc_field_id_t, so an entry
// can not silently shift the ones after it.
static const char *s_fieldcache_names[CSQC_FLD_COUNT] = {
	[CSQC_FLD_PREDRAW] = "predraw",
	[CSQC_FLD_MODELINDEX] = "modelindex",
	[CSQC_FLD_MODEL] = "model",
	[CSQC_FLD_COLORMAP] = "colormap",
	[CSQC_FLD_ORIGIN] = "origin",
	[CSQC_FLD_ANGLES] = "angles",
	[CSQC_FLD_FRAME] = "frame",
	[CSQC_FLD_FRAME2] = "frame2",
	[CSQC_FLD_LERPFRAC] = "lerpfrac",
	[CSQC_FLD_SKIN] = "skin",
	[CSQC_FLD_EFFECTS] = "effects",
	[CSQC_FLD_ALPHA] = "alpha",
	[CSQC_FLD_SCALE] = "scale",
	[CSQC_FLD_RENDERFLAGS] = "renderflags",
	[CSQC_FLD_SIZE] = "size",
	[CSQC_FLD_MINS] = "mins",
	[CSQC_FLD_MAXS] = "maxs",
	[CSQC_FLD_MODELFLAGS] = "modelflags",
	[CSQC_FLD_CHAIN] = "chain",
	[CSQC_FLD_SOLID] = "solid",
	[CSQC_FLD_FLAGS] = "flags",
	[CSQC_FLD_OWNER] = "owner",
	[CSQC_FLD_DRAWMASK] = "drawmask",
	[CSQC_FLD_ABSMIN] = "absmin",
	[CSQC_FLD_ABSMAX] = "absmax",
	[CSQC_FLD_ENTNUM] = "entnum",
	[CSQC_FLD_GROUNDENTITY] = "groundentity",
	[CSQC_FLD_ENEMY] = "enemy",
	[CSQC_FLD_IDEALPITCH] = "idealpitch",
	[CSQC_FLD_PITCH_SPEED] = "pitch_speed",
};

static int s_traceg[CSQC_TRACEG_COUNT];
static int s_fieldcache[CSQC_FLD_COUNT];

static void CSQC_Client_OffsetCacheReset(void)
{
	int i;

	for (i = 0; i < CSQC_TRACEG_COUNT; i++) {
		s_traceg[i] = -1;
	}
	for (i = 0; i < CSQC_FLD_COUNT; i++) {
		s_fieldcache[i] = -1;
	}
}

static void CSQC_Client_OffsetCacheResolve(pr1vm_t *vm)
{
	int i;

	for (i = 0; i < CSQC_TRACEG_COUNT; i++) {
		s_traceg[i] = PR1VM_FindGlobal(vm, s_traceg_names[i]);
	}
	for (i = 0; i < CSQC_FLD_COUNT; i++) {
		s_fieldcache[i] = CSQC_Client_FindField(vm, s_fieldcache_names[i]);
	}
	// Written only by the engine: refuse a non-vector declaration.
	s_fieldcache[CSQC_FLD_ABSMIN] = CSQC_Client_FindFieldVec(vm, "absmin");
	s_fieldcache[CSQC_FLD_ABSMAX] = CSQC_Client_FindFieldVec(vm, "absmax");
}

int CSQC_Client_TraceGlobal(pr1vm_t *vm, int id)
{
	(void)vm;
	return (id >= 0 && id < CSQC_TRACEG_COUNT) ? s_traceg[id] : -1;
}

int CSQC_Client_FieldOfs(pr1vm_t *vm, int id)
{
	(void)vm;
	return (id >= 0 && id < CSQC_FLD_COUNT) ? s_fieldcache[id] : -1;
}

// The self global offset, resolved at module load.
int CSQC_Client_SelfGlobal(void)
{
	return s_csqc.global_self;
}

/*
=================
CSQC_Client_SetContextSlot

Sets the entity context for CSQC_Ent_Update/Remove: self = the slot and the slot's
.entnum = the server number (module-spawned entities have none; it stays 0).
=================
*/
static void CSQC_Client_SetContextSlot(pr1vm_t *vm, unsigned slot, unsigned number)
{
	float *s;

	if (!vm || !vm->game_edicts) {
		return;
	}
	if (s_csqc.global_self >= 0) {
		*(int *)&vm->globals[s_csqc.global_self] = (int)slot * vm->edict_size;
	}
	if (s_csqc.field_entnum >= 0 && slot < CSQC_MAX_EDICTS) {
		s = (float *)((byte *)vm->game_edicts + (size_t)slot * vm->edict_size + s_csqc.field_entnum * 4);
		s[0] = (float)number;
	}
}

/*
=================
CSQC_Client_EntityEntNum

ssqc number of the touched entity - the arena edict's `.entnum`: the server number
for network entities, 0 for module-spawned ones. Feeds trace_networkentity.
Out of range / no field - 0.
=================
*/
int CSQC_Client_EntityEntNum(pr1vm_t *vm, int slot)
{
	float *s;

	if (!vm || !vm->game_edicts || s_csqc.field_entnum < 0) {
		return 0;
	}
	if (slot <= 0 || slot >= CSQC_MAX_EDICTS || slot >= vm->num_edicts) {
		return 0;
	}
	s = (float *)((byte *)vm->game_edicts + (size_t)slot * vm->edict_size + s_csqc.field_entnum * 4);
	return (int)s[0];
}

/*
=================
CSQC_Client_RunEntSpawn

Hook for a new CSQC entity. The engine sets self = world, puts the server number
into PARM0 and calls CSQC_Ent_Spawn; the module creates the entity (usually
spawn(); self.entnum = entnum) and returns it in self. Returns a valid occupied
slot or 0 (world/invalid; no fallback).
=================
*/
static int CSQC_Client_RunEntSpawn(pr1vm_t *vm, unsigned int entnum)
{
	int selfval, slot;

	if (!vm || s_csqc.func_entspawn <= 0 || s_csqc.global_self < 0 || vm->edict_size <= 0) {
		return 0;
	}
	*(int *)&vm->globals[s_csqc.global_self] = 0;
	vm->globals[OFS_PARM0] = (float)entnum;
	if (!CSQC_Client_Exec(s_csqc.func_entspawn)) {
		return 0;
	}
	selfval = *(int *)&vm->globals[s_csqc.global_self];
	slot = CSQC_Client_EntNum(vm, selfval);
	return (slot > 0 && slot < CSQC_MAX_EDICTS && s_used[slot]) ? slot : 0;
}

static void CSQC_Client_ClearSlotBridges(int slot);

/* After CSQC_Ent_Update the module may change self; remap number->slot onto the new
   valid slot (0 = world/removed).
   The entity the number used to point at remains an ordinary module entity: when
   the mapping moves, the old slot is handed to the module (s_own, reverse map
   cleared) - not freed, the module may still reference it - otherwise it would
   stay engine-owned with a stale s_slotnum and neither side could ever free it.
   When self is world/invalid the number is unmapped, so the next update starts a
   fresh entity.
   ctxslot/ctxseq: the slot self was set to before the callback and its allocation
   sequence. If self is unchanged but the slot was freed and reallocated during the
   callback (remove(self) then spawn() inside the reuse-delay exemptions, see
   CSQC_Client_AllocSlot), self now names an unrelated entity: treat it as removed
   rather than mapping the number onto it. An unchanged self keeps the mapping even
   when the module freed that slot with remove() (see CSQC_Client_EntFree). */
static void CSQC_Client_RemapAfterUpdate(pr1vm_t *vm, unsigned int entnum, int ctxslot, unsigned int ctxseq)
{
	int selfval, slot, old;

	if (!vm || s_csqc.func_entspawn <= 0 || s_csqc.global_self < 0 || vm->edict_size <= 0) {
		return;
	}
	selfval = *(int *)&vm->globals[s_csqc.global_self];
	slot = CSQC_Client_EntNum(vm, selfval);
	old = CSQC_Client_NumToSlot((int)entnum);
	if (slot > 0 && slot == ctxslot && s_allocseq[slot] != ctxseq) {
		slot = 0; // freed and reallocated during the callback
	}
	else if (slot == old) {
		return;  // unchanged (possibly freed but still mapped)
	}
	if (slot < 0 || slot >= CSQC_MAX_EDICTS || !s_used[slot]) {
		slot = 0;
	}
	if (slot == old) {
		return;
	}
	if (old > 0 && old < CSQC_MAX_EDICTS && s_slotnum[old] == (int)entnum) {
		s_slotnum[old] = 0;
		if (s_used[old]) {
			s_own[old] = true;
		}
	}
	CSQC_Client_MapNumber((int)entnum, slot);
}

/*
=================
CSQC_Client_EntAlloc / EntFree (entity pool)

Module entities (builtin spawn) take a free pool slot (first free from 1) and are
marked s_own (their remove() also drops any number mapping). Network slots are
allocated from the same pool (without s_own) and held by the number->slot map in
ParseEntities. PR1 entity value = slot*edict_size.

Reuse is delayed: a slot freed less than 0.5 s ago is skipped, except slots freed
while the module time was < 2 s (map start churn) and instant frees (freetime 0);
if nothing else is free, recently freed slots are used anyway. This keeps a stale
entity reference held by the module from silently aliasing a brand new entity. A
full pool aborts the module on the spawn path; the network path gets 0 and drops
the entity.
=================
*/
static int CSQC_Client_AllocSlot(pr1vm_t *vm)
{
	double now = CSQC_Client_TimeNow();
	int i, pass;

	for (pass = 0; pass < 2; pass++) {
		for (i = 1; i < CSQC_MAX_EDICTS; i++) {
			if (s_used[i]) {
				continue;
			}
			// first pass: honour the reuse delay; second pass: any free slot
			if (!pass && !(s_freetime[i] < 2 || now - s_freetime[i] > 0.5)) {
				continue;
			}
			// A freed network slot keeps its number mapping (see EntFree); drop it
			// now so the old number gets a fresh entity (isnew = 1) on its next
			// update instead of aliasing this one.
			if (s_slotnum[i]) {
				if (s_slotnum[i] > 0 && s_slotnum[i] < CSQC_MAX_NUM && s_numslot[s_slotnum[i]] == i) {
					s_numslot[s_slotnum[i]] = 0;
				}
				s_slotnum[i] = 0;
				CSQC_Client_ClearSlotBridges(i);
			}
			s_used[i] = true;
			s_own[i] = false;
			s_allocseq[i]++;
			// Zero the slot fields on (re)use, or the module sees remnants of a
			// previous entity.
			if (vm && vm->game_edicts && vm->edict_size > 0) {
				memset((byte *)vm->game_edicts + (size_t)i * vm->edict_size, 0, vm->edict_size);
			}
			return i;
		}
	}
	Con_Printf("CSQC_Client_AllocSlot: pool full (%d)\n", CSQC_MAX_EDICTS - 1);
	return 0;
}

int CSQC_Client_EntAlloc(struct pr1vm_s *v)
{
	pr1vm_t *vm = (pr1vm_t *)v;
	int slot;
	slot = CSQC_Client_AllocSlot(vm);
	if (!slot) {
		// Never hand world to the module (it would write into edict 0). Abort
		// does not return into the VM.
		CSQC_Client_Abort(va("ED_Alloc: no free edicts (max is %i)", CSQC_MAX_EDICTS));
		return 0;
	}
	s_own[slot] = true; // spawn entity: .entnum not written (0)
	return slot;
}

void CSQC_Client_EntFree(struct pr1vm_s *v, int entnum)
{
	pr1vm_t *vm = (pr1vm_t *)v;
	float *s;

	if (!vm || !vm->game_edicts) {
		return;
	}
	if (entnum <= 0 || entnum >= CSQC_MAX_EDICTS || !s_used[entnum]) {
		return;
	}
	if (!s_own[entnum]) {
		// Engine-owned network entity: free the slot (not drawn, not found,
		// wasfreed true) but keep the number->slot and bridge mappings, so the
		// server's next update for the number runs CSQC_Ent_Update on the freed
		// entity with isnew = false and it stays invisible (writes to free
		// entities are allowed). AllocSlot drops the mappings if the slot is
		// reused. Only the drawing/think/collision fields below are cleared.
		int ofs[8], i;

		s_used[entnum] = false;
		s_freetime[entnum] = CSQC_Client_TimeNow(); // non-instant free
		ofs[0] = CSQC_Client_FieldOfs(vm, CSQC_FLD_SOLID);
		ofs[1] = s_csqc.f_movetype;
		ofs[2] = s_csqc.f_modelindex;
		ofs[3] = s_csqc.f_think;
		ofs[4] = s_csqc.f_nextthink;
		ofs[5] = CSQC_Client_FieldOfs(vm, CSQC_FLD_PREDRAW);
		ofs[6] = s_csqc.f_drawmask;
		ofs[7] = CSQC_Client_FieldOfs(vm, CSQC_FLD_RENDERFLAGS);
		s = (float *)((byte *)vm->game_edicts + (size_t)entnum * vm->edict_size);
		for (i = 0; i < 8; i++) {
			if (ofs[i] >= 0 && ofs[i] * 4 < vm->edict_size) {
				*(int *)&s[ofs[i]] = 0;
			}
		}
		return;
	}
	// Drop the reverse slot->N map, otherwise s_numslot[N] stays valid on the freed
	// slot, which may be reused.
	if (s_slotnum[entnum] > 0 && s_slotnum[entnum] < CSQC_MAX_NUM && s_numslot[s_slotnum[entnum]] == entnum) {
		s_numslot[s_slotnum[entnum]] = 0;
	}
	s_slotnum[entnum] = 0;
	s_used[entnum] = false;
	s_own[entnum] = false;
	s_freetime[entnum] = CSQC_Client_TimeNow(); // non-instant free
	s = (float *)((byte *)vm->game_edicts + (size_t)entnum * vm->edict_size);
	memset(s, 0, vm->edict_size);
	// Symmetric with NetFreeSlot: drop any player/delta bridge referencing the freed
	// slot - otherwise a module `remove()` of a handed-off network entity leaves
	// a stale bridge writing into a reused slot.
	CSQC_Client_ClearSlotBridges(entnum);
}

/* internal network path (ParseEntities): a slot without s_own.
   vm is needed to zero the slot fields. */
int CSQC_Client_NetAllocSlot(struct pr1vm_s *v)
{
	return CSQC_Client_AllocSlot((pr1vm_t *)v);
}

void CSQC_Client_NetFreeSlot(int slot, int number)
{
	if (slot > 0 && slot < CSQC_MAX_EDICTS && s_used[slot]) {
		s_used[slot] = false;
		s_own[slot] = false;
		s_freetime[slot] = CSQC_Client_TimeNow(); // non-instant free
	}
	if (slot > 0 && slot < CSQC_MAX_EDICTS) {
		s_slotnum[slot] = 0; // reverse map does not outlive the free
	}
	if (number > 0 && number < CSQC_MAX_NUM && s_numslot[number] == slot) {
		s_numslot[number] = 0;
	}
	// Drop any player/delta bridge that referenced this slot so a reused slot is
	// not written with player/packet state belonging to a different logical edict.

	CSQC_Client_ClearSlotBridges(slot);
}

/*
=================
CSQC_Client_NetRemove

Shared remove path for a network entity: an svc 76 remove record, or a deltalisten
player/packet entity that left the frame. The number->entity mapping is dropped
first, so a later entity with the same number takes a fresh slot
instead of aliasing this one. A declared CSQC_Ent_Remove then owns the edict and
frees it itself with remove() (s_own is transferred so remove() accepts the slot);
the engine frees the slot only when no callback is declared (or callback is false,
e.g. the module errored mid-message). The caller drops its own bridge mapping
(s_player_slot / s_delta_slot) before calling this.
=================
*/
static void CSQC_Client_NetRemove(pr1vm_t *vm, int slot, int number, qbool callback)
{
	if (slot <= 0 || slot >= CSQC_MAX_EDICTS) {
		return;
	}
	if (number > 0 && number < CSQC_MAX_NUM && s_numslot[number] == slot) {
		s_numslot[number] = 0;
	}
	if (s_slotnum[slot] == number) {
		s_slotnum[slot] = 0;
	}
	if (callback && s_csqc.func_entremove > 0) {
		// A slot the module already freed with remove() (mapped but free, see
		// EntFree) still gets the callback; its remove() is then a no-op.
		if (s_used[slot]) {
			s_own[slot] = true;
		}
		CSQC_Client_SetContextSlot(vm, (unsigned)slot, (unsigned)number);
		CSQC_Client_Exec(s_csqc.func_entremove);
		return;
	}
	CSQC_Client_NetFreeSlot(slot, number);
}

qbool CSQC_Client_EntUsed(int entnum)
{
	return (entnum > 0 && entnum < CSQC_MAX_EDICTS) ? s_used[entnum] : false;
}

/*
 Recompute .absmin/.absmax after an origin/size/model change (the abs-box part of
 linking an edict). SOLID_BSP brush models use the model bounds; rotated brushes get a
 cube of the largest extent. Expanded by 1 (15 horizontally for FL_ITEM) because
 movement clips an epsilon away from edges.
*/
void CSQC_Client_LinkBounds(pr1vm_t *vm, int e)
{
	float *base, *amin, *amax, *org, *mins, *maxs, *ang;
	float solid = 0, flags = 0;
	model_t *mod = NULL;
	int ofs, i;

	if (!vm || !s_csqc.game_edicts || !CSQC_Client_EntUsed(e) || e >= vm->num_edicts) {
		return;
	}
	if (s_fieldcache[CSQC_FLD_ABSMIN] < 0 || s_fieldcache[CSQC_FLD_ABSMAX] < 0 || s_fieldcache[CSQC_FLD_ORIGIN] < 0 || s_fieldcache[CSQC_FLD_MINS] < 0 || s_fieldcache[CSQC_FLD_MAXS] < 0) {
		return;
	}
	base = (float *)(s_csqc.game_edicts + (size_t)e * vm->edict_size);
	amin = base + s_fieldcache[CSQC_FLD_ABSMIN];
	amax = base + s_fieldcache[CSQC_FLD_ABSMAX];
	org = base + s_fieldcache[CSQC_FLD_ORIGIN];
	mins = base + s_fieldcache[CSQC_FLD_MINS];
	maxs = base + s_fieldcache[CSQC_FLD_MAXS];
	ang = (s_fieldcache[CSQC_FLD_ANGLES] >= 0) ? base + s_fieldcache[CSQC_FLD_ANGLES] : NULL;
	if ((ofs = s_fieldcache[CSQC_FLD_SOLID]) >= 0) {
		solid = base[ofs];
	}
	if ((ofs = s_fieldcache[CSQC_FLD_FLAGS]) >= 0) {
		flags = base[ofs];
	}

	if ((int)solid == 4 /* SOLID_BSP */ && (ofs = s_fieldcache[CSQC_FLD_MODELINDEX]) >= 0) {
		mod = CSQC_Client_ModelForIndex((int)base[ofs]);
		if (mod && mod->type == mod_brush) {
			mins = mod->mins;
			maxs = mod->maxs;
		}
	}

	if (((int)solid == 4 || (int)solid == 22 /* SOLID_BSPTRIGGER */) && ang && (ang[0] || ang[1] || ang[2])) {
		float r = 0;

		for (i = 0; i < 3; i++) {
			r = max(r, fabs(mins[i]));
			r = max(r, fabs(maxs[i]));
		}
		for (i = 0; i < 3; i++) {
			amin[i] = org[i] - r;
			amax[i] = org[i] + r;
		}
	}
	else {
		VectorAdd(org, mins, amin);
		VectorAdd(org, maxs, amax);
	}

	if ((int)flags & 256 /* FL_ITEM */) {
		amin[0] -= 15;
		amin[1] -= 15;
		amin[2] -= 1;
		amax[0] += 15;
		amax[1] += 15;
		amax[2] += 1;
	}
	else {
		for (i = 0; i < 3; i++) {
			amin[i] -= 1;
			amax[i] += 1;
		}
	}
}

int CSQC_Client_EntSpawnBase(void)
{
	return 1; // first usable pool slot (0 = world)
}

int CSQC_Client_EntUsedCount(void)
{
	int i, n = 0;
	for (i = 1; i < CSQC_MAX_EDICTS; i++) {
		if (s_used[i]) {
			n++;
		}
	}
	return n;
}

/*
=================
CSQC_Client_EntNum

Convert a raw PR1 entity value (slot*edict_size) into a pool slot. The client
VM executes untrusted csprogs and a builtin entity argument is not dereferenced
by the VM (unlike an opcode pointer), so an out-of-range value is clamped to
world (0) instead of faulting. Same bound as the opcode predicate
PR1VM_ClientBadEdict; on the client instance
num_edicts == max_edicts == CSQC_MAX_EDICTS (see CSQC_Client_AllocArena).
=================
*/
int CSQC_Client_EntNum(struct pr1vm_s *v, int raw)
{
	pr1vm_t *vm = (pr1vm_t *)v;
	int idx;

	if (!vm || vm->edict_size <= 0) {
		return 0;
	}
	idx = raw / vm->edict_size;
	if (raw < 0 || idx >= vm->max_edicts) {
		Con_DPrintf("CSQC: bad entity value %d\n", raw);
		return 0; // world
	}
	return idx;
}

int CSQC_Client_NumToSlot(int number)
{
	return (number > 0 && number < CSQC_MAX_NUM) ? s_numslot[number] : 0;
}

int CSQC_Client_MapNumber(int number, int slot)
{
	if (number > 0 && number < CSQC_MAX_NUM) {
		s_numslot[number] = slot;
	}
	if (slot > 0 && slot < CSQC_MAX_EDICTS) {
		s_slotnum[slot] = (number > 0 && number < CSQC_MAX_NUM) ? number : 0;
	}
	return slot;
}

/*
=================
#371 deltalisten: engine bridge player_state -> arena edict. The bridge gives
the module the authoritative (no-lerp) player state: `self`/`.entnum` = pnum+1, fields
origin/velocity/angles (+modelindex/skin). The module callback is invoked as
CSQC_Ent_Update (PARM0 = isnew) once per new acked frame (cl.parsecount).
=================
*/
static int s_delta_func[MAX_MODELS];
static int s_delta_flags[MAX_MODELS];
// Personal player-bridge mapping (pnum -> arena slot), to distinguish ownership
// from svc76 (CSQC_Client_NumToSlot). num = pnum+1 (server entnum of the player).
static int s_player_slot[MAX_CLIENTS];
// delta-entity bridge: packet entity number -> arena slot + "seen this frame".
static int s_delta_slot[CSQC_MAX_NUM];
static byte s_delta_seen[CSQC_MAX_NUM];
// MASK_DELTA: callback returned !=0 -> the engine does not draw the entity (the module does).
static byte s_delta_player_owned[MAX_CLIENTS];
static byte s_delta_ent_owned[CSQC_MAX_NUM];
// Whether at least one deltalisten is registered (func>0). If not, the Delta* paths
// do not memset the 4096 arrays and do not scan packet entities each frame.
static qbool s_delta_any;

static void CSQC_Client_DeltaReset(void)
{
	s_delta_any = false;
	memset(s_delta_func, 0, sizeof(s_delta_func));
	memset(s_delta_flags, 0, sizeof(s_delta_flags));
	memset(s_player_slot, 0, sizeof(s_player_slot));
	memset(s_delta_slot, 0, sizeof(s_delta_slot));
	memset(s_delta_seen, 0, sizeof(s_delta_seen));
	memset(s_delta_player_owned, 0, sizeof(s_delta_player_owned));
	memset(s_delta_ent_owned, 0, sizeof(s_delta_ent_owned));
}

// Clear the player/delta bridges referencing a freed arena slot (see NetFreeSlot).
static void CSQC_Client_ClearSlotBridges(int slot)
{
	int i;

	if (slot <= 0) {
		return;
	}
	for (i = 0; i < MAX_CLIENTS; i++) {
		if (s_player_slot[i] == slot) {
			s_player_slot[i] = 0;
		}
	}
	for (i = 0; i < CSQC_MAX_NUM; i++) {
		if (s_delta_slot[i] == slot) {
			s_delta_slot[i] = 0;
		}
	}
}

qbool CSQC_Client_DeltaPlayerOwned(int pnum)
{
	return pnum >= 0 && pnum < MAX_CLIENTS && s_delta_player_owned[pnum];
}

qbool CSQC_Client_DeltaEntityOwned(int number)
{
	return number > 0 && number < CSQC_MAX_NUM && s_delta_ent_owned[number];
}

void CSQC_Client_DeltaListen(const char *model, int func, int flags)
{
	int i;
	if (!model) {
		return;
	}
	if (!strcmp(model, "*")) {
		for (i = 0; i < MAX_MODELS; i++) {
			s_delta_func[i] = (func > 0) ? func : 0;
			s_delta_flags[i] = flags;
		}
		if (func > 0) {
			s_delta_any = true;
		}
		return;
	}
	for (i = 1; i < MAX_MODELS; i++) {
		if (!cl.model_name[i][0]) {
			break;
		}
		if (!strcmp(cl.model_name[i], model)) {
			s_delta_func[i] = (func > 0) ? func : 0;
			s_delta_flags[i] = flags;
			if (func > 0) {
				s_delta_any = true;
			}
			break;
		}
	}
}

static void CSQC_Client_DeltaPlayers(pr1vm_t *vm)
{
	int pnum;

	if (!vm || !vm->game_edicts || !vm->edict_size) {
		return;
	}
	if (!s_delta_any) {
		return;
	}
	if (cls.demoplayback || cls.mvdplayback) {
		return;  // live game only
	}
	memset(s_delta_player_owned, 0, sizeof(s_delta_player_owned));
	for (pnum = 0; pnum < MAX_CLIENTS; pnum++) {
		player_state_t *st = &cl.frames[cl.parsecount & UPDATE_MASK].playerstate[pnum];
		int num = pnum + 1;
		int slot = s_player_slot[pnum];
		int func = 0, isnew = 0;

		if (st->messagenum == cl.parsecount && st->modelindex > 0 && st->modelindex < MAX_MODELS) {
			func = s_delta_func[st->modelindex];
		}

		if (!func) {
			// entity absent / no listener - remove it if it was there
			if (slot) {
				// Drop the bridge first (see CSQC_Client_NetRemove).
				s_player_slot[pnum] = 0;
				CSQC_Client_NetRemove(vm, slot, num, true);
			}
			continue;
		}

		// svc76 already owns the number - do not overwrite
		if (!slot && CSQC_Client_NumToSlot(num)) {
			continue;
		}

		if (!slot) {
			slot = CSQC_Client_NetAllocSlot(vm);
			if (!slot) {
				continue;
			}
			CSQC_Client_MapNumber(num, slot);
			s_player_slot[pnum] = slot;
			isnew = 1;
		}

		CSQC_Client_SetContextSlot(vm, (unsigned)slot, (unsigned)num);

		// player_state fields (no-lerp: raw values).
		{
			float *base = (float *)((byte *)vm->game_edicts + (size_t)slot * vm->edict_size);
			if (s_csqc.f_origin >= 0) {
				VectorCopy(st->origin, base + s_csqc.f_origin);
			}
			if (s_csqc.f_velocity >= 0) {
				VectorCopy(st->velocity, base + s_csqc.f_velocity);
			}
			if (s_csqc.f_angles >= 0) {
				// The server sends viewangles only in demos; for the local player use
				// fresh cl.viewangles (updated from usercmd).
				const float *ang = (pnum == cl.playernum) ? cl.viewangles : st->viewangles;
				VectorCopy(ang, base + s_csqc.f_angles);
				// .angles carries model-space pitch: -pitch/3 (QW player models).
				base[s_csqc.f_angles] = -base[s_csqc.f_angles] / 3;
			}
			if (s_csqc.f_modelindex >= 0) {
				base[s_csqc.f_modelindex] = (float)st->modelindex;
			}
			if (s_csqc.f_skin >= 0) {
				base[s_csqc.f_skin] = (float)st->skinnum;
			}
			// Player render fields.
			if (s_csqc.f_frame >= 0) {
				base[s_csqc.f_frame] = (float)st->frame;
			}
			if (s_csqc.f_effects >= 0) {
				base[s_csqc.f_effects] = (float)st->effects;
			}
			if (s_csqc.f_colormap >= 0) {
				base[s_csqc.f_colormap] = (float)(pnum + 1); // player index
			}
			if (s_csqc.f_drawmask >= 0) {
				base[s_csqc.f_drawmask] = 1; // MASK_DELTA
			}
		}

		vm->globals[OFS_PARM0] = isnew;
		{
			// callback return !=0 -> the engine does not draw this player (the module does).
			float pret = 0;
			if (CSQC_Client_ExecRet(func, &pret) && pret != 0) {
				s_delta_player_owned[pnum] = 1;
			}
		}
		if (s_csqc.errored) {
			return;
		}
	}
}

/*
=================
#371 delta-entity bridge: packet entities of the frame (entity_state_t) with a
model-registered callback are handed to the module as
CSQC_Ent_Update (self/.entnum, PARM0 = isnew). Ones missing from the frame go through
the remove path. RSES_NOLERP/NOROTATE: raw state (no interpolation); NOTRAILS/NOLIGHTS
are invalid (ezq CSQC has no trails/dynamic light).
=================
*/
static void CSQC_Client_DeltaEntities(pr1vm_t *vm)
{
	packet_entities_t *pack;
	int i, num;

	if (!vm || !vm->game_edicts || !vm->edict_size) {
		return;
	}
	if (!s_delta_any) {
		return;
	}
	if (cls.demoplayback || cls.mvdplayback) {
		return;
	}
	if (!cl.validsequence) {
		return;
	}

	memset(s_delta_seen, 0, sizeof(s_delta_seen));
	memset(s_delta_ent_owned, 0, sizeof(s_delta_ent_owned));
	pack = &cl.frames[cl.validsequence & UPDATE_MASK].packet_entities;

	for (i = 0; i < pack->num_entities; i++) {
		entity_state_t *es = &pack->entities[i];
		int slot, func, isnew = 0;
		float *base;

		num = es->number;
		if (num <= 0 || num >= CSQC_MAX_NUM) {
			continue;
		}
		if (es->modelindex <= 0 || es->modelindex >= MAX_MODELS) {
			continue;
		}
		func = s_delta_func[es->modelindex];
		if (!func) {
			continue;
		}

		s_delta_seen[num] = 1;
		slot = s_delta_slot[num];
		if (!slot) {
			// svc76 already owns the number - do not overwrite
			if (CSQC_Client_NumToSlot(num)) {
				continue;
			}
			slot = CSQC_Client_NetAllocSlot(vm);
			if (!slot) {
				continue;
			}
			CSQC_Client_MapNumber(num, slot);
			s_delta_slot[num] = slot;
			isnew = 1;
		}

		CSQC_Client_SetContextSlot(vm, (unsigned)slot, (unsigned)num);
		base = (float *)((byte *)vm->game_edicts + (size_t)slot * vm->edict_size);
		if (s_csqc.f_origin >= 0) {
			VectorCopy(es->origin, base + s_csqc.f_origin);
		}
		if (s_csqc.f_angles >= 0) {
			VectorCopy(es->angles, base + s_csqc.f_angles);
		}
		if (s_csqc.f_modelindex >= 0) {
			base[s_csqc.f_modelindex] = (float)es->modelindex;
		}
		if (s_csqc.f_frame >= 0) {
			base[s_csqc.f_frame] = (float)es->frame;
		}
		if (s_csqc.f_skin >= 0) {
			base[s_csqc.f_skin] = (float)es->skinnum;
		}
		if (s_csqc.f_effects >= 0) {
			base[s_csqc.f_effects] = (float)es->effects;
		}
		if (s_csqc.f_drawmask >= 0) {
			base[s_csqc.f_drawmask] = 1; // MASK_DELTA
		}

		vm->globals[OFS_PARM0] = isnew;
		{
			// callback return !=0 -> the engine does not draw this packet entity.
			float pret = 0;
			if (CSQC_Client_ExecRet(func, &pret) && pret != 0) {
				s_delta_ent_owned[num] = 1;
			}
		}
		if (s_csqc.errored) {
			return;
		}
	}

	// ones missing this frame - remove path
	for (num = 1; num < CSQC_MAX_NUM; num++) {
		int slot = s_delta_slot[num];
		if (slot && !s_delta_seen[num]) {
			// Drop the bridge first (see CSQC_Client_NetRemove).
			s_delta_slot[num] = 0;
			CSQC_Client_NetRemove(vm, slot, num, true);
		}
	}
}

/*
=================
CSQC_Client_UpdateLocalEntnum
=================

Publish the module global player_localentnum (entity number of the observed
player) each frame before CSQC_UpdateView. No player entity is fabricated: the
entity environment is what the server sent via svc76 plus the module's own spawns.
*/
void CSQC_Client_UpdateLocalEntnum(void)
{
	pr1vm_t *vm = &s_csqc.vm;

	if (!s_csqc.loaded || !s_csqc.inited || s_csqc.errored || s_csqc.g_localentnum < 0) {
		return;
	}
	vm->globals[s_csqc.g_localentnum] = (cl.viewplayernum >= 0) ? cl.viewplayernum + 1 : 0;
}

/*
=================
view/listener/view_angles + project/unproject.

- `view_angles` - module global, published each frame; value is the engine's
  view angles (cl.viewangles).
- `#351 setlistener` - the module sets the audio listener; cl_main.c uses it in
  S_Update while the module is active (otherwise as usual).
- `#303 setproperty` (VF_* subset) - the module's view-origin/angles/vrect/fov;
  applied to r_refdef after V_CalcRefdef (cl_view.c) when CSQC is active.
  Update timing differs by mode: takeover modules run CSQC_UpdateView before 3D
  rendering, non-takeover modules in the HUD phase (one frame after the scene).
- `#310/#311 project/unproject` - screen<->world via the engine matrices
  (R_GetModelviewMatrix/R_GetProjectionMatrix/R_GetViewport).
=================
*/
#define CSQC_VFP_MIN		1
#define CSQC_VFP_MIN_X		2
#define CSQC_VFP_MIN_Y		3
#define CSQC_VFP_SIZE		4
#define CSQC_VFP_SIZE_X		5
#define CSQC_VFP_SIZE_Y		6
#define CSQC_VFP_VIEWPORT	7
#define CSQC_VFP_FOV		8
#define CSQC_VFP_FOVX		9
#define CSQC_VFP_FOVY		10
#define CSQC_VFP_ORIGIN		11
#define CSQC_VFP_ORIGIN_X	12
#define CSQC_VFP_ORIGIN_Y	13
#define CSQC_VFP_ORIGIN_Z	14
#define CSQC_VFP_ANGLES		15
#define CSQC_VFP_ANGLES_X	16
#define CSQC_VFP_ANGLES_Y	17
#define CSQC_VFP_ANGLES_Z	18
// set flags (standard VF_* numbers). VF_PERSPECTIVE is recognized (return 1) but
// not implemented - the ezq renderer has no orthographic view.
#define CSQC_VFP_DRAWWORLD	19
#define CSQC_VFP_DRAWENGINESBAR	20
#define CSQC_VFP_DRAWCROSSHAIR	21
#define CSQC_VFP_PERSPECTIVE	200

static qbool s_listener_on;
static vec3_t s_listener_org, s_listener_fwd, s_listener_rht, s_listener_up;

static qbool s_vp_on;
static qbool s_vp_origin_set, s_vp_angles_set, s_vp_vrect_set, s_vp_fovx_set, s_vp_fovy_set;
static vec3_t s_vp_origin, s_vp_angles;
static int s_vp_x, s_vp_y, s_vp_w, s_vp_h;
static float s_vp_fovx, s_vp_fovy;
// set flags (clearscene defaults).
static qbool s_vp_drawsbar = false;
static qbool s_vp_drawcrosshair = false;

static void CSQC_Client_ViewPropsReset(void)
{
	s_vp_on = false;
	s_vp_origin_set = s_vp_angles_set = s_vp_vrect_set = false;
	s_vp_fovx_set = s_vp_fovy_set = false;
	// clearscene: sbar/crosshair off.
	s_vp_drawsbar = false;
	s_vp_drawcrosshair = false;
}

static void CSQC_Client_ViewReset(void)
{
	s_listener_on = false;
	VectorClear(s_listener_org);
	VectorClear(s_listener_fwd);
	VectorClear(s_listener_rht);
	VectorClear(s_listener_up);
	CSQC_Client_ViewPropsReset();
}

// Engine camera of the current frame, taken in V_CalcRefdef before the module's view
// properties are applied over r_refdef; clearscene puts it back.
static vec3_t s_engine_vieworg, s_engine_viewangles;
static qbool s_engine_view_valid;

void CSQC_Client_SnapshotEngineView(void)
{
	VectorCopy(r_refdef.vieworg, s_engine_vieworg);
	VectorCopy(r_refdef.viewangles, s_engine_viewangles);
	s_engine_view_valid = true;
}

// #300 clearscene: reset the view properties (the module calls clearscene every
// CSQC_UpdateView; without the reset the #303 override would "stick" between frames).
void CSQC_Client_ResetViewProps(void)
{
	// clearscene restores the engine view defaults. fov/vrect are recomputed only
	// by SCR_CalcRefdef (on vid/fov cvar changes), so a module VF_FOV/VF_SIZE override
	// written into r_refdef would otherwise persist after clearscene. Recompute the
	// engine defaults only when the module had overridden fov/vrect: clearscene runs
	// every module frame, and SCR_CalcRefdef has side effects (Sbar_Changed,
	// scr_fullupdate) that we avoid in the common (no-override) case.
	qbool had_vrect = s_vp_vrect_set;
	qbool had_fov = s_vp_fovx_set || s_vp_fovy_set;
	CSQC_Client_ViewPropsReset();
	if (had_vrect || had_fov) {
		SCR_CalcRefdef();
	}
	// r_refdef still holds last frame's VF_ORIGIN/VF_ANGLES (applied in V_CalcRefdef).
	if (s_engine_view_valid) {
		VectorCopy(s_engine_vieworg, r_refdef.vieworg);
		VectorCopy(s_engine_viewangles, r_refdef.viewangles);
	}
}

// #351 setlistener(origin, forward, right, up)
void CSQC_Client_SetListener(const float *origin, const float *forward, const float *right, const float *up)
{
	VectorCopy(origin, s_listener_org);
	VectorCopy(forward, s_listener_fwd);
	VectorCopy(right, s_listener_rht);
	VectorCopy(up, s_listener_up);
	s_listener_on = true;
}

qbool CSQC_Client_ListenerActive(void)
{
	return s_listener_on && s_csqc.loaded && !s_csqc.errored;
}

void CSQC_Client_GetListener(float *origin, float *forward, float *right, float *up)
{
	VectorCopy(s_listener_org, origin);
	VectorCopy(s_listener_fwd, forward);
	VectorCopy(s_listener_rht, right);
	VectorCopy(s_listener_up, up);
}

// #303 setproperty: VF_* subset (view). args are the sequential float arguments
// after property (a vector is 3 values, a scalar is 1).
qbool CSQC_Client_SetViewProperty(int prop, int argc, const float *args)
{
	qbool handled = true;
	switch (prop) {
		case CSQC_VFP_ORIGIN:
			if (argc >= 3) {
				VectorCopy(args, s_vp_origin);
				s_vp_origin_set = true;
			}
			break;
		case CSQC_VFP_ORIGIN_X:
			s_vp_origin[0] = args[0];
			s_vp_origin_set = true;
			break;
		case CSQC_VFP_ORIGIN_Y:
			s_vp_origin[1] = args[0];
			s_vp_origin_set = true;
			break;
		case CSQC_VFP_ORIGIN_Z:
			s_vp_origin[2] = args[0];
			s_vp_origin_set = true;
			break;
		case CSQC_VFP_ANGLES:
			if (argc >= 3) {
				VectorCopy(args, s_vp_angles);
				s_vp_angles_set = true;
			}
			break;
		case CSQC_VFP_ANGLES_X:
			s_vp_angles[0] = args[0];
			s_vp_angles_set = true;
			break;
		case CSQC_VFP_ANGLES_Y:
			s_vp_angles[1] = args[0];
			s_vp_angles_set = true;
			break;
		case CSQC_VFP_ANGLES_Z:
			s_vp_angles[2] = args[0];
			s_vp_angles_set = true;
			break;
		case CSQC_VFP_VIEWPORT:
			// position vector (PARM1) + size vector (PARM2), i.e. 6 words.
			if (argc >= 6) {
				s_vp_x = (int)args[0];
				s_vp_y = (int)args[1];
				s_vp_w = (int)args[3];
				s_vp_h = (int)args[4];
				s_vp_vrect_set = true;
			}
			break;
		case CSQC_VFP_MIN:
			if (argc >= 2) {
				s_vp_x = (int)args[0];
				s_vp_y = (int)args[1];
				s_vp_vrect_set = true;
			}
			break;
		case CSQC_VFP_MIN_X:
			s_vp_x = (int)args[0];
			s_vp_vrect_set = true;
			break;
		case CSQC_VFP_MIN_Y:
			s_vp_y = (int)args[0];
			s_vp_vrect_set = true;
			break;
		case CSQC_VFP_SIZE:
			if (argc >= 2) {
				s_vp_w = (int)args[0];
				s_vp_h = (int)args[1];
				s_vp_vrect_set = true;
			}
			break;
		case CSQC_VFP_SIZE_X:
			s_vp_w = (int)args[0];
			s_vp_vrect_set = true;
			break;
		case CSQC_VFP_SIZE_Y:
			s_vp_h = (int)args[0];
			s_vp_vrect_set = true;
			break;
		case CSQC_VFP_FOV:
			if (argc >= 2) {
				s_vp_fovx = args[0];
				s_vp_fovy = args[1];
				s_vp_fovx_set = s_vp_fovy_set = true;
			}
			break;
		case CSQC_VFP_FOVX:
			s_vp_fovx = args[0];
			s_vp_fovx_set = true;
			break;
		case CSQC_VFP_FOVY:
			s_vp_fovy = args[0];
			s_vp_fovy_set = true;
			break;
		case CSQC_VFP_DRAWWORLD:
			// recognized (return 1); the world under takeover is always drawn by
			// R_RenderView - there is no RDF_NOWORLDMODEL analog.
			break;
		case CSQC_VFP_DRAWENGINESBAR:
			if (argc >= 1) {
				s_vp_drawsbar = (args[0] != 0);
			}
			break;
		case CSQC_VFP_DRAWCROSSHAIR:
			if (argc >= 1) {
				s_vp_drawcrosshair = (args[0] != 0);
			}
			break;
		case CSQC_VFP_PERSPECTIVE:
			// recognized (return 1); not implemented (see CSQC_VFP_PERSPECTIVE).
			break;
		default:
			handled = false; // unsupported property: return 0
			break;
	}
	s_vp_on = s_vp_origin_set || s_vp_angles_set || s_vp_vrect_set || s_vp_fovx_set || s_vp_fovy_set;
	// Apply at once so the change takes effect in this frame, not one frame late.
	CSQC_Client_ApplyViewProps();
	return handled;
}

// Applied after V_CalcRefdef and on every #303, only when a CSQC module is active.
void CSQC_Client_ApplyViewProps(void)
{
	if (!s_vp_on || !s_csqc.loaded || s_csqc.errored) {
		return;
	}
	if (s_vp_origin_set) {
		VectorCopy(s_vp_origin, r_refdef.vieworg);
	}
	if (s_vp_angles_set) {
		VectorCopy(s_vp_angles, r_refdef.viewangles);
	}
	if (s_vp_vrect_set) {
		r_refdef.vrect.x = s_vp_x;
		r_refdef.vrect.y = s_vp_y;
		r_refdef.vrect.width = s_vp_w;
		r_refdef.vrect.height = s_vp_h;
	}
	if (s_vp_fovx_set) {
		r_refdef.fov_x = s_vp_fovx;
	}
	if (s_vp_fovy_set) {
		r_refdef.fov_y = s_vp_fovy;
	}
	// The render camera is derived from r_refdef later, in R_CSQC_BeginCull and
	// CSQC_Client_RenderScene. Doing it here (before R_SetupFrame) would
	// re-snapshot r_oldviewleaf/r_oldviewleaf2 and desync R_MarkLeaves (stale PVS).
}

/*
=================
Gate of the engine sbar/HUD and crosshair. clearscene turns both off; the module
turns them back on via #303 setproperty(VF_DRAWENGINESBAR/VF_DRAWCROSSHAIR, 1).
Only applied under takeover (CSQC_Client_SceneActive).
=================
*/
qbool CSQC_Client_DrawEngineSbar(void)
{
	return s_vp_drawsbar;
}

qbool CSQC_Client_DrawCrosshairFlag(void)
{
	return s_vp_drawcrosshair;
}

// Called before CSQC_UpdateView.
void CSQC_Client_PublishViewAngles(void)
{
	pr1vm_t *vm = &s_csqc.vm;
	if (!s_csqc.loaded || !s_csqc.inited || s_csqc.errored || s_csqc.g_view_angles < 0) {
		return;
	}
	vm->globals[s_csqc.g_view_angles + 0] = cl.viewangles[0];
	vm->globals[s_csqc.g_view_angles + 1] = cl.viewangles[1];
	vm->globals[s_csqc.g_view_angles + 2] = cl.viewangles[2];
}

/*
#311 project / #310 unproject:
clip = v * (model*proj) (row vectors, i.e. proj*modelview), NDC -> screen with a
Y flip and r_refdef.vrect; depth is NDC z, negated when w < 0.
There is no guard on w; degenerate cases yield NaN/Inf.
*/
qbool CSQC_Client_Project(const float *world, float *sx, float *sy, float *sz)
{
	float model[16], proj[16], a[16], v[4], clip[4], sum;
	float rx, ry, rw, rh;
	int i, j, k;

	// Build the matrices from r_refdef (the current frame's camera), not the live GL
	// matrices - before renderscene the latter are the previous 2D-ortho HUD pass.

	R_GetRefdefMatrices(model, proj);

	// a = model * proj (row vector)
	for (i = 0; i < 4; i++) {
		for (j = 0; j < 4; j++) {
			sum = 0;
			for (k = 0; k < 4; k++) {
				sum += model[i * 4 + k] * proj[k * 4 + j];
			}
			a[i * 4 + j] = sum;
		}
	}
	v[0] = world[0];
	v[1] = world[1];
	v[2] = world[2];
	v[3] = 1;
	for (j = 0; j < 4; j++) {
		sum = 0;
		for (k = 0; k < 4; k++) {
			sum += v[k] * a[k * 4 + j];
		}
		clip[j] = sum;
	}
	clip[0] /= clip[3]; // no guard (degenerate w -> NaN/Inf)
	clip[1] /= clip[3];
	clip[2] /= clip[3];

	rx = r_refdef.vrect.x;
	ry = r_refdef.vrect.y;
	rw = r_refdef.vrect.width;
	rh = r_refdef.vrect.height;
	*sx = (1 + clip[0]) / 2 * rw + rx;
	*sy = (1 - (1 + clip[1]) / 2) * rh + ry;
	*sz = clip[2];
	if (clip[3] < 0) {
		*sz = -*sz;
	}
	return true;
}

// Inverse 4x4 (row-major) by Gauss-Jordan elimination.
static qbool CSQC_Mat4_Invert(const float *m, float *out)
{
	float a[4][8];
	int i, j, k;

	for (i = 0; i < 4; i++) {
		for (j = 0; j < 4; j++) {
			a[i][j] = m[i * 4 + j];
			a[i][j + 4] = (i == j) ? 1.0f : 0.0f;
		}
	}
	for (i = 0; i < 4; i++) {
		int piv = i;
		for (k = i + 1; k < 4; k++) {
			if (fabs(a[k][i]) > fabs(a[piv][i])) {
				piv = k;
			}
		}
		if (fabs(a[piv][i]) < 1e-12f) {
			return false;
		}
		if (piv != i) {
			for (j = 0; j < 8; j++) {
				float t = a[i][j];
				a[i][j] = a[piv][j];
				a[piv][j] = t;
			}
		}
		{
			float d = a[i][i];
			for (j = 0; j < 8; j++) {
				a[i][j] /= d;
			}
		}
		for (k = 0; k < 4; k++) {
			float f;
			if (k == i) {
				continue;
			}
			f = a[k][i];
			for (j = 0; j < 8; j++) {
				a[k][j] -= f * a[i][j];
			}
		}
	}
	for (i = 0; i < 4; i++) {
		for (j = 0; j < 4; j++) {
			out[i * 4 + j] = a[i][j + 4];
		}
	}
	return true;
}

// #310 unproject(screen x, y, depth) -> world; inverse of CSQC_Client_Project.
qbool CSQC_Client_Unproject(float sx, float sy, float sz, float *world)
{
	float model[16], proj[16], a[16], inv[16], v[4], res[4], sum, tx, ty;
	int i, j, k;

	// r_refdef-derived matrices (see CSQC_Client_Project).
	R_GetRefdefMatrices(model, proj);
	for (i = 0; i < 4; i++) {
		for (j = 0; j < 4; j++) {
			sum = 0;
			for (k = 0; k < 4; k++) {
				sum += model[i * 4 + k] * proj[k * 4 + j];
			}
			a[i * 4 + j] = sum;
		}
	}
	if (!CSQC_Mat4_Invert(a, inv)) {
		return false;
	}

	tx = (sx - r_refdef.vrect.x) / r_refdef.vrect.width;
	ty = (sy - r_refdef.vrect.y) / r_refdef.vrect.height;
	ty = 1 - ty;
	v[0] = tx * 2 - 1;
	v[1] = ty * 2 - 1;
	v[2] = sz * 2 - 1;
	if (v[2] >= 1) {
		v[2] = 0.999999f;
	}
	v[3] = 1;
	for (j = 0; j < 4; j++) {
		sum = 0;
		for (k = 0; k < 4; k++) {
			sum += v[k] * inv[k * 4 + j];
		}
		res[j] = sum;
	}
	// no guard on res[3]
	world[0] = res[0] / res[3];
	world[1] = res[1] / res[3];
	world[2] = res[2] / res[3];
	return true;
}

/*
=================
CSQC_Client_GetEntity

#504 getentity (FTE builtin): read interpolated state of non-csqc
(engine-networked) entities by server number. The source is cl_entities[]
(current entity_state_t + per-frame lerp data) and, for players, player_state_t /
player bbox / player colours. "Active" = present in the current packet (player:
playerstate.messagenum == cl.parsecount; map: cent->sequence == cl.validsequence).

Origin and angles are interpolated with the same lerp data the renderer uses
(cent->old_origin/current.origin, old_angles/current.angles, startlerp/deltalerp),
so even the local player and non-drawn entities (whose lerp_origin is not written by
CL_LinkPlayers) get the lerped values. The local player's pitch is model-space
(-viewangles[0]/3); remote players keep the raw packet angles. GE_MAXENTS is the
highest packet entity number + headroom.

out[3] is always zeroed then filled (float fields use out[0]; vector fields use all
three). Fields with no ezq data source return the default (0, or '1 1 1' for
GLOWMOD/RTCOLOUR).
=================
*/
#define CSQC_GE_MAXENTS		(-1)
#define CSQC_GE_ACTIVE		0
#define CSQC_GE_ORIGIN		1
#define CSQC_GE_FORWARD		2
#define CSQC_GE_RIGHT		3
#define CSQC_GE_UP		4
#define CSQC_GE_SCALE		5
#define CSQC_GE_ORIGINANDVECTORS 6
#define CSQC_GE_ALPHA		7
#define CSQC_GE_COLORMOD	8
#define CSQC_GE_PANTSCOLOR	9
#define CSQC_GE_SHIRTCOLOR	10
#define CSQC_GE_SKIN		11
#define CSQC_GE_MINS		12
#define CSQC_GE_MAXS		13
#define CSQC_GE_ABSMIN		14
#define CSQC_GE_ABSMAX		15
#define CSQC_GE_LIGHT		16
#define CSQC_GE_MODELINDEX	200
#define CSQC_GE_EFFECTS		202
#define CSQC_GE_FRAME		203
// Alias models store model-space (negated) pitch, so GE_FORWARD/RIGHT/UP flip it
// back before building the vectors; brush/sprite models use the angles as-is.
static float CSQC_Client_GePitchScale(int modelindex)
{
	model_t *mod = (modelindex > 0 && modelindex < MAX_MODELS) ? cl.model_precache[modelindex] : NULL;

	return (mod && mod->type == mod_alias) ? -1.0f : 1.0f;
}

#define CSQC_GE_ANGLES		204
#define CSQC_GE_GLOWMOD		208
#define CSQC_GE_RTCOLOUR	213

// Lerp helpers for #504 - mirror the renderer's interpolation. The lerp data
// (old_origin/current.origin, old_angles/current.angles, startlerp/deltalerp) is
// filled by CL_SetupPacketEntity (map entities) and SetupPlayerEntity (players).
extern cvar_t cl_nolerp, cl_lerp_monsters;
extern qbool cl_nolerp_on_entity_flag;
extern qbool NewLerp_AbleModel(int idx);

static qbool CSQC_Client_EntityIsMonster(int modelindex)
{
	int i;
	if (!cl_lerp_monsters.value) {
		return false;
	}
	for (i = 1; i < 17; i++) {
		if (modelindex == cl_modelindices[mi_monster1 + i - 1]) {
			return true;
		}
	}
	return false;
}

static void CSQC_Client_EntityLerp(const centity_t *cent, vec3_t org, vec3_t ang)
{
	double time = cls.mvdplayback ? cls.demotime : cl.time;
	float lerp;

	// Same gates as the renderer: no-lerp cvars/flag (not in demos, not for
	// monsters) and a non-positive lerp delta fall back to the packet state.
	if (((cl_nolerp.value || cl_nolerp_on_entity_flag) && !cls.mvdplayback &&
			!CSQC_Client_EntityIsMonster(cent->current.modelindex)) ||
		cent->deltalerp <= 0) {
		VectorCopy(cent->current.origin, org);
		VectorCopy(cent->current.angles, ang);
		return;
	}

	lerp = min(max((float)((time - cent->startlerp) / cent->deltalerp), 0.0f), 1.0f);

	if (NewLerp_AbleModel(cent->current.modelindex)) {
		float d = time - cent->startlerp;

		if (d >= 2 * cent->deltalerp) { // entity looks stopped - stay at last lerp
			VectorCopy(cent->lerp_origin, org);
		}
		else {
			VectorMA(cent->old_origin, d, cent->velocity, org);
		}
	}
	else {
		VectorInterpolate(cent->old_origin, lerp, cent->current.origin, org);
	}
	AngleInterpolate(cent->old_angles, lerp, cent->current.angles, ang);
}

void CSQC_Client_GetEntity(int entnum, int fldnum, float out[3])
{
	centity_t *cent;
	entity_state_t *es;
	player_state_t *ps;
	qbool is_player = false;
	qbool active;
	int pnum = -1, modelindex;
	const model_t *model;
	vec3_t org, ang;

	if (out) {
		out[0] = out[1] = out[2] = 0;
	}

	if (cls.state != ca_active) {
		return;
	}

	if (fldnum == CSQC_GE_MAXENTS) {
		// Highest packet-entity number seen this frame + 16 headroom.
		// Replacement deltas keep entities outside packet_entities, so the
		// retained slot extent is the highest-number source there.
		packet_entities_t *pack;
		int i, mx = 0;

		if (CL_RD_Valid()) {
			mx = CL_RD_SlotLimit();
		}
		else if (cl.validsequence) {
			pack = &cl.frames[cl.parsecount & UPDATE_MASK].packet_entities;
			for (i = 0; i < pack->num_entities; i++) {
				mx = max(mx, pack->entities[i].number + 1);
			}
		}
		mx = max(mx, MAX_CLIENTS);
		out[0] = (float)min(CL_MAX_EDICTS, mx + 16);
		return;
	}

	if (entnum < 0 || entnum >= CL_MAX_EDICTS) {
		return;  // invalid entity -> 0
	}

	cent = &cl_entities[entnum];
	es = &cent->current;

	// Players are tracked through playerinfo (SetupPlayerEntity: cent->sequence =
	// state->messagenum) and are "present" when playerstate was updated this frame
	// (same test as CL_LinkPlayers); map entities use the packet-entity frame
	// sequence (CL_SetupPacketEntity: cent->sequence = cl.validsequence).
	ps = (entnum >= 1 && entnum <= MAX_CLIENTS) ? &cl.frames[cl.parsecount & UPDATE_MASK].playerstate[entnum - 1] : NULL;
	if (ps && ps->messagenum == cl.parsecount) {
		is_player = true, pnum = entnum - 1;
	}

	active = is_player ? true : (cent->sequence != 0 && cent->sequence == cl.validsequence);
	if (!active) {
		return;
	}

	modelindex = es->modelindex;

	CSQC_Client_EntityLerp(cent, org, ang);

	// Local player: the server does not send the local player its own viewangles
	// (playerstate.viewangles is demo-only), so use the client's own angles - the
	// same source as the #371 bridge - with the renderer's model-space pitch
	// (-viewangles[0]/3). Remote players keep the raw command angles.
	if (is_player && pnum == cl.playernum) {
		VectorCopy(cl.viewangles, ang);
		ang[0] = -ang[0] / 3;
	}

	switch (fldnum) {
		case CSQC_GE_ACTIVE:
			out[0] = 1;
			break;
		case CSQC_GE_ORIGIN:
			VectorCopy(org, out);
			break;
		case CSQC_GE_ANGLES:
			VectorCopy(ang, out);
			break;
		case CSQC_GE_FORWARD:
		case CSQC_GE_RIGHT:
		case CSQC_GE_UP:
		{
			vec3_t vang;
			VectorCopy(ang, vang);
			vang[0] *= CSQC_Client_GePitchScale(modelindex);
			AngleVectors(vang,
				(fldnum == CSQC_GE_FORWARD) ? out : NULL,
				(fldnum == CSQC_GE_RIGHT) ? out : NULL,
				(fldnum == CSQC_GE_UP) ? out : NULL);
		} break;
		case CSQC_GE_ORIGINANDVECTORS:
		{
			vec3_t vang;
			VectorCopy(org, out);
			VectorCopy(ang, vang);
			vang[0] *= CSQC_Client_GePitchScale(modelindex);
			CSQC_Client_MakeVectors(vang); // module v_forward/v_right/v_up
		} break;
		case CSQC_GE_MINS:
		case CSQC_GE_MAXS:
		case CSQC_GE_ABSMIN:
		case CSQC_GE_ABSMAX:
		{
			vec3_t mn, mx;
			if (is_player) {
				// the prediction hull
				extern vec3_t player_mins, player_maxs;
				VectorCopy(player_mins, mn);
				VectorCopy(player_maxs, mx);
			}
			else {
				// The entity state carries no solid size: approximate with the
				// model bounding box.
				model = (modelindex > 0 && modelindex < MAX_MODELS) ? cl.model_precache[modelindex] : NULL;
				if (model) {
					VectorCopy(model->mins, mn);
					VectorCopy(model->maxs, mx);
				}
				else {
					VectorClear(mn), VectorClear(mx);
				}
			}
			if (fldnum == CSQC_GE_MINS) {
				VectorCopy(mn, out);
			}
			else if (fldnum == CSQC_GE_MAXS) {
				VectorCopy(mx, out);
			}
			else if (fldnum == CSQC_GE_ABSMIN) {
				VectorAdd(org, mn, out);
			}
			else {
				VectorAdd(org, mx, out);
			}
		} break;
		case CSQC_GE_SCALE:
			out[0] = 1;  // the entity state carries no scale
			break;
		case CSQC_GE_ALPHA:
#ifdef FTE_PEXT_TRANS
			out[0] = es->trans / 255.0f;
#else
			out[0] = 1;
#endif
			break;
		case CSQC_GE_COLORMOD:
#ifdef FTE_PEXT_COLOURMOD
			out[0] = es->colourmod[0] / 8.0f;
			out[1] = es->colourmod[1] / 8.0f;
			out[2] = es->colourmod[2] / 8.0f;
#endif
			break;
		case CSQC_GE_PANTSCOLOR:
			out[0] = is_player ? (float)cl.players[pnum].bottomcolor : (float)(es->colormap & 15);
			break;
		case CSQC_GE_SHIRTCOLOR:
			out[0] = is_player ? (float)cl.players[pnum].topcolor : (float)((es->colormap >> 4) & 15);
			break;
			// For players the source is player_state - SetupPlayerEntity does not
			// copy skinnum/effects into cent->current.
		case CSQC_GE_SKIN:
			out[0] = (float)(is_player ? ps->skinnum : es->skinnum);
			break;
		case CSQC_GE_LIGHT:
			out[0] = 0;
			break;
		case CSQC_GE_MODELINDEX:
			out[0] = (float)(is_player ? ps->modelindex : es->modelindex);
			break;
		case CSQC_GE_EFFECTS:
			out[0] = (float)(is_player ? ps->effects : es->effects);
			break;
		case CSQC_GE_FRAME:
			out[0] = (float)(is_player ? ps->frame : es->frame);
			break;
		default:
			// GE_MODELINDEX2/GE_FATNESS/GE_DRAWFLAGS/GE_ABSLIGHT/GE_GLOWSIZE/
			// GE_GLOWCOLOUR/GE_RTSTYLE/GE_RTPFLAGS/GE_RTRADIUS/GE_TAGENTITY/
			// GE_TAGINDEX/GE_GRAVITYDIR/GE_TRAILEFFECTNUM - no ezq data source;
			// return the default (the vector fields GE_GLOWMOD and GE_RTCOLOUR are
			// '1 1 1', GE_GLOWCOLOUR and the rest are 0).
			if (fldnum == CSQC_GE_GLOWMOD || fldnum == CSQC_GE_RTCOLOUR) {
				out[0] = out[1] = out[2] = 1;
			}
			break;
	}
}

/*
=================
PR1VM_LumpFits / PR1VM_StmtWords / PR1VM_ValidateClientV6

A downloaded csprogs.dat is server-supplied and must not be trusted. The server PR1
core is only ever fed a locally-installed, CRC-checked progs, so its loader trusts
the header; the client loader must not. Validate the header lump ranges against the
file size and the operand/field ranges the interpreter indexes
(`vm->globals[st->a/b/c]`, `parm_start/locals`, `first_statement`) before
PR1VM_LoadData byte-swaps and walks the lumps.
=================
*/
static qbool PR1VM_LumpFits(int ofs, int num, int elemsize, int filesize, const char *name)
{
	// Bound by division (num <= (filesize - ofs) / elemsize) instead of the
	// product ofs + num*elemsize, which wraps on a 32-bit size_t (i686) and would
	// let a crafted num slip past. Same result on 32/64-bit.
	if (ofs < (int)sizeof(dprograms_t) || ofs > filesize || num < 0 || elemsize <= 0 || num > (filesize - ofs) / elemsize) {
		Con_Printf("CSQC: csprogs.dat rejected: bad %s lump (ofs=%d num=%d)\n", name, ofs, num);
		return false;
	}
	return true;
}

// Width (in float words) of operand a/b/c (which 0/1/2) for the ops that read or
// write 3 words; all others are single-word. Branch ops (OP_GOTO/OP_IF/OP_IFNOT)
// store a *statement delta* in one operand (handled at the call site, not here).
static int PR1VM_StmtWords(int op, int which)
{
	switch (op) {
		case OP_DONE:
		case OP_RETURN:
			return which == 0 ? 3 : 1;
		case OP_MUL_V:
			return (which == 0 || which == 1) ? 3 : 1;
		case OP_ADD_V:
		case OP_SUB_V:
			return 3;
		case OP_MUL_FV:
			return (which == 1 || which == 2) ? 3 : 1;
		case OP_MUL_VF:
			return (which == 0 || which == 2) ? 3 : 1;
		case OP_EQ_V:
		case OP_NE_V:
			return (which == 0 || which == 1) ? 3 : 1;
		case OP_NOT_V:
			return which == 0 ? 3 : 1;
		case OP_STORE_V:
			// Both operands are vectors: a is the source (a->vector[0..2]), b the
			// destination (b->vector[0..2]) - matches pr_exec.c OP_STORE_V.
			return (which == 0 || which == 1) ? 3 : 1;
		case OP_LOAD_V:
			return which == 2 ? 3 : 1;
		case OP_STOREP_V:
			// a is the vector source (OPA->_vector[0..2]); b is the edict pointer
			// (runtime-bounded), c unused. Matches pr_exec.c OP_STOREP_V.
			return which == 0 ? 3 : 1;
		default:
			return 1;
	}
}

static qbool PR1VM_ValidateClientV6(const byte *data, int filesize)
{
	dprograms_t h;
	const dstatement_t *st;
	const dfunction_t *fn;
	const ddef_t *dd;
	int i, j;

	if (!data || filesize < (int)sizeof(h)) {
		Con_Printf("PR1VM_LoadClientV6: file too small (%d bytes)\n", filesize);
		return false;
	}

	memcpy(&h, data, sizeof(h));
	for (i = 0; i < (int)(sizeof(h) / sizeof(int)); i++) {
		((int *)&h)[i] = LittleLong(((int *)&h)[i]);
	}

	if (h.version != PROG_VERSION) {
		Con_Printf("PR1VM_LoadClientV6: not a QW v6 progs (version=%d)\n", h.version);
		return false;
	}

	if (!PR1VM_LumpFits(h.ofs_statements, h.numstatements, sizeof(dstatement_t), filesize, "statements") ||
		!PR1VM_LumpFits(h.ofs_globaldefs, h.numglobaldefs, sizeof(ddef_t), filesize, "globaldefs") ||
		!PR1VM_LumpFits(h.ofs_fielddefs, h.numfielddefs, sizeof(ddef_t), filesize, "fielddefs") ||
		!PR1VM_LumpFits(h.ofs_functions, h.numfunctions, sizeof(dfunction_t), filesize, "functions") ||
		!PR1VM_LumpFits(h.ofs_strings, h.numstrings, 1, filesize, "strings") ||
		!PR1VM_LumpFits(h.ofs_globals, h.numglobals, sizeof(float), filesize, "globals")) {
		return false;
	}

	if (h.numstatements < 1 || h.numfunctions < 1 || h.numstrings < 1 ||
		h.numglobals < RESERVED_OFS ||
		h.entityfields <= 0 || h.entityfields > CSQC_MAX_ENTITYFIELDS) {
		Con_Printf("CSQC: csprogs.dat rejected: bad counts (stmt=%d func=%d str=%d glob=%d ef=%d)\n",
			h.numstatements, h.numfunctions, h.numstrings, h.numglobals, h.entityfields);
		return false;
	}

	// Reject overlapping header lumps. The lumps are used
	// in-place in the file buffer, so a write through one lump must not reach
	// another's bytes: a valid OP_STORE_F global write overlapping statements[]
	// would rewrite an executable statement, and a later OP_CALL would read/write
	// outside the module buffer (heap OOB). Ranges already fit the file (checked
	// above), so ofs + len cannot overflow.
	{
		struct {
			int ofs;
			int len;
			const char *name;
		} lumps[6];
		int a, b;

		lumps[0].ofs = h.ofs_statements;
		lumps[0].len = h.numstatements * (int)sizeof(dstatement_t);
		lumps[0].name = "statements";
		lumps[1].ofs = h.ofs_globaldefs;
		lumps[1].len = h.numglobaldefs * (int)sizeof(ddef_t);
		lumps[1].name = "globaldefs";
		lumps[2].ofs = h.ofs_fielddefs;
		lumps[2].len = h.numfielddefs * (int)sizeof(ddef_t);
		lumps[2].name = "fielddefs";
		lumps[3].ofs = h.ofs_functions;
		lumps[3].len = h.numfunctions * (int)sizeof(dfunction_t);
		lumps[3].name = "functions";
		// For QW v6 progs numstrings is the byte size of the strings area
		// [strings, strings+numstrings), not a count of strings.
		lumps[4].ofs = h.ofs_strings;
		lumps[4].len = h.numstrings;
		lumps[4].name = "strings";
		lumps[5].ofs = h.ofs_globals;
		lumps[5].len = h.numglobals * (int)sizeof(float);
		lumps[5].name = "globals";

		for (a = 0; a < 6; a++) {
			if (lumps[a].len <= 0) {
				continue;
			}
			for (b = a + 1; b < 6; b++) {
				if (lumps[b].len <= 0) {
					continue;
				}
				if (lumps[a].ofs < lumps[b].ofs + lumps[b].len && lumps[b].ofs < lumps[a].ofs + lumps[a].len) {
					Con_Printf("CSQC: csprogs.dat rejected: %s lump overlaps %s\n", lumps[a].name, lumps[b].name);
					return false;
				}
			}
		}
	}

	// statements: every operand indexes vm->globals[st->a/b/c]; vector ops use 3.
	// Branch deltas (OP_GOTO->a, OP_IF/OP_IFNOT->b) are targets, not globals -
	// bounded at run time in PR1VM_ExecuteProgram.
	st = (const dstatement_t *)((const byte *)data + h.ofs_statements);
	for (i = 0; i < h.numstatements; i++) {
		int op = (int)(unsigned short)LittleShort((short)st[i].op);
		int ops[3];

		if (op > OP_BITOR) {
			Con_Printf("CSQC: csprogs.dat rejected: bad opcode %d at statement %d\n", op, i);
			return false;
		}

		// Operands are unsigned shorts; a signed read would make valid offsets
		// >32767 look negative. Branch deltas are skipped below, so the sign of
		// those operands is irrelevant.
		ops[0] = (int)(unsigned short)LittleShort(st[i].a);
		ops[1] = (int)(unsigned short)LittleShort(st[i].b);
		ops[2] = (int)(unsigned short)LittleShort(st[i].c);
		for (j = 0; j < 3; j++) {
			int words;

			if ((op == OP_GOTO && j == 0) || ((op == OP_IF || op == OP_IFNOT) && j == 1)) {
				continue;
			}
			// OP_RETURN/OP_DONE read a..a+2, but the client VM owns a copy of the
			// globals padded by two words, so a == numglobals-1 is valid and
			// stays inside the allocation.
			words = ((op == OP_RETURN || op == OP_DONE) && j == 0) ? 1 : PR1VM_StmtWords(op, j);
			if (ops[j] < 0 || ops[j] > h.numglobals - words) {
				Con_Printf("CSQC: csprogs.dat rejected: statement %d operand out of range\n", i);
				return false;
			}
		}
	}

	// defs: every def offset + its type width must stay inside the lump the
	// engine writes through (globaldefs -> globals, fielddefs -> entvars);
	// width is 3 for a vector def, 1 otherwise. ofs is unsigned (ddef_t).
	dd = (const ddef_t *)((const byte *)data + h.ofs_globaldefs);
	for (i = 0; i < h.numglobaldefs; i++) {
		int ofs = (int)(unsigned short)LittleShort((short)dd[i].ofs);
		int type = (int)(unsigned short)LittleShort((short)dd[i].type) & ~DEF_SAVEGLOBAL;
		int width = (type == ev_vector) ? 3 : 1;

		if (ofs + width > h.numglobals) {
			Con_Printf("CSQC: csprogs.dat rejected: globaldef %d out of range (ofs=%d w=%d glob=%d)\n",
				i, ofs, width, h.numglobals);
			return false;
		}
	}
	dd = (const ddef_t *)((const byte *)data + h.ofs_fielddefs);
	for (i = 0; i < h.numfielddefs; i++) {
		int ofs = (int)(unsigned short)LittleShort((short)dd[i].ofs);
		int type = (int)(unsigned short)LittleShort((short)dd[i].type) & ~DEF_SAVEGLOBAL;
		int width = (type == ev_vector) ? 3 : 1;

		if (ofs + width > h.entityfields) {
			Con_Printf("CSQC: csprogs.dat rejected: fielddef %d out of range (ofs=%d w=%d ef=%d)\n",
				i, ofs, width, h.entityfields);
			return false;
		}
	}

	// functions: parm_start/locals within globals, first_statement within statements
	fn = (const dfunction_t *)((const byte *)data + h.ofs_functions);
	for (i = 0; i < h.numfunctions; i++) {
		int first = LittleLong(fn[i].first_statement);
		int parm = LittleLong(fn[i].parm_start);
		int loc = LittleLong(fn[i].locals);
		int nparm = LittleLong(fn[i].numparms);
		int sum = 0;
		int k;

		// first_statement: builtins are negative (|first| <= builtin slots),
		// real code is [0, numstatements). INT_MIN would make OP_CALL's
		// i = -first a negative builtin index. first == 0 for i > 0 is the
		// name-mapped `#0` placeholder; CSQCVM_ResolveNamedBuiltins assigns every
		// such function a numeric slot (known name or the aborting unknown-name
		// stub) right after PR1VM_LoadData, so none is left at 0.
		// Function 0 legitimately has first == 0.
		if (first < -CSQC_MAX_BUILTINS || (first > 0 && first >= h.numstatements)) {
			Con_Printf("CSQC: csprogs.dat rejected: function %d first_statement out of range (%d)\n", i, first);
			return false;
		}
		// Overflow-safe: parm + loc must not be summed before the comparison
		// (parm=1, loc=INT_MAX wraps to INT_MIN).
		if (nparm < 0 || nparm > MAX_PARMS || parm < 0 || loc < 0 || parm > h.numglobals || loc > h.numglobals - parm) {
			Con_Printf("CSQC: csprogs.dat rejected: function %d parms/locals out of range\n", i);
			return false;
		}
		// parm_size: each 0..3 (byte) and the word sum must fit locals;
		// EnterFunction copies sum(parm_size) words from parm_start.
		for (k = 0; k < nparm; k++) {
			int ps = (int)fn[i].parm_size[k];
			if (ps > 3) {
				Con_Printf("CSQC: csprogs.dat rejected: function %d parm_size out of range (%d)\n", i, ps);
				return false;
			}
			sum += ps;
		}
		if (sum > loc) {
			Con_Printf("CSQC: csprogs.dat rejected: function %d parm_size sum %d > locals %d\n", i, sum, loc);
			return false;
		}
	}

	return true;
}

/*
=================
PR1VM_LoadClientV6

Client loader for csprogs.dat (classic QW version 6). No CRC check; errors ->
false + Con_Printf (no SV_Error). Validates the structure before executing
server-supplied bytes.
=================
*/
static qbool PR1VM_LoadClientV6(pr1vm_t *vm, const byte *data, int filesize)
{
	if (!PR1VM_ValidateClientV6(data, filesize)) {
		return false;
	}

	PR1VM_LoadData(vm, (dprograms_t *)data);
	// Resolve `#0` name-mapped builtins now that functions are host-endian, so no
	// i>0 function is left with first_statement == 0 (the validator allows it; the
	// interpreter would otherwise enter statement -1).
	CSQCVM_ResolveNamedBuiltins(vm);
	return true;
}

#ifdef CSQC_DEBUG
/*
=================
PR1VM_CSQCSmoke_f

Debug command: loads csprogs.dat (classic v6) from the current gamedir into a
static client instance, resolves CSQC functions and runs CSQC_WorldLoaded.
=================
*/
static pr1vm_t csqc_smoke_vm;
// Separate string pool for the csqc_smoke debug instance (its own vm).
static csqc_strpool_t csqc_smoke_strpool;

static void PR1VM_CSQCSmoke_f(void)
{
	byte *data;
	int filesize;
	pr1vm_t *vm = &csqc_smoke_vm;
	dfunction_t *f;
	func_t idx;

	// Developer-gated manual diagnostic (matches pr1vm_test_error).
	if (!developer.value) {
		return;
	}

	data = (byte *)FS_LoadHunkFile("csprogs.dat", &filesize);
	if (!data) {
		Con_Printf("csqc_smoke: couldn't load csprogs.dat from gamedir\n");
		return;
	}

	// cleanup (incl. Q_free of the builtin table), then reload
	PR1VM_UnLoad(vm);
	vm->get_string = CSQC_Client_GetString; // bounded untrusted csprogs strings
	if (!PR1VM_LoadClientV6(vm, data, filesize)) {
		Con_Printf("csqc_smoke: v6 load failed\n");
		return;
	}

	// Own string pool (back-pointer in host_udata). No GC for the debug instance
	// (no host_postcall, no arena): its temp strings live until the next run.
	CSQC_Client_FreeTempStrings(&csqc_smoke_strpool);
	memset(&csqc_smoke_strpool, 0, sizeof(csqc_smoke_strpool));
	vm->host_udata = &csqc_smoke_strpool;
	vm->strtbl = csqc_smoke_strpool.strtbl;
	vm->newstrtbl = csqc_smoke_strpool.newstrtbl;
	vm->numstr = &csqc_smoke_strpool.numstr;

	Con_Printf("csqc_smoke: client (v6): statements=%d functions=%d globals=%d"
			   " (server PR1: statements=%d functions=%d)\n",
		vm->progs->numstatements, vm->progs->numfunctions, vm->progs->numglobals,
		progs ? progs->numstatements : -1, progs ? progs->numfunctions : -1);

	CSQCVM_RegisterBuiltins(vm);

	f = PR1VM_FindFunction(vm, "CSQC_Init");
	Con_Printf("csqc_smoke: CSQC_Init %s\n", f ? "found" : "MISSING");
	if (f) {
		idx = (func_t)(f - vm->functions);
		vm->globals[OFS_PARM0] = 0;
		vm->globals[OFS_PARM1] = 0;
		vm->globals[OFS_PARM2] = 0;
		PR1VM_ExecuteProgram(vm, idx);
		Con_Printf("csqc_smoke: CSQC_Init executed ok (registercommand builtins)\n");
	}
	f = PR1VM_FindFunction(vm, "CSQC_WorldLoaded");
	Con_Printf("csqc_smoke: CSQC_WorldLoaded %s\n", f ? "found" : "MISSING");
	if (f) {
		idx = (func_t)(f - vm->functions);
		PR1VM_ExecuteProgram(vm, idx);
		Con_Printf("csqc_smoke: CSQC_WorldLoaded executed ok (server PR1 still alive)\n");
	}
	f = PR1VM_FindFunction(vm, "CSQC_ConsoleCommand");
	Con_Printf("csqc_smoke: CSQC_ConsoleCommand %s\n", f ? "found" : "MISSING");
	if (f) {
		idx = (func_t)(f - vm->functions);
		vm->globals[OFS_PARM0] = 0; // empty command
		vm->globals[OFS_RETURN] = -1;
		PR1VM_ExecuteProgram(vm, idx);
		Con_Printf("csqc_smoke: CSQC_ConsoleCommand ok (ret=%.0f, tokenize/argv builtins)\n", vm->globals[OFS_RETURN]);
	}
	// weapon_name(0) -> ftos(0)="0" (builtin ftos + string return)
	f = PR1VM_FindFunction(vm, "weapon_name");
	if (f) {
		idx = (func_t)(f - vm->functions);
		vm->globals[OFS_PARM0] = 0;
		vm->globals[OFS_RETURN] = 0;
		PR1VM_ExecuteProgram(vm, idx);
		Con_Printf("csqc_smoke: weapon_name(0) -> \"%s\" (ftos builtin)\n",
			CSQC_Client_GetString(vm, *(int *)&vm->globals[OFS_RETURN]));
	}
}

/*
=================
CSQC_Client_ProgsCheck_f

Debug canary (client console `csqc_progscheck`): verifies the load-time validator
(clean csprogs.dat -> accepted; synthetically corrupted copies -> rejected) and runs
the runtime-guard predicate unit tests (PR1VM_TestGuards_f).
=================
*/
static void CSQC_Client_ProgsCheck_f(void)
{
	byte *data;
	byte *buf;
	dprograms_t *h;
	int filesize;
	int pass = 0, fail = 0;

	// Developer-gated manual diagnostic (matches pr1vm_test_error).
	if (!developer.value) {
		return;
	}

	data = (byte *)FS_LoadHunkFile("csprogs.dat", &filesize);
	if (!data || filesize < (int)sizeof(dprograms_t)) {
		Con_Printf("csqc_progscheck: no/short csprogs.dat in gamedir\n");
		return;
	}

#define PC_CHECK(name, cond) \
	do { if (cond) pass++; else { fail++; Con_Printf ("[CSQC-TEST] FAIL %s\n", name); } } while (0)

	// 1) the real file must pass validation
	PC_CHECK("clean-accepted", PR1VM_ValidateClientV6(data, filesize));

	// 2) corrupted copies must be rejected (scratch copy, LE-safe writes)
	buf = (byte *)Q_malloc(filesize);
	if (!buf) {
		Con_Printf("csqc_progscheck: out of memory\n");
		return;
	}
	h = (dprograms_t *)buf;

	memcpy(buf, data, filesize);
	h->ofs_functions = LittleLong(filesize + 4096);
	PC_CHECK("ofs-functions-oob", !PR1VM_ValidateClientV6(buf, filesize));

	memcpy(buf, data, filesize);
	h->numstatements = LittleLong(0x7fffffff);
	PC_CHECK("numstatements-huge", !PR1VM_ValidateClientV6(buf, filesize));

	memcpy(buf, data, filesize);
	h->numglobals = LittleLong(0x7fffffff);
	PC_CHECK("numglobals-huge", !PR1VM_ValidateClientV6(buf, filesize));

	memcpy(buf, data, filesize);
	h->version = LittleLong(7);
	PC_CHECK("bad-version", !PR1VM_ValidateClientV6(buf, filesize));

	memcpy(buf, data, filesize);
	PC_CHECK("tiny-size", !PR1VM_ValidateClientV6(buf, 8));

	memset(buf, 0, filesize);
	PC_CHECK("zeroed", !PR1VM_ValidateClientV6(buf, filesize));

	// 2b) OP_STOREP_V vector source width: a == numglobals-1 must be rejected
	// (interpreter reads globals[a+1..a+2]), a == numglobals-3 must be accepted.
	{
		dstatement_t *st;
		int numglobals = LittleLong(((dprograms_t *)data)->numglobals);
		int ofs_st = LittleLong(((dprograms_t *)data)->ofs_statements);

		memcpy(buf, data, filesize);
		st = (dstatement_t *)(buf + ofs_st);
		st->op = (unsigned short)LittleShort((short)OP_STOREP_V);
		st->a = (short)LittleShort((short)(numglobals - 1));
		st->b = (short)LittleShort(0);
		st->c = (short)LittleShort(0);
		PC_CHECK("storep_v-a-vector-oob", !PR1VM_ValidateClientV6(buf, filesize));

		memcpy(buf, data, filesize);
		st->op = (unsigned short)LittleShort((short)OP_STOREP_V);
		st->a = (short)LittleShort((short)(numglobals - 3));
		st->b = (short)LittleShort(0);
		st->c = (short)LittleShort(0);
		PC_CHECK("storep_v-a-vector-edge", PR1VM_ValidateClientV6(buf, filesize));
	}

	// 2c) OP_STORE_V vector width: both a (source) and b (dest) are 3 words;
	// a == numglobals-1 must be rejected, a == b == numglobals-3 accepted.
	{
		dstatement_t *st;
		int numglobals = LittleLong(((dprograms_t *)data)->numglobals);
		int ofs_st = LittleLong(((dprograms_t *)data)->ofs_statements);

		memcpy(buf, data, filesize);
		st = (dstatement_t *)(buf + ofs_st);
		st->op = (unsigned short)LittleShort((short)OP_STORE_V);
		st->a = (short)LittleShort((short)(numglobals - 1));
		st->b = (short)LittleShort(0);
		st->c = (short)LittleShort(0);
		PC_CHECK("store_v-a-vector-oob", !PR1VM_ValidateClientV6(buf, filesize));

		memcpy(buf, data, filesize);
		st->op = (unsigned short)LittleShort((short)OP_STORE_V);
		st->a = (short)LittleShort((short)(numglobals - 3));
		st->b = (short)LittleShort((short)(numglobals - 3));
		st->c = (short)LittleShort(0);
		PC_CHECK("store_v-a-vector-edge", PR1VM_ValidateClientV6(buf, filesize));
	}

	// 2d) defs: an offset at/past the lump end must be rejected (width-aware).
	// type is forced to ev_float (width 1) so the reject is the ofs bound, not
	// vector width.
	{
		ddef_t *dd;
		int numglobals = LittleLong(((dprograms_t *)data)->numglobals);
		int entityfields = LittleLong(((dprograms_t *)data)->entityfields);
		int numgd = LittleLong(((dprograms_t *)data)->numglobaldefs);
		int numfd = LittleLong(((dprograms_t *)data)->numfielddefs);
		int ofs_gd = LittleLong(((dprograms_t *)data)->ofs_globaldefs);
		int ofs_fd = LittleLong(((dprograms_t *)data)->ofs_fielddefs);

		if (numgd >= 1) {
			memcpy(buf, data, filesize);
			dd = (ddef_t *)(buf + ofs_gd);
			dd->type = (unsigned short)LittleShort((short)ev_float);
			dd->ofs = (unsigned short)LittleShort((short)numglobals);
			PC_CHECK("globaldef-ofs-oob", !PR1VM_ValidateClientV6(buf, filesize));
		}
		else {
			Con_Printf("csqc_progscheck: skip globaldef-ofs-oob (numglobaldefs=0)\n");
		}

		if (numfd >= 1) {
			memcpy(buf, data, filesize);
			dd = (ddef_t *)(buf + ofs_fd);
			dd->type = (unsigned short)LittleShort((short)ev_float);
			dd->ofs = (unsigned short)LittleShort((short)entityfields);
			PC_CHECK("fielddef-ofs-oob", !PR1VM_ValidateClientV6(buf, filesize));
		}
		else {
			Con_Printf("csqc_progscheck: skip fielddef-ofs-oob (numfielddefs=0)\n");
		}
	}

	// 2e) parm_size: a word count > 3 must be rejected.
	{
		dfunction_t *fnp;
		int numf = LittleLong(((dprograms_t *)data)->numfunctions);
		int ofs_fn = LittleLong(((dprograms_t *)data)->ofs_functions);

		if (numf >= 1) {
			memcpy(buf, data, filesize);
			fnp = (dfunction_t *)(buf + ofs_fn);
			fnp->numparms = LittleLong(1);
			fnp->parm_size[0] = 4;
			PC_CHECK("parm_size-oob", !PR1VM_ValidateClientV6(buf, filesize));
		}
		else {
			Con_Printf("csqc_progscheck: skip parm_size-oob (numfunctions=0)\n");
		}
	}

	// 2f) first_statement: INT_MIN rejected; 0 for i>0 is the name-mapped #0
	// placeholder (accepted on load, resolved to a numeric slot); -1 accepted.
	{
		dfunction_t *fnp;
		int numf = LittleLong(((dprograms_t *)data)->numfunctions);
		int ofs_fn = LittleLong(((dprograms_t *)data)->ofs_functions);

		if (numf >= 2) {
			memcpy(buf, data, filesize);
			fnp = (dfunction_t *)(buf + ofs_fn);
			fnp[1].first_statement = LittleLong(INT_MIN);
			PC_CHECK("firststmt-intmin", !PR1VM_ValidateClientV6(buf, filesize));

			memcpy(buf, data, filesize);
			fnp = (dfunction_t *)(buf + ofs_fn);
			fnp[1].first_statement = LittleLong(0);
			PC_CHECK("firststmt-zero-nonfirst-ok", PR1VM_ValidateClientV6(buf, filesize));

			memcpy(buf, data, filesize);
			fnp = (dfunction_t *)(buf + ofs_fn);
			fnp[1].first_statement = LittleLong(-1);
			PC_CHECK("firststmt-builtin-ok", PR1VM_ValidateClientV6(buf, filesize));
		}
		else {
			Con_Printf("csqc_progscheck: skip first_statement cases (numfunctions<2)\n");
		}
	}

	// 2g) lump-range division and entityfields cap. The 32-bit product wrap only
	// manifests on i686; the division keeps the result bitness-invariant, so the
	// same reject holds on 64-bit.
	{
		memcpy(buf, data, filesize);
		h = (dprograms_t *)buf;
		h->numstatements = LittleLong(0x40000000);
		PC_CHECK("lump-division-wrap", !PR1VM_ValidateClientV6(buf, filesize));

		memcpy(buf, data, filesize);
		h = (dprograms_t *)buf;
		h->entityfields = LittleLong(0x40000000);
		PC_CHECK("entityfields-cap", !PR1VM_ValidateClientV6(buf, filesize));
	}

	Q_free(buf);

	// 3) client string accessor: a positive offset at/beyond the module string
	// block must be rejected; in-range stays readable. The bound lives here (client
	// layer), not in the shared PR1VM_GetString.
	{
		pr1vm_t tvm;
		dprograms_t xh;
		int ns = LittleLong(((dprograms_t *)data)->numstrings);

		memset(&tvm, 0, sizeof(tvm));
		memset(&xh, 0, sizeof(xh));
		xh.numstrings = ns;
		tvm.strings = (char *)data;
		tvm.progs = &xh;
		PC_CHECK("clientstr-in-range", CSQC_Client_GetString(&tvm, 0) != NULL);
		PC_CHECK("clientstr-oob", CSQC_Client_GetString(&tvm, ns) == NULL);
		PC_CHECK("clientstr-nullvm", CSQC_Client_GetString(NULL, 0) == NULL);
	}

#undef PC_CHECK

	// 4) runtime-guard predicate unit tests (synthetic instance)
	PR1VM_TestGuards_f();

	// 5) entity-argument conversion bound (CSQC_Client_EntNum) - OOB must clamp to
	// world(0), never fault. Synthetic instance: the
	// helper only needs edict_size/max_edicts.
	{
		pr1vm_t tvm;
		int gpass = 0, gfail = 0;

		memset(&tvm, 0, sizeof(tvm));
		tvm.edict_size = 16;
		tvm.max_edicts = 4;

		PR1VM_GuardCheck("ent_of-zero", CSQC_Client_EntNum(&tvm, 0) == 0, &gpass, &gfail);
		PR1VM_GuardCheck("ent_of-3", CSQC_Client_EntNum(&tvm, 3 * 16) == 3, &gpass, &gfail);
		PR1VM_GuardCheck("ent_of-over", CSQC_Client_EntNum(&tvm, 4 * 16) == 0, &gpass, &gfail);
		PR1VM_GuardCheck("ent_of-negative", CSQC_Client_EntNum(&tvm, -1) == 0, &gpass, &gfail);
		PR1VM_GuardCheck("ent_of-huge", CSQC_Client_EntNum(&tvm, 0x40000000) == 0, &gpass, &gfail);
		PR1VM_GuardCheck("ent_of-small", CSQC_Client_EntNum(&tvm, 1) == 0, &gpass, &gfail);
		PR1VM_GuardCheck("ent_of-nullvm", CSQC_Client_EntNum(NULL, 3 * 16) == 0, &gpass, &gfail);

		Con_Printf("[CSQC-TEST] SUMMARY group=ent_of pass=%d fail=%d\n", gpass, gfail);
	}

	Con_Printf("[CSQC-TEST] SUMMARY group=progscheck pass=%d fail=%d\n", pass, fail);
}

/*
=================
CSQC_Client_DlCheck_f

Debug canary (client console `csqc_dlcheck`): verifies the csprogs download-path
policy. The local write path is always the crc key (never *csprogsname, so a
hostile name cannot escape <gamedir>/csprogsvers/), and a download is only valid
for a non-empty, fully numeric *csprogs. Tests the helpers in isolation, not the
ConnectCheck wiring.
=================
*/
static void CSQC_Client_DlCheck_f(void)
{
	int pass = 0, fail = 0;
	char path[MAX_QPATH];
	char *end;

	// Developer-gated manual diagnostic (matches csqc_progscheck).
	if (!developer.value) {
		return;
	}

#define DLC_CHECK(name, cond) \
	do { if (cond) pass++; else { fail++; Con_Printf ("[CSQC-TEST] FAIL %s\n", name); } } while (0)

	// Local path: crc-key only; a hostile *csprogsname is not an input.
	CSQC_Client_DlPath(path, sizeof(path), 0);
	DLC_CHECK("path-crc0", !strcmp(path, "csprogsvers/0.dat"));
	CSQC_Client_DlPath(path, sizeof(path), 0xdeadbeefu);
	DLC_CHECK("path-crc", !strcmp(path, "csprogsvers/deadbeef.dat"));

	// Download gate: non-empty + fully numeric.
	(void)strtoul("0", &end, 0);
	DLC_CHECK("valid-zero", CSQC_Client_CsprogsDlValid("0", end));
	(void)strtoul("0xdeadbeef", &end, 0);
	DLC_CHECK("valid-hex", CSQC_Client_CsprogsDlValid("0xdeadbeef", end));
	(void)strtoul("", &end, 0);
	DLC_CHECK("invalid-empty", !CSQC_Client_CsprogsDlValid("", end));
	(void)strtoul("abc", &end, 0);
	DLC_CHECK("invalid-alpha", !CSQC_Client_CsprogsDlValid("abc", end));
	(void)strtoul("123xyz", &end, 0);
	DLC_CHECK("invalid-trailing", !CSQC_Client_CsprogsDlValid("123xyz", end));

#undef DLC_CHECK

	Con_Printf("[CSQC-TEST] SUMMARY group=dlcheck pass=%d fail=%d\n", pass, fail);
}
#endif

/*
=================
CSQC_Client_Load

Loads csprogs (path from gamedir; a local file or a freshly downloaded
csprogsvers/<crc>.dat) into the client instance and calls CSQC_Init. Returns true
on success; on failure prints the reason.
=================
*/
static qbool CSQC_Client_Load(const char *path)
{
	byte *data;
	int filesize;
	pr1vm_t *vm;
	dfunction_t *f;

	// PR1VM_LoadData does not copy the buffer, so it is owned by
	// s_csqc.module_data until unload (see csqc_client_state_t).
	data = (byte *)FS_LoadHeapFile(path, &filesize);
	if (!data) {
		Con_Printf("CSQC: server offers csprogs but %s not found locally\n", path);
		return false;
	}

	// Release state of a previous load before the memset.
	CSQC_Client_FreeArena();
	CSQC_Client_BufReset();
	CSQC_Client_DeltaReset();
	CSQC_Client_ViewReset();
	CSQC_Client_ModelReset();
	CSQCVM_ClearParticleEffects();
	CSQC_Client_FreeStringPool();
	CSQC_Client_DrawFontScaleReset();

	Q_free(s_csqc.module_data);
	s_csqc.module_data = NULL;
	Q_free(s_csqc.globals_owned);
	s_csqc.globals_owned = NULL;

	memset(&s_csqc, 0, sizeof(s_csqc));
	s_csqc.module_data = data;
	s_csqc.func_init = s_csqc.func_world = s_csqc.func_update = s_csqc.func_console = s_csqc.func_shutdown = -1;
	s_csqc.func_entupdate = s_csqc.func_entremove = s_csqc.func_parseevent = s_csqc.func_parsetempent = -1;
	s_csqc.func_entspawn = -1;
	s_csqc.mayread = false;
	s_csqc.func_input = -1;
	s_csqc.func_inputevent = -1;
	s_csqc.func_startframe = s_csqc.func_endframe = -1;
	s_csqc.global_time = -1;
	s_csqc.global_gamespeed = -1;
	s_csqc.global_self = -1;
	s_csqc.global_other = -1;
	s_csqc.global_physics_mode = -1;
	s_csqc.field_entnum = -1;
	s_csqc.f_origin = s_csqc.f_velocity = s_csqc.f_angles = s_csqc.f_mins = s_csqc.f_maxs = -1;
	s_csqc.f_movetype = s_csqc.f_flags = s_csqc.f_gravity = s_csqc.f_pmove_flags = -1;
	s_csqc.f_modelindex = s_csqc.f_skin = -1;
	s_csqc.f_frame = s_csqc.f_effects = s_csqc.f_colormap = s_csqc.f_drawmask = -1;
	s_csqc.f_think = s_csqc.f_nextthink = -1;
	s_csqc.g_localentnum = -1;
	s_csqc.in_timelength = s_csqc.in_angles = s_csqc.in_movevalues = -1;
	s_csqc.in_buttons = s_csqc.in_impulse = -1;
	s_csqc.in_sequence = -1;
	s_csqc.g_ccframe = s_csqc.g_scframe = -1;
	s_csqc.p_org = s_csqc.p_vel = s_csqc.p_onground = -1;
	s_csqc.g_vfwd = s_csqc.g_vright = s_csqc.g_vup = -1;
	s_csqc.g_view_angles = -1;
	s_csqc.g_frametime = s_csqc.g_cltime = s_csqc.g_maxclients = -1;
	s_csqc.g_player_localnum = s_csqc.g_intermission = -1;
	CSQC_Client_OffsetCacheReset();
	memset(s_inhist, 0, sizeof(s_inhist));
	s_last_seq = 0;
	s_ccframe = 0;
	s_console_executing = false;

	vm = &s_csqc.vm;
	vm->host_error = CSQC_Client_HostError;
	vm->host_print = CSQC_Client_HostPrint;
	vm->abortbuf_valid = true; // client VM unwinds via the abort-stack
	vm->get_string = CSQC_Client_GetString;
	vm->stateop = CSQC_Client_StateOp;
	vm->host_postcall = CSQC_Client_StringGC;

	if (!PR1VM_LoadClientV6(vm, data, filesize)) {
		Con_Printf("CSQC: %s load failed (v6)\n", path);
		Q_free(data);
		s_csqc.module_data = NULL;
		return false;
	}

	// Client-owned globals copy with two zero words appended. PR1VM_LoadData
	// leaves vm->globals pointing into the module buffer; the validator accepts
	// OP_RETURN/OP_DONE with a == numglobals-1 and the interpreter reads a+1/a+2.
	{
		int numglobals = vm->progs->numglobals;
		float *globals = (float *)Q_malloc((size_t)(numglobals + 2) * sizeof(float));

		memcpy(globals, vm->globals, (size_t)numglobals * sizeof(float));
		globals[numglobals] = 0;
		globals[numglobals + 1] = 0;
		s_csqc.globals_owned = globals;
		vm->globals = globals;
		vm->global_struct = (globalvars_t *)globals;
	}

	// host_udata is the pool back-pointer for PR1VM_ClientSetString.
	vm->host_udata = &s_csqc.strpool;
	vm->strtbl = s_csqc.strpool.strtbl;
	vm->newstrtbl = s_csqc.strpool.newstrtbl;
	vm->numstr = &s_csqc.strpool.numstr;

	CSQCVM_RegisterBuiltins(vm);

	// edict_size is known only after load.
	CSQC_Client_AllocArena(vm);

	f = PR1VM_FindFunction(vm, "CSQC_Init");
	if (f) {
		s_csqc.func_init = (int)(f - vm->functions);
	}
	f = PR1VM_FindFunction(vm, "CSQC_WorldLoaded");
	if (f) {
		s_csqc.func_world = (int)(f - vm->functions);
	}
	f = PR1VM_FindFunction(vm, "CSQC_UpdateView");
	if (f) {
		s_csqc.func_update = (int)(f - vm->functions);
	}
	f = PR1VM_FindFunction(vm, "CSQC_ConsoleCommand");
	if (f) {
		s_csqc.func_console = (int)(f - vm->functions);
	}
	f = PR1VM_FindFunction(vm, "CSQC_Shutdown");
	if (f) {
		s_csqc.func_shutdown = (int)(f - vm->functions);
	}
	f = PR1VM_FindFunction(vm, "CSQC_Ent_Update");
	if (f) {
		s_csqc.func_entupdate = (int)(f - vm->functions);
	}
	f = PR1VM_FindFunction(vm, "CSQC_Ent_Remove");
	if (f) {
		s_csqc.func_entremove = (int)(f - vm->functions);
	}
	f = PR1VM_FindFunction(vm, "CSQC_Ent_Spawn");
	if (f) {
		s_csqc.func_entspawn = (int)(f - vm->functions);
	}
	f = PR1VM_FindFunction(vm, "CSQC_Parse_Event");
	if (f) {
		s_csqc.func_parseevent = (int)(f - vm->functions);
	}
	f = PR1VM_FindFunction(vm, "CSQC_Input_Frame");
	if (f) {
		s_csqc.func_input = (int)(f - vm->functions);
	}
	f = PR1VM_FindFunction(vm, "CSQC_InputEvent");
	if (f) {
		s_csqc.func_inputevent = (int)(f - vm->functions);
	}
	f = PR1VM_FindFunction(vm, "StartFrame");
	if (f) {
		s_csqc.func_startframe = (int)(f - vm->functions);
	}
	f = PR1VM_FindFunction(vm, "EndFrame");
	if (f) {
		s_csqc.func_endframe = (int)(f - vm->functions);
	}

	s_csqc.global_time = PR1VM_FindGlobal(vm, "time");
	// gamespeed: QW has no game speed, so 1 is published (0 on server pause) -
	// see CSQC_Client_Update.
	s_csqc.global_gamespeed = PR1VM_FindGlobal(vm, "gamespeed");
	s_csqc.global_self = PR1VM_FindGlobal(vm, "self");
	// other is world for StartFrame/EndFrame/thinks; see CSQC_Client_RunFrameThink
	// for physics_mode.
	s_csqc.global_other = PR1VM_FindGlobal(vm, "other");
	s_csqc.global_physics_mode = PR1VM_FindGlobal(vm, "physics_mode");
	s_csqc.field_entnum = CSQC_Client_FindField(vm, "entnum");
	// #347: standard physics fields. Vector-written fields must be declared
	// ev_vector (see CSQC_Client_FindFieldVec).
	s_csqc.f_origin = CSQC_Client_FindFieldVec(vm, "origin");
	s_csqc.f_velocity = CSQC_Client_FindFieldVec(vm, "velocity");
	s_csqc.f_angles = CSQC_Client_FindFieldVec(vm, "angles");
	s_csqc.f_mins = CSQC_Client_FindFieldVec(vm, "mins");
	s_csqc.f_maxs = CSQC_Client_FindFieldVec(vm, "maxs");
	s_csqc.f_movetype = CSQC_Client_FindField(vm, "movetype");
	s_csqc.f_flags = CSQC_Client_FindField(vm, "flags");
	s_csqc.f_gravity = CSQC_Client_FindField(vm, "gravity");
	s_csqc.f_pmove_flags = CSQC_Client_FindField(vm, "pmove_flags");
	s_csqc.f_modelindex = CSQC_Client_FindField(vm, "modelindex");
	s_csqc.f_skin = CSQC_Client_FindField(vm, "skin");
	s_csqc.f_frame = CSQC_Client_FindField(vm, "frame");
	s_csqc.f_effects = CSQC_Client_FindField(vm, "effects");
	s_csqc.f_colormap = CSQC_Client_FindField(vm, "colormap");
	s_csqc.f_drawmask = CSQC_Client_FindField(vm, "drawmask");
	s_csqc.f_think = CSQC_Client_FindField(vm, "think");
	s_csqc.f_nextthink = CSQC_Client_FindField(vm, "nextthink");
	s_csqc.g_localentnum = PR1VM_FindGlobal(vm, "player_localentnum");

	s_csqc.in_timelength = PR1VM_FindGlobal(vm, "input_timelength");
	s_csqc.in_angles = CSQC_Client_FindGlobalVec(vm, "input_angles");
	s_csqc.in_movevalues = CSQC_Client_FindGlobalVec(vm, "input_movevalues");
	s_csqc.in_buttons = PR1VM_FindGlobal(vm, "input_buttons");
	s_csqc.in_impulse = PR1VM_FindGlobal(vm, "input_impulse");
	s_csqc.in_sequence = PR1VM_FindGlobal(vm, "input_sequence");
	s_csqc.g_ccframe = PR1VM_FindGlobal(vm, "clientcommandframe");
	s_csqc.g_scframe = PR1VM_FindGlobal(vm, "servercommandframe");
	s_csqc.p_org = CSQC_Client_FindGlobalVec(vm, "pmove_org");
	s_csqc.p_vel = CSQC_Client_FindGlobalVec(vm, "pmove_vel");
	s_csqc.p_onground = PR1VM_FindGlobal(vm, "pmove_onground");
	s_csqc.g_vfwd = CSQC_Client_FindGlobalVec(vm, "v_forward");
	s_csqc.g_vright = CSQC_Client_FindGlobalVec(vm, "v_right");
	s_csqc.g_vup = CSQC_Client_FindGlobalVec(vm, "v_up");
	s_csqc.g_view_angles = CSQC_Client_FindGlobalVec(vm, "view_angles");
	s_csqc.g_frametime = PR1VM_FindGlobal(vm, "frametime");
	s_csqc.g_cltime = PR1VM_FindGlobal(vm, "cltime");
	s_csqc.g_maxclients = PR1VM_FindGlobal(vm, "maxclients");
	s_csqc.g_player_localnum = PR1VM_FindGlobal(vm, "player_localnum");
	s_csqc.g_intermission = PR1VM_FindGlobal(vm, "intermission");

	f = PR1VM_FindFunction(vm, "CSQC_Parse_TempEntity");
	if (f) {
		s_csqc.func_parsetempent = (int)(f - vm->functions);
	}

	CSQC_Client_OffsetCacheResolve(vm);

	s_csqc.loaded = true;

	Con_DPrintf("CSQC: loaded %s (%d statements, crc=0x%x), funcs i=%d w=%d u=%d "
				"c=%d s=%d eu=%d er=%d pe=%d if=%d ie=%d time=%d\n",
		path, vm->progs->numstatements, (unsigned int)vm->progs->crc, s_csqc.func_init,
		s_csqc.func_world, s_csqc.func_update, s_csqc.func_console, s_csqc.func_shutdown,
		s_csqc.func_entupdate, s_csqc.func_entremove, s_csqc.func_parseevent,
		s_csqc.func_input, s_csqc.func_inputevent, s_csqc.global_time);
	// CSQC_Init(apiver, enginename, enginever); the module uses them only as hints.
	if (s_csqc.func_init > 0) {
		vm->globals[OFS_PARM0] = CSQC_API_VERSION;
		PR1VM_ClientSetString(vm, (string_t *)&vm->globals[OFS_PARM1], "ezQuake");
		vm->globals[OFS_PARM2] = VERSION_NUM;
		// World is writable during CSQC_Init.
		vm->world_writable = true;
		CSQC_Client_Exec(s_csqc.func_init);
		vm->world_writable = false;
	}
	// Availability does not depend on the optional CSQC_Init: "progs loaded and
	// not broken", so an Init-less module still receives entities/events/frames/
	// console. A failed CSQC_Init left errored=true, so inited stays false.
	s_csqc.inited = !s_csqc.errored;
	return true;
}

/*
=================
CSQC_Client_NotifyCSQC

Tell the server whether our module receives the CSQC stream. enablecsqc
- module loaded and ready (after CSQC_WorldLoaded); disablecsqc - server offered
CSQC but the module did not start. Idempotent (a repeated state is not sent); a
no-op in demos / without a connection (CL_SendClientCommand).
=================
*/
static void CSQC_Client_NotifyCSQC(qbool enable)
{
#ifdef FTE_PEXT_CSQC
	if (!(cls.fteprotocolextensions & FTE_PEXT_CSQC)) {
		return;
	}
#endif
	if (s_csqc.enable_sent && s_csqc.enable_value == enable) {
		return;
	}
	CL_SendClientCommand(true, enable ? "enablecsqc" : "disablecsqc");
	s_csqc.enable_sent = true;
	s_csqc.enable_value = enable;
	Con_DPrintf("CSQC: %s sent\n", enable ? "enablecsqc" : "disablecsqc");
}

// Signon hold (CSQC_Client_PreBegin). Kept outside s_csqc: CSQC_Client_Load clears
// that struct. Reset in CSQC_Client_Disconnect.
static struct
{
	qbool done; // module resolved for this map (loaded or definitively absent)
	qbool held; // `begin` is waiting for the csprogs download
} s_prebegin;

/*
=================
CSQC_Client_RunWorldLoaded

CSQC_WorldLoaded + enablecsqc, once per loaded module.
=================
*/
static void CSQC_Client_RunWorldLoaded(void)
{
	pr1vm_t *vm = &s_csqc.vm;

	if (s_csqc.world_done || !s_csqc.loaded || !s_csqc.inited || s_csqc.errored) {
		return;
	}
	s_csqc.world_done = true;
	// CSQC_WorldLoaded is optional: call it only when declared. A module that
	// errors inside it does not become ready.
	if (s_csqc.func_world > 0) {
		qbool ok;

		// World is writable until CSQC_WorldLoaded returns.
		vm->world_writable = true;
		ok = CSQC_Client_Exec(s_csqc.func_world);
		vm->world_writable = false;
		if (!ok) {
			return;
		}
	}
	// enablecsqc once per map for an available module, whether or not it
	// declares CSQC_WorldLoaded.
	CSQC_Client_NotifyCSQC(true);
}

/*
=================
CSQC_Client_LoadModule

Load + CSQC_Init + CSQC_WorldLoaded for a validated csprogs file; tells the server
enablecsqc/disablecsqc according to the outcome.
=================
*/
static void CSQC_Client_LoadModule(const char *path)
{
	// CSQC_Client_Load resets the module state; the signon hold must survive it.
	qbool held = s_prebegin.held;

	if (!CSQC_Client_Load(path) || !s_csqc.loaded || !s_csqc.inited || s_csqc.errored) {
		s_prebegin.held = held;
		CSQC_Client_NotifyCSQC(false);
		return;
	}
	s_prebegin.held = held;
	// A fresh module for this map: WorldLoaded/enablecsqc are this map's.
	s_csqc.world_done = false;
	s_csqc.enable_sent = false;
	CSQC_Client_RunWorldLoaded();
}

/*
=================
CSQC_Client_ResolveModule

If the server offers CSQC (*csprogssize) find a valid local csprogs and load it, or
request the download. Returns true when the outcome is final (loaded, not offered,
not available), false while the csprogs download is pending
(CSQC_Client_SignonFrame finishes it).
=================
*/
static qbool CSQC_Client_ResolveModule(void)
{
	extern cvar_t cl_download_csprogs;
	const char *name, *crcs;
	unsigned crc;
	int sizep;
	char path[MAX_QPATH];
	char *crcend;
	qbool anycsqc;

	// The module is loaded only when the session negotiated CSQC (the enabling
	// cvar is sampled when the declare mask is built on
	// connect, see CL_SupportedFTEExtensions) - a live cvar change must not load a
	// module the server never offered.
	if (!(cls.fteprotocolextensions & FTE_PEXT_CSQC)) {
		return true;
	}

	sizep = (int)strtoul(Info_ValueForKey(cl.serverinfo, "*csprogssize"), NULL, 0);

	// anycsqc: the server allows loading a local csprogs without crc check; a
	// corrupt *csprogs (trailing garbage) is treated the same. Demos are never
	// checked.
	anycsqc = atoi(Info_ValueForKey(cl.serverinfo, "anycsqc")) != 0;
	crcs = Info_ValueForKey(cl.serverinfo, "*csprogs");
	crc = (unsigned)strtoul(crcs, &crcend, 0);
	if (crcs[0] && *crcend) {
		Con_Printf("CSQC: corrupt *csprogs key in serverinfo\n");
		anycsqc = true;
		crc = 0;
	}
	if (cls.demoplayback) {
		anycsqc = true;
	}

	if (sizep <= 0 && !anycsqc) {
		return true;  // ordinary server without CSQC (or server PR1 gate)
	}

	name = Info_ValueForKey(cl.serverinfo, "*csprogsname");
	if (!name || !name[0]) {
		name = "csprogs.dat";
	}

	// The module is loaded fresh on every world entry; it is normally unloaded on
	// leaving the world (CL_ClearState). Unload defensively for paths that skip
	// CL_ClearState (a double call is a safe no-op).
	if (s_csqc.loaded) {
		CSQC_Client_Disconnect();
	}

	if (CSQC_Client_FindMainProgs(path, sizeof(path), name, sizep, crc, anycsqc)) {
		CSQC_Client_LoadModule(path);
		return true;
	}

	// Demo/MVD: no local csprogs, download is unavailable in demos (StartDownload is
	// a no-op) - do not set pending (otherwise a false timeout).
	if (cls.demoplayback) {
		Con_Printf("CSQC: no local csprogs for demo playback\n");
		return true;
	}

	// Download only for a non-empty, fully numeric *csprogs checksum; otherwise
	// tell the server we have no CSQC module.
	if (!CSQC_Client_CsprogsDlValid(crcs, crcend)) {
		Con_Printf("CSQC: not downloading csprogs (invalid *csprogs)\n");
		CSQC_Client_NotifyCSQC(false);
		return true;
	}

	if (!cl_download_csprogs.value) {
		Con_Printf("CSQC: not downloading %s (cl_download_csprogs 0)\n", name);
		CSQC_Client_NotifyCSQC(false);
		return true;
	}

	// No valid local one - download it (see CSQC_Client_StartDownload). The module
	// is loaded in CSQC_Client_SignonFrame once the file appears.
	s_csqc.csprogs_crc = crc;
	s_csqc.csprogs_size = sizep;
	CSQC_Client_DlPath(s_csqc.csprogs_dl_path, sizeof(s_csqc.csprogs_dl_path), crc);
	// May be deferred (returns false) when another download is in flight; the
	// per-frame CSQC_Client_SignonFrame retries it.
	s_dl.csprogs_dl_requested = CSQC_Client_StartDownload("csprogs.dat", s_csqc.csprogs_dl_path);
	s_dl.csprogs_dl_pending = true;
	return false;
}

/*
=================
CSQC_Client_PreBegin

Called when signon is complete and the client is about to send `begin` (end of the
skins stage: all content including the world model is loaded). The module is
resolved here, so that it exists before the server spawns us and starts sending
multicasts (unsized svc 83 cannot be skipped without a module). Order: csprogs
download, CSQC_Init, CSQC_WorldLoaded, enablecsqc, begin.

Returns true when `begin` may be sent now. false: the csprogs download is running
and CSQC_Client_SignonFrame sends `begin` once it completes, fails or times out.
=================
*/
qbool CSQC_Client_PreBegin(void)
{
	if (s_prebegin.done) {
		return true;
	}
	if (s_prebegin.held) {
		return false;
	}

	// cltime base for CSQC_Init/CSQC_WorldLoaded; set again on world entry.
	s_mapstarttime = cls.realtime;
	s_prev_cltime = cl.time;

	if (CSQC_Client_ResolveModule()) {
		s_prebegin.done = true;
		return true;
	}
	s_prebegin.held = (cls.state == ca_onserver);
	return !s_prebegin.held;
}

/*
=================
CSQC_Client_ReleaseBegin

The pending csprogs download reached a final outcome: send the `begin` that
CSQC_Client_PreBegin held back.
=================
*/
static void CSQC_Client_ReleaseBegin(void)
{
	qbool held = s_prebegin.held;

	s_prebegin.done = true;
	s_prebegin.held = false;
	if (held && cls.state == ca_onserver) {
		CL_SendClientCommand(true, "begin %i", cl.servercount);
	}
}

/*
=================
CSQC_Client_ConnectCheck

Called on entering the world (CL_MakeActive, before ca_active). The module is
normally resolved before `begin` (CSQC_Client_PreBegin); this is the fallback for
paths that become active without the skins stage.
=================
*/
void CSQC_Client_ConnectCheck(void)
{
	// cltime and frametime bases, set on each world entry.
	s_mapstarttime = cls.realtime;
	s_prev_cltime = cl.time;

	if (!s_prebegin.done && !s_dl.csprogs_dl_pending) {
		s_prebegin.done = CSQC_Client_ResolveModule();
	}
}

/*
=================
EXT_CSQC_1 prediction window - the module globals clientcommandframe /
servercommandframe.

- clientcommandframe = the live (last built) client frame = s_ccframe. Set in
  CSQC_Client_InputFrame at cmd build and not recomputed after sending:
  Netchan_Transmit increments outgoing_sequence, but clientcommandframe stays the
  built cmd's number. This is what #345(clientcommandframe) asks for - the live
  pending frame; the next one does not exist yet.
- servercommandframe = the last server-acked client frame = cl.parsecount.
- Prediction unavailable (0): demo/MVD, not ca_active, before the first accepted
  server frame (cl.validsequence == 0).
- The window (servercommandframe, clientcommandframe] is the module's contract (the
  engine does not check it; see ext_csqc_1 spec) - see CSQC_Client_ApplyInput.
=================
*/
static float CSQC_Client_ClientCmdFrame(void)
{
	if (!s_csqc.loaded || s_csqc.errored) {
		return 0;
	}
	if (cls.state != ca_active || cls.demoplayback || cls.mvdplayback) {
		return 0;
	}
	// The last built frame, not the next outgoing_sequence (see s_ccframe).
	return (float)s_ccframe;
}

static float CSQC_Client_ServerCmdFrame(void)
{
	if (!s_csqc.loaded || s_csqc.errored) {
		return 0;
	}
	if (cls.state != ca_active || cls.demoplayback || cls.mvdplayback) {
		return 0;
	}
	if (!cl.validsequence) {
		return 0; // no accepted server frame yet (prespawn)
	}
	return (float)cl.parsecount;
}

// Publish the prediction window; called before CSQC_UpdateView and before
// CSQC_Input_Frame.
static void CSQC_Client_PatchFrames(void)
{
	if (!s_csqc.loaded || !s_csqc.inited || s_csqc.errored) {
		return;
	}
	if (s_csqc.g_ccframe >= 0) {
		s_csqc.vm.globals[s_csqc.g_ccframe] = CSQC_Client_ClientCmdFrame();
	}
	if (s_csqc.g_scframe >= 0) {
		s_csqc.vm.globals[s_csqc.g_scframe] = CSQC_Client_ServerCmdFrame();
	}
}

/*
=================
CSQC think-loop - the per-frame module lifecycle in 3D takeover.

StartFrame -> thinks (.nextthink/.think, single-think NQ-style) -> EndFrame.
StartFrame/EndFrame: self/other = world, time = frame time. think: self = entity,
other = world, time = frame time, nextthink is zeroed before the call. thinks run
only when physics_mode != 0. Not implemented: movetype physics (physics_mode 2),
customphysics and threads (PR1VM has no sleep/fork).
=================
*/
static void CSQC_Client_RunFrameThink(void)
{
	pr1vm_t *vm = &s_csqc.vm;
	int mode, slot;
	float t, frame;

	if (!s_csqc.loaded || !s_csqc.inited || s_csqc.errored) {
		return;
	}

	// Base is the module time, window is the frame interval.
	CSQC_Client_SetTime();
	t = (s_csqc.global_time >= 0) ? vm->globals[s_csqc.global_time] : 0;
	frame = (float)cls.frametime;
	if (frame < 0) {
		frame = 0;
	}

	// physics_mode 0 = original CSQC: only StartFrame/EndFrame, no engine-run
	// .think loop. An undeclared physics_mode means 0.
	mode = (s_csqc.global_physics_mode >= 0) ? (int)vm->globals[s_csqc.global_physics_mode] : 0;

	if (s_csqc.func_startframe > 0) {
		if (s_csqc.global_self >= 0) {
			*(int *)&vm->globals[s_csqc.global_self] = 0;
		}
		if (s_csqc.global_other >= 0) {
			*(int *)&vm->globals[s_csqc.global_other] = 0;
		}
		if (s_csqc.global_time >= 0) {
			vm->globals[s_csqc.global_time] = t;
		}
		PR1VM_ExecuteProgram(vm, (func_t)s_csqc.func_startframe);
		if (s_csqc.errored) {
			return;
		}
	}

	// thinks for mode 1 and 2 (mode 2 movetypes are not run). Slot 0 = world,
	// free slots are skipped.
	if (mode != 0 && s_csqc.f_think >= 0 && s_csqc.f_nextthink >= 0) {
		for (slot = 1; slot < CSQC_MAX_EDICTS; slot++) {
			float *base, nt;
			int thinkfunc;

			if (!s_used[slot]) {
				continue;
			}
			base = (float *)((byte *)vm->game_edicts + (size_t)slot * vm->edict_size);
			nt = base[s_csqc.f_nextthink];
			if (nt <= 0 || nt > t + frame) {
				continue;
			}
			thinkfunc = *(int *)&base[s_csqc.f_think];
			base[s_csqc.f_nextthink] = 0;
			if (thinkfunc <= 0) {
				continue; // nextthink without think
			}
			if (s_csqc.global_self >= 0) {
				*(int *)&vm->globals[s_csqc.global_self] = (int)slot * vm->edict_size;
			}
			if (s_csqc.global_other >= 0) {
				*(int *)&vm->globals[s_csqc.global_other] = 0;
			}
			if (s_csqc.global_time >= 0) {
				vm->globals[s_csqc.global_time] = t;
			}
			PR1VM_ExecuteProgram(vm, (func_t)thinkfunc);
			if (s_csqc.errored) {
				return;
			}
		}
	}

	if (s_csqc.func_endframe > 0) {
		if (s_csqc.global_self >= 0) {
			*(int *)&vm->globals[s_csqc.global_self] = 0;
		}
		if (s_csqc.global_other >= 0) {
			*(int *)&vm->globals[s_csqc.global_other] = 0;
		}
		if (s_csqc.global_time >= 0) {
			vm->globals[s_csqc.global_time] = t;
		}
		PR1VM_ExecuteProgram(vm, (func_t)s_csqc.func_endframe);
	}
}

/*
=================
CSQC_Client_NotMenu / CSQC_Client_KeynumToQC / CSQC_Client_QCToKeynum

The internal ezq domain is keys.h (K_*, K_MOUSE1=200, K_MWHEELUP=244); the QC/CSQC
contract is DP numbering. The module receives/returns only QC codes; unknown keys
pass as the negated engine keynum, so the round-trip is preserved.
=================
*/
qbool CSQC_Client_NotMenu(void)
{
	// Any layered menu hides the game; console/chat/startup demo stay notmenu=1.
	return !(key_dest == key_menu || key_dest == key_hudeditor || key_dest == key_demo_controls || key_dest == key_startupdemo_menu);
}

int CSQC_Client_KeynumToQC(int keynum)
{
	switch (keynum) {
		case K_TAB: return 9;
		case K_ENTER: return 13;
		case K_ESCAPE: return 27;
		case K_SPACE: return 32;
		case K_BACKSPACE: return 127;

		case K_UPARROW: return 128;
		case K_DOWNARROW: return 129;
		case K_LEFTARROW: return 130;
		case K_RIGHTARROW:
			return 131;

			// Both sides of the modifiers map to the same QC code, so a module's
			// `scanx == K_SHIFT` matches the right Shift too.
		case K_LALT:
		case K_RALT: return 132;
		case K_LCTRL:
		case K_RCTRL: return 133;
		case K_LSHIFT:
		case K_RSHIFT: return 134;

		case K_F1: return 135;
		case K_F2: return 136;
		case K_F3: return 137;
		case K_F4: return 138;
		case K_F5: return 139;
		case K_F6: return 140;
		case K_F7: return 141;
		case K_F8: return 142;
		case K_F9: return 143;
		case K_F10: return 144;
		case K_F11: return 145;
		case K_F12: return 146;

		case K_INS: return 147;
		case K_DEL: return 148;
		case K_PGDN: return 149;
		case K_PGUP: return 150;
		case K_HOME: return 151;
		case K_END: return 152;
		case K_PAUSE: return 153;

		case KP_NUMLOCK: return 154;
		case K_CAPSLOCK: return 155;
		case K_SCRLCK: return 156;

		case KP_INS: return 157;
		case KP_END: return 158;
		case KP_DOWNARROW: return 159;
		case KP_PGDN: return 160;
		case KP_LEFTARROW: return 161;
		case KP_5: return 162;
		case KP_RIGHTARROW: return 163;
		case KP_HOME: return 164;
		case KP_UPARROW: return 165;
		case KP_PGUP: return 166;
		case KP_DEL: return 167;
		case KP_SLASH: return 168;
		case KP_STAR: return 169;
		case KP_MINUS: return 170;
		case KP_PLUS: return 171;
		case KP_ENTER: return 172;

		case K_PRINTSCR:
			return 174;

			// mouse: DP interleaves the wheels between MOUSE3 and MOUSE4.
		case K_MOUSE1: return 512;
		case K_MOUSE2: return 513;
		case K_MOUSE3: return 514;
		case K_MWHEELUP: return 515;
		case K_MWHEELDOWN: return 516;
		case K_MOUSE4: return 517;
		case K_MOUSE5: return 518;
		case K_MOUSE6: return 519;
		case K_MOUSE7: return 520;
		case K_MOUSE8: return 521;

		case K_JOY1: return 768;
		case K_JOY2: return 769;
		case K_JOY3: return 770;
		case K_JOY4:
			return 771;

			// K_AUX1..16 -> 800..815 (FTE's key codes; some csdefs declare
			// 784..799 instead).
		case K_AUX1: return 800;
		case K_AUX2: return 801;
		case K_AUX3: return 802;
		case K_AUX4: return 803;
		case K_AUX5: return 804;
		case K_AUX6: return 805;
		case K_AUX7: return 806;
		case K_AUX8: return 807;
		case K_AUX9: return 808;
		case K_AUX10: return 809;
		case K_AUX11: return 810;
		case K_AUX12: return 811;
		case K_AUX13: return 812;
		case K_AUX14: return 813;
		case K_AUX15: return 814;
		case K_AUX16: return 815;

		default:
			if (keynum == -1) {   // module passed "no key"
				return keynum;
			}
			if (keynum < 0) {    // already a native negative code
				return -keynum;
			}
			if (keynum >= 0 && keynum < 128) { // printable/control ascii - identity
				return keynum;
			}
			return -keynum;   // no QC equivalent - native code
	}
}

int CSQC_Client_QCToKeynum(int code)
{
	switch (code) {
		case 9: return K_TAB;
		case 13: return K_ENTER;
		case 27: return K_ESCAPE;
		case 32: return K_SPACE;
		case 127: return K_BACKSPACE;

		case 128: return K_UPARROW;
		case 129: return K_DOWNARROW;
		case 130: return K_LEFTARROW;
		case 131: return K_RIGHTARROW;

		case 132: return K_LALT;
		case 133: return K_LCTRL;
		case 134: return K_LSHIFT;

		case 135: return K_F1;
		case 136: return K_F2;
		case 137: return K_F3;
		case 138: return K_F4;
		case 139: return K_F5;
		case 140: return K_F6;
		case 141: return K_F7;
		case 142: return K_F8;
		case 143: return K_F9;
		case 144: return K_F10;
		case 145: return K_F11;
		case 146: return K_F12;

		case 147: return K_INS;
		case 148: return K_DEL;
		case 149: return K_PGDN;
		case 150: return K_PGUP;
		case 151: return K_HOME;
		case 152: return K_END;
		case 153: return K_PAUSE;

		case 154: return KP_NUMLOCK;
		case 155: return K_CAPSLOCK;
		case 156: return K_SCRLCK;

		case 157: return KP_INS;
		case 158: return KP_END;
		case 159: return KP_DOWNARROW;
		case 160: return KP_PGDN;
		case 161: return KP_LEFTARROW;
		case 162: return KP_5;
		case 163: return KP_RIGHTARROW;
		case 164: return KP_HOME;
		case 165: return KP_UPARROW;
		case 166: return KP_PGUP;
		case 167: return KP_DEL;
		case 168: return KP_SLASH;
		case 169: return KP_STAR;
		case 170: return KP_MINUS;
		case 171: return KP_PLUS;
		case 172: return KP_ENTER;

		case 174: return K_PRINTSCR;

		case 512: return K_MOUSE1;
		case 513: return K_MOUSE2;
		case 514: return K_MOUSE3;
		case 515: return K_MWHEELUP;
		case 516: return K_MWHEELDOWN;
		case 517: return K_MOUSE4;
		case 518: return K_MOUSE5;
		case 519: return K_MOUSE6;
		case 520: return K_MOUSE7;
		case 521: return K_MOUSE8;

		case 768: return K_JOY1;
		case 769: return K_JOY2;
		case 770: return K_JOY3;
		case 771: return K_JOY4;

		case 800: return K_AUX1;
		case 801: return K_AUX2;
		case 802: return K_AUX3;
		case 803: return K_AUX4;
		case 804: return K_AUX5;
		case 805: return K_AUX6;
		case 806: return K_AUX7;
		case 807: return K_AUX8;
		case 808: return K_AUX9;
		case 809: return K_AUX10;
		case 810: return K_AUX11;
		case 811: return K_AUX12;
		case 812: return K_AUX13;
		case 813: return K_AUX14;
		case 814: return K_AUX15;
		case 815: return K_AUX16;

		default:
			if (code == -1) {    // module passed "no key"
				return code;
			}
			if (code < 0) {    // native code - back
				return -code;
			}
			if (code >= 0 && code < 128) { // printable/control ascii - identity
				return code;
			}
			return -code;    // no ezq equivalent
	}
}

/*
=================
CSQC_Client_PublishSimGlobals

Engine-maintained module globals - frametime, cltime, maxclients, player_localnum,
intermission. Published each frame before CSQC_UpdateView. frametime - client time
delta; cltime - client map-uptime.
=================
*/
static void CSQC_Client_PublishSimGlobals(void)
{
	pr1vm_t *vm = &s_csqc.vm;

	if (s_csqc.g_frametime >= 0) {
		vm->globals[s_csqc.g_frametime] = cl.paused ? 0 : (float)bound(0, cl.time - s_prev_cltime, 0.1);
	}
	if (s_csqc.g_cltime >= 0) {
		vm->globals[s_csqc.g_cltime] = (float)(cls.realtime - s_mapstarttime);
	}
	if (s_csqc.g_maxclients >= 0) {
		// The number of client slots: the serverinfo value, or MAX_CLIENTS when
		// absent/invalid - never 0 on a live connection.
		const char *mc = Info_ValueForKey(cl.serverinfo, "maxclients");
		int n = (mc && mc[0]) ? atoi(mc) : 0;
		if (n <= 0) {
			n = MAX_CLIENTS;
		}
		vm->globals[s_csqc.g_maxclients] = (float)bound(1, n, MAX_CLIENTS);
	}
	if (s_csqc.g_player_localnum >= 0) {
		// Own client slot - not the chase/spectator-tracked view player
		// (cl.viewplayernum).
		int n = (cl.playernum >= 0 && cl.playernum < MAX_CLIENTS) ? cl.playernum : 0;
		vm->globals[s_csqc.g_player_localnum] = (float)n;
	}
	if (s_csqc.g_intermission >= 0) {
		vm->globals[s_csqc.g_intermission] = !!cl.intermission;
	}

	s_prev_cltime = cl.time;
}

// Once-per-frame guard for CSQC_Client_Update: under multiview the 3D phase
// (SCR_UpdateScreenPlayerView) runs once per player view, but CSQC_UpdateView must
// run once per redraw. host_screenupdatecount
// is bumped once per frame in SCR_UpdateScreenPrePlayerView, before the multiview
// loop, so it is stable across the per-view iterations.
static int s_update_screencount = -1;

/*
=================
CSQC_Client_SignonFrame

Called every client frame (CL_Frame), independent of connection state and screen
updates: sends queued module downloads and finishes a pending csprogs download. A
valid file -> load the module; failure / no progress -> give up and tell the server
disablecsqc. Every final
outcome releases the `begin` held by CSQC_Client_PreBegin.
=================
*/
void CSQC_Client_SignonFrame(void)
{
	double now;
	qbool ours;

	// Module precache downloads: one request per frame (single slot). Also before
	// ca_active, since CSQC_Init runs before `begin` and may queue precaches.
	if (cls.state >= ca_connected) {
		CSQC_Client_FlushDownloadQueue();
	}

	if (!s_dl.csprogs_dl_pending) {
		return;
	}
	if (cls.state < ca_connected) {
		s_dl.csprogs_dl_pending = false;
		return;
	}

	if (CSQC_Client_ValidateFile(s_csqc.csprogs_dl_path, s_csqc.csprogs_size, s_csqc.csprogs_crc)) {
		char path[MAX_QPATH];

		strlcpy(path, s_csqc.csprogs_dl_path, sizeof(path));
		s_dl.csprogs_dl_pending = false;
		CSQC_Client_LoadModule(path);
		CSQC_Client_ReleaseBegin();
		return;
	}

	// Timeout by absence of progress: a flat limit from the start would give up
	// on slow links even though the download is running.
	now = Sys_DoubleTime();
	ours = cls.download && !strcmp(cls.downloadname, s_dl.csprogs_dl_localname);

	if (ours) {
		int pct = (int)cls.downloadpercent;

		if (pct > s_dl.csprogs_dl_percent) {
			s_dl.csprogs_dl_percent = pct;
			s_dl.csprogs_dl_lastprogress = now;
		}
		s_dl.csprogs_dl_started = true;
		return;
	}
	if (s_dl.csprogs_dl_started) {
		// our download finished without a valid file (failure/cancel)
		Con_Printf("CSQC: csprogs download failed\n");
	}
	else if (!s_dl.csprogs_dl_requested) {
		// The request is not out yet: the download slot was busy with another
		// transfer. Retry once it is free and keep the no-progress window from
		// expiring while we defer.
		if (!cls.download) {
			s_dl.csprogs_dl_requested = CSQC_Client_StartDownload("csprogs.dat", s_csqc.csprogs_dl_path);
		}
		s_dl.csprogs_dl_lastprogress = now;
		return;
	}
	else if (now - s_dl.csprogs_dl_lastprogress > 20) {
		cls.downloadrequested = false;
		Con_Printf("CSQC: csprogs download timed out (no progress)\n");
	}
	else {
		return;
	}

	s_dl.csprogs_dl_pending = false;
	CSQC_Client_NotifyCSQC(false);
	CSQC_Client_ReleaseBegin();
}

/*
=================
CSQC_Client_Frame

Once per client frame, when the entities for the frame are linked
(CL_LinkEntities), whether or not anything is drawn: publishes the per-frame
globals and runs the deltalisten bridges.
=================
*/
void CSQC_Client_Frame(void)
{
	static int last_framecount = -1;
	pr1vm_t *vm = &s_csqc.vm;

	// CL_LinkEntities runs once per view (multiview); the module gets one frame.
	if (last_framecount == cls.framecount) {
		return;
	}
	last_framecount = cls.framecount;

	// No callback from a previous frame may leave the phase flag set.
	vm->world_writable = false;

	if (cls.state != ca_active) {
		return;
	}
	if (!s_csqc.loaded || !s_csqc.inited || s_csqc.errored) {
		return;
	}

	CSQC_Client_UpdateLocalEntnum();
	CSQC_Client_PatchFrames();
	CSQC_Client_PublishViewAngles();
	// QW has no game speed: 1, or 0 on server pause.
	if (s_csqc.global_gamespeed >= 0) {
		s_csqc.vm.globals[s_csqc.global_gamespeed] = !(cl.paused & PAUSED_SERVER);
	}
	CSQC_Client_PublishSimGlobals();

	CSQC_Client_DeltaPlayers(vm);
	if (!s_csqc.errored) {
		CSQC_Client_DeltaEntities(vm);
	}

	// With an active CSQC cursor, send the absolute mouse position to the module
	// only when it changed since last frame.
	{
		static float ie_abs_lastx = -1, ie_abs_lasty = -1;
		float cmx = 0, cmy = 0;

		CSQC_Client_GetCursorPos(&cmx, &cmy);
		if (CSQC_Client_CSQCCursor()) {
			if (cmx != ie_abs_lastx || cmy != ie_abs_lasty) {
				ie_abs_lastx = cmx;
				ie_abs_lasty = cmy;
				CSQC_Client_InputEvent(IE_MOUSEABS, cmx, cmy, 0);
			}
		}
		else {
			ie_abs_lastx = ie_abs_lasty = -1; // cursor removed - reset
		}
	}
}

/*
=================
CSQC_Client_Update

The module's view, once per screen update, called in the 3D phase for a module
that has CSQC_UpdateView (CSQC_Client_SceneActive): StartFrame, thinks and
EndFrame, then CSQC_UpdateView(vid.width, vid.height, notmenu). A module without
CSQC_UpdateView has no view and gets no thinks. What does not depend on drawing
is in CSQC_Client_Frame.
=================
*/
void CSQC_Client_Update(void)
{
	pr1vm_t *vm = &s_csqc.vm;

	// No callback from a previous frame may leave the phase flag set.
	vm->world_writable = false;

	if (cls.state != ca_active) {
		return;
	}

	if (s_update_screencount == host_screenupdatecount) {
		return;
	}
	s_update_screencount = host_screenupdatecount;

	s_clip_on = false;

	if (!s_csqc.loaded || !s_csqc.inited || s_csqc.errored) {
		return;
	}

	if (s_csqc.func_update > 0) {
		CSQC_Client_RunFrameThink();
		if (s_csqc.errored) {
			return;
		}

		// #351: the listener applies only if the module set it this frame
		// (otherwise the engine view).
		s_listener_on = false;
		vm->globals[OFS_PARM0] = vid.width;
		vm->globals[OFS_PARM1] = vid.height;
		vm->globals[OFS_PARM2] = CSQC_Client_NotMenu();
		CSQC_Client_Exec(s_csqc.func_update);
	}
}

/*
=================
CSQC_Client_ParseAllowed

Runtime gate of the CSQC parsers: FTE_PEXT_CSQC as negotiated at connect. The
negotiated bit decides the wire layout for the whole session; the cl_pext_csqc
cvar is only sampled when the client declares its mask, never read live.
=================
*/
qbool CSQC_Client_ParseAllowed(void)
{
#ifdef FTE_PEXT_CSQC
	return (cls.fteprotocolextensions & FTE_PEXT_CSQC) != 0;
#else
	return false;
#endif
}

/*
=================
CSQC_Client_MayRead

The module's read*-builtins are allowed only inside parse callbacks (CSQC_Ent_Update
/ CSQC_Parse_Event); outside them they abort the module (CSQC_Client_Abort).
=================
*/
qbool CSQC_Client_MayRead(void)
{
	return s_csqc.mayread;
}

/*
=================
CSQC_Client_SizedRewind

Sized-message guard: after handling a payload, align
msg_readcount to the declared end payload_start+payload_len, for both under-read
(pad) and over-read (rewind). MSG_ReadSkip is forward-only in ezq, so over-read is
rewound by writing msg_readcount directly. A degenerate (negative) length is
clamped to 0 so the pointer can never move behind payload_start - otherwise a
server-controlled >=0x8000 length would cause an out-of-bounds read and a parser
loop. The clamp is defensive: the callers read unsigned lengths (svc 90/91) or
gate payload_len >= 0 (svc 92).
=================
*/
void CSQC_Client_SizedRewind(int payload_start, int payload_len)
{
	int used = msg_readcount - payload_start;

	if (payload_len < 0) {
		payload_len = 0;
	}

	if (used < payload_len) {
		MSG_ReadSkip(payload_len - used);
	}
	else if (used > payload_len) {
		int target = payload_start + payload_len;

		if (target < payload_start) {
			target = payload_start;
		}
		msg_readcount = target;
	}
}

/*
=================
CSQC_Client_ParsePrecacheMsg

FTE svcfte_precache (77): [short idx|type][string name]. Register the index->name
mapping without downloading (model/sound); particle/unused are drained.
=================
*/
void CSQC_Client_ParsePrecacheMsg(void)
{
	int code = (unsigned short)MSG_ReadShort();
	const char *name = MSG_ReadString();
	int ptype = code & SVCFTE_PC_TYPE;
	int pidx = code & ~SVCFTE_PC_TYPE;

	// An empty name would reach Mod_FindName("") -> Sys_Error. Distinct names
	// are capped too: mod_known slots are never freed and 4096 distinct names
	// exit the client.
	if (!name || !name[0]) {
		return;
	}

	if (ptype == SVCFTE_PC_MODEL) {
		if (pidx >= 1 && pidx < MAX_MODELS) {
			struct model_s *m;
			if (!CSQC_Client_AllowWireModelName(name)) {
				return;
			}
			strlcpy(cl.model_name[pidx], name, sizeof(cl.model_name[pidx]));
			// Resolve the name to a model (or a stub) so a later
			// packet entity with this server index does not hit
			// Host_Error("bad modelindex") on a NULL cl.model_precache[]. A found
			// non-model file yields a stub instead of aborting.
			m = Mod_ForNameTolerant(name, false);
			if (!m) {
				// missing file: non-NULL placeholder + request the download.
				m = Mod_ForNameTolerant(name, true);
				if (name[0] != '*') {
					CSQC_Client_RequestDownload(name);
				}
			}
			cl.model_precache[pidx] = m;
		}
	}
	else if (ptype == SVCFTE_PC_SOUND) {
		// Without S_PrecacheSound cl.sound_precache[] stays NULL and a later
		// svc_sound with this index is silent. Cap the distinct module names:
		// MAX_SFX is 512 and S_FindName Sys_Errors.
		if (pidx >= 1 && pidx < MAX_SOUNDS && CSQC_Client_AllowModuleSound(name)) {
			strlcpy(cl.sound_name[pidx], name, sizeof(cl.sound_name[pidx]));
			cl.sound_precache[pidx] = S_PrecacheSound(cl.sound_name[pidx]);
		}
	}
}

/*
=================
CSQC_Client_DrainTrailMsg

FTE svcfte_trailparticles (80): [ent short][short effect][coord x6]. Drained only
(no visual). The entity is a plain short: ezq does not negotiate
PEXT2_REPLACEMENTDELTAS, which would change the entity encoding.
=================
*/
void CSQC_Client_DrainTrailMsg(void)
{
	int i;

	MSG_ReadShort();    // entity number
	MSG_ReadShort();    // effect index
	for (i = 0; i < 6; i++) {
		MSG_ReadCoord();
	}
}

/*
=================
CSQC_Client_DrainPointMsg

FTE svcfte_pointparticles (81) / pointparticles1 (82). 81: [short effect][coord x6]
[short count]; 82 (compact): [short effect][coord x3].
=================
*/
void CSQC_Client_DrainPointMsg(qbool compact)
{
	int i;

	MSG_ReadShort();    // effect index
	for (i = 0; i < (compact ? 3 : 6); i++) {
		MSG_ReadCoord();
	}
	if (!compact) {
		MSG_ReadShort();   // count
	}
}

/*
=================
CSQC_Client_DrainTempEntSizedMsg

FTE svcfte_temp_entity_sized (91): [short len][payload]. The payload is a normal
temp-entity stream, parsed by the engine and then aligned with the sized guard.
=================
*/
void CSQC_Client_DrainTempEntSizedMsg(void)
{
	// Unsigned length; a signed read would let a server-controlled >=0x8000
	// length rewind the parser behind the payload (out-of-bounds read + parser
	// loop).
	int payload_len = (unsigned short)MSG_ReadShort();
	int payload_start = msg_readcount;

	CL_ParseTEnt();
	CSQC_Client_SizedRewind(payload_start, payload_len);
}

#ifdef CSQC_DEBUG
static void CSQC_Client_NetProbe_f(void);

static void CSQC_NetProbe_WriteShort(byte *buf, int *n, int v)
{
	buf[(*n)++] = (byte)(v & 0xff);
	buf[(*n)++] = (byte)((v >> 8) & 0xff);
}

/*
=================
CSQC_Client_NetProbe_f

Debug canary (client console `csqc_netprobe`): feeds synthetic wire bodies through
the same receive helpers cl_parse.c uses for the FTE-CSQC messages absent from
qwprot (77/80/81/82/91) and the sized entities drain (92), and checks exact byte
consumption / no badread. No server or module required.
=================
*/
#define NETPROBE_RUN(label, call, expect) \
	do { \
		sizebuf_t _s = net_message; \
		int _rc = msg_readcount; \
		qbool _bad = msg_badread; \
		net_message.data = buf; \
		net_message.cursize = n; \
		msg_readcount = 0; \
		msg_badread = false; \
		call; \
		PR1VM_GuardCheck (label, !msg_badread && msg_readcount == (expect), &pass, &fail); \
		net_message = _s; \
		msg_readcount = _rc; \
		msg_badread = _bad; \
	} while (0)

static void CSQC_Client_NetProbe_f(void)
{
	int pass = 0, fail = 0;
	byte buf[128];
	int n;

	// 77 precache: [short idx|type][string], model index 5.
	{
		const char *nm = "progs/test.mdl";
		n = 0;
		CSQC_NetProbe_WriteShort(buf, &n, 5 | SVCFTE_PC_MODEL);
		memcpy(buf + n, nm, strlen(nm) + 1);
		n += (int)strlen(nm) + 1;
		NETPROBE_RUN("netprobe 77 precache", CSQC_Client_ParsePrecacheMsg(), n);
	}
	// 80 trail: [ent short][short effect][coord x6].
	{
		int i;
		n = 0;
		CSQC_NetProbe_WriteShort(buf, &n, 3);
		CSQC_NetProbe_WriteShort(buf, &n, 7);
		for (i = 0; i < 6; i++) {
			CSQC_NetProbe_WriteShort(buf, &n, 100 + i);
		}
		NETPROBE_RUN("netprobe 80 trail", CSQC_Client_DrainTrailMsg(), n);
	}
	// 81 point (full): [short effect][coord x6][short count].
	{
		int i;
		n = 0;
		CSQC_NetProbe_WriteShort(buf, &n, 7);
		for (i = 0; i < 6; i++) {
			CSQC_NetProbe_WriteShort(buf, &n, 200 + i);
		}
		CSQC_NetProbe_WriteShort(buf, &n, 4);
		NETPROBE_RUN("netprobe 81 point", CSQC_Client_DrainPointMsg(false), n);
	}
	// 82 point1 (compact): [short effect][coord x3].
	{
		int i;
		n = 0;
		CSQC_NetProbe_WriteShort(buf, &n, 7);
		for (i = 0; i < 3; i++) {
			CSQC_NetProbe_WriteShort(buf, &n, 300 + i);
		}
		NETPROBE_RUN("netprobe 82 point1", CSQC_Client_DrainPointMsg(true), n);
	}
	// 91 temp_entity_sized: [short len][payload len].
	{
		n = 0;
		CSQC_NetProbe_WriteShort(buf, &n, 4);
		buf[n++] = 1;
		buf[n++] = 2;
		buf[n++] = 3;
		buf[n++] = 4;
		NETPROBE_RUN("netprobe 91 tent-sized", CSQC_Client_DrainTempEntSizedMsg(), n);
	}
	// 91 sized guard, over-read: module-less path drains exactly len.
	{
		n = 0;
		CSQC_NetProbe_WriteShort(buf, &n, 2);
		buf[n++] = 9;
		buf[n++] = 9;
		NETPROBE_RUN("netprobe 91 sized-guard", CSQC_Client_DrainTempEntSizedMsg(), n);
	}
	// 91 degenerate length (0xFFFD): the unsigned read must not rewind behind
	// the payload start; the oversized forward skip terminates via msg_badread.
	{
		n = 0;
		CSQC_NetProbe_WriteShort(buf, &n, 0xFFFD);
		buf[n++] = 1;
		buf[n++] = 2;
		{
			sizebuf_t _s = net_message;
			int _rc = msg_readcount;
			qbool _bad = msg_badread;
			net_message.data = buf;
			net_message.cursize = n;
			msg_readcount = 0;
			msg_badread = false;
			CSQC_Client_DrainTempEntSizedMsg();
			PR1VM_GuardCheck("netprobe 91 degenerate no-backward", msg_readcount >= 2 && msg_badread, &pass, &fail);
			net_message = _s;
			msg_readcount = _rc;
			msg_badread = _bad;
		}
	}
	// SizedRewind with a negative length: clamp, never move behind payload_start.
	{
		sizebuf_t _s = net_message;
		int _rc = msg_readcount;
		qbool _bad = msg_badread;
		n = 4;
		buf[0] = 1;
		buf[1] = 2;
		buf[2] = 3;
		buf[3] = 4;
		net_message.data = buf;
		net_message.cursize = n;
		msg_readcount = 3;
		msg_badread = false;
		CSQC_Client_SizedRewind(2, -3);
		PR1VM_GuardCheck("netprobe sized-rewind negative clamp", msg_readcount >= 2 && !msg_badread, &pass, &fail);
		net_message = _s;
		msg_readcount = _rc;
		msg_badread = _bad;
	}
	// 92 csqcentities_sized (walked without module): [ent][len][payload]...[0].
	{
		n = 0;
		CSQC_NetProbe_WriteShort(buf, &n, 1);
		CSQC_NetProbe_WriteShort(buf, &n, 3);
		buf[n++] = 1;
		buf[n++] = 2;
		buf[n++] = 3;
		CSQC_NetProbe_WriteShort(buf, &n, 0);
		NETPROBE_RUN("netprobe 92 entities", CSQC_Client_ParseEntities(true), n);
	}

	Con_Printf("[CSQC-NETPROBE] pass=%d fail=%d\n", pass, fail);
}

#undef NETPROBE_RUN
#endif // CSQC_DEBUG

/*
=================
CSQC_Client_ParseEntities

Parsing of svc_fte_csqcentities(76)/sized(92): for each entity - a short entnum,
bit 0x8000 = remove, 0 = end. Update: CSQC_Ent_Update(isnew) - the module reads the
payload from the current message (read*); the entity context (self/.entnum) is set
by the engine before the call. Remove: CSQC_Ent_Remove with self/.entnum (no builtin
stream). Sized (92, only mvdsv under sv_csqcdebug): before each update entity's
payload there is a short length - a skip guard against desync.
=================
*/
void CSQC_Client_ParseEntities(qbool sized)
{
	pr1vm_t *vm = &s_csqc.vm;
	unsigned int entnum;
	qbool removeflag;
	qbool ready;
	int isnew;
	int ctxslot;
	unsigned int ctxseq;
	// csqc_dbg is a module-owned cvar (registercvar #93); it can be unset/recreated
	// between frames, so resolve it per call instead of caching a pointer that can
	// dangle.
	cvar_t *dbg = Cvar_Find("csqc_dbg");

	// Publish the prediction frames before the entity loop, so CSQC_Ent_Update
	// sees the current parse-time frames (servercommandframe == cl.parsecount)
	// rather than the previous render frame.
	CSQC_Client_PatchFrames();

	// Runtime gate + a live module. A sized stream can be walked without a module
	// (only the wire fields entnum/len are needed); a non-sized one cannot - the
	// payload length is unknown without executing the module, so this is a
	// protocol error, not a silent desync.
	ready = CSQC_Client_ParseAllowed() && s_csqc.loaded && s_csqc.inited && !s_csqc.errored && (s_csqc.func_entupdate > 0 || s_csqc.func_entremove > 0);
	if (!ready && !sized) {
		// A module that already errored is disabled and disablecsqc has been sent;
		// the datagram cannot be skipped (no length), so drop it instead of turning
		// one bad callback into a disconnect.
		if (s_csqc.errored) {
			msg_readcount = net_message.cursize;
			return;
		}
		Host_Error("CSQC_Client_ParseEntities: svc_fte_csqcentities without CSQC\n");
		return;
	}

	for (;;) {
		int payload_start = 0;
		int payload_len = -1;

		entnum = (unsigned short)MSG_ReadShort();
		removeflag = !!(entnum & 0x8000);
		entnum &= ~0x8000u;
		if ((!entnum && !removeflag) || msg_badread) {
			break;
		}
		if (entnum >= CSQC_MAX_NUM) {
			// Unreachable: entnum is a 15-bit wire value and CSQC_MAX_NUM covers the
			// full domain. Defensive guard; the stream must stay aligned instead of
			// being cut mid-message.
			if (sized) {
				int len = (unsigned short)MSG_ReadShort();

				if (len > 0) {
					MSG_ReadSkip(len);
				}
				continue;
			}
			msg_readcount = net_message.cursize;
			return;
		}

		if (removeflag) {
			int slot;

			// Removing world is a fatal protocol error.
			if (!entnum) {
				Host_Error("CSQC_Client_ParseEntities: cannot remove world\n");
			}

			slot = CSQC_Client_NumToSlot((int)entnum);
			if (slot) {
				qbool callback = ready && s_csqc.func_entremove > 0;

				CSQC_Client_NetRemove(vm, slot, (int)entnum, callback);
				if (callback && s_csqc.errored) {
					// Do not run the rest of this message against a disabled
					// module; the remaining records are engine-only from here.
					ready = false;
					if (!sized) {
						msg_readcount = net_message.cursize;
						return;
					}
				}
			}
			continue;
		}

		// Update. Sized: [len short][payload]. payload_start is after the length.
		if (sized) {
			// Unsigned length (as svc 90/91): a signed read would let a
			// server-controlled >=0x8000 length rewind the parser behind the payload
			// (out-of-bounds read + parser loop).
			payload_len = (unsigned short)MSG_ReadShort();
			payload_start = msg_readcount;
		}

		if (!ready || s_csqc.func_entupdate <= 0) {
			// No update module/callback: read out the payload so the stream is not
			// desynced. Non-sized does not know the length - fatal (see above).
			if (sized && payload_len > 0) {
				MSG_ReadSkip(payload_len);
			}
			else if (!sized) {
				Host_Error("CSQC_Client_ParseEntities: update without CSQC\n");
			}
			continue;
		}

		// isnew is true exactly when the number has no entity mapped. A mapped
		// slot the module freed with remove() keeps its mapping and gets isnew = 0.
		// It is saved across the spawn/alloc branch: CSQC_Ent_Spawn writes PARM0
		// (the server number).
		isnew = !CSQC_Client_NumToSlot((int)entnum);
		vm->globals[OFS_PARM0] = (float)isnew;

		// A new number gets a pool slot or CSQC_Ent_Spawn.
		{
			int slot = CSQC_Client_NumToSlot((int)entnum);
			if (!slot) {
				if (s_csqc.func_entspawn > 0) {
					// an invalid self (0/world) -> no slot
					slot = CSQC_Client_RunEntSpawn(vm, entnum);
					if (slot) {
						CSQC_Client_MapNumber((int)entnum, slot);
					}
				}
				else {
					slot = CSQC_Client_NetAllocSlot(vm);
					if (!slot) {
						Con_Printf("CSQC: pool full, entity %u dropped\n", entnum);
						// Keep the stream aligned: sized knows the payload length, so
						// skip this record and continue; unsized cannot, so drop the
						// remainder of the datagram (payload must never be parsed as
						// svc opcodes).
						if (sized && payload_len >= 0) {
							CSQC_Client_SizedRewind(payload_start, payload_len);
							continue;
						}
						msg_readcount = net_message.cursize;
						return;
					}
					CSQC_Client_MapNumber((int)entnum, slot);
					// number -> pool slot diagnostic, at most 32 lines while
					// csqc_dbg >= 3 stays set.
					{
						static int s_dbg_lines = 0;
						if (dbg && dbg->value >= 3) {
							if (s_dbg_lines < 32) {
								Con_Printf("CSQC ent num %u -> slot %d\n", entnum, slot);
								s_dbg_lines++;
							}
						}
						else {
							s_dbg_lines = 0;
						}
					}
				}
			}
			if (slot) {
				CSQC_Client_SetContextSlot(vm, (unsigned)slot, entnum);
			}
			else if (s_csqc.global_self >= 0) {
				*(int *)&vm->globals[s_csqc.global_self] = 0; // world
			}
			ctxslot = slot;
			ctxseq = (slot > 0 && slot < CSQC_MAX_EDICTS) ? s_allocseq[slot] : 0;
		}

		// Restore the isnew argument the spawn/alloc branch may have clobbered.
		vm->globals[OFS_PARM0] = (float)isnew;

		s_csqc.mayread = true;
		CSQC_Client_Exec(s_csqc.func_entupdate);
		s_csqc.mayread = false;

		if (s_csqc.errored) {
			// Module crashed mid-message. Sized: read out the current payload and
			// continue with the next record. Unsized has no length, so the rest of
			// the datagram cannot be resynced - drop it instead of disconnecting
			// (the module is already disabled and disablecsqc was sent).
			if (payload_len >= 0) {
				CSQC_Client_SizedRewind(payload_start, payload_len);
				ready = false;
				continue;
			}
			msg_readcount = net_message.cursize;
			return;
		}

		CSQC_Client_RemapAfterUpdate(vm, entnum, ctxslot, ctxseq);

		if (payload_len >= 0) {
			CSQC_Client_SizedRewind(payload_start, payload_len);
		}
	}
}

/*
=================
CSQC_Client_ParseEvent

Parsing of svc_fte_cgamepacket(83): the module itself reads the event name and
payload (CSQC_Parse_Event, or CSQC_Parse_TempEntity when that is the only one
declared) via read*-builtins from the current message. Without a module, a CSQC
multicast does not crash the client.
=================
*/
void CSQC_Client_ParseEvent(qbool sized)
{
	int evfn = (s_csqc.func_parseevent > 0) ? s_csqc.func_parseevent
											: ((s_csqc.func_parsetempent > 0) ? s_csqc.func_parsetempent : -1);
	qbool ready = CSQC_Client_ParseAllowed() && s_csqc.loaded && s_csqc.inited && !s_csqc.errored && evfn > 0;

	// The sized-stream caller (cl_parse.c case 90) drains by length itself - here
	// it is enough to return. Non-sized (case 83) has no
	// length byte, so the payload cannot be skipped: drop the rest of the datagram.
	// The module is resolved before `begin`, so this is only reached without a
	// usable module (no csprogs, download disabled/failed, module error). A
	// disconnect would make a multicast to clients that negotiated FTE_PEXT_CSQC
	// fatal; the packet cannot be decoded either way.
	if (!ready) {
		if (!sized) {
			Con_DPrintf("CSQC: cgamepacket dropped (module not ready)\n");
			// Only a module that cannot consume the stream at all justifies
			// disabling it: a loaded module without event callbacks still receives
			// entities, and disablecsqc turns off the whole stream server-side.
			if (!s_csqc.loaded || !s_csqc.inited || s_csqc.errored) {
				CSQC_Client_NotifyCSQC(false);
			}
			msg_readcount = net_message.cursize;
		}
		return;
	}
	s_csqc.mayread = true;
	CSQC_Client_Exec(evfn);
	s_csqc.mayread = false;
	// The module may have crashed mid-payload. Sized - caller drains by length.
	// Non-sized does not know the length, so drop the remainder of the datagram:
	// the module is disabled already, and a disconnect would only make one bad
	// callback fatal for the session.
	if (s_csqc.errored && !sized) {
		msg_readcount = net_message.cursize;
	}
}

/*
=================
CSQC_Client_InputFrame

CSQC_Input_Frame: called before each usercmd is sent (CL_SendCmd). The engine fills
the module-declared input_* globals (input_sequence/timelength/angles/movevalues/
buttons/impulse) from cmd, runs CSQC_Input_Frame, then writes them back into cmd.

- usercmd.angles is float degrees (MSG_WriteDeltaUsercmd quantizes), so angles are
  copied directly;
- input_timelength is scaled by gamespeed (1, or 0 on server pause);
- the lightlevel/weapon/servertime/clienttime/cursor/VR input globals are not
  supported.
=================
*/
void CSQC_Client_InputFrame(usercmd_t *cmd)
{
	pr1vm_t *vm = &s_csqc.vm;

	// Live clientcommandframe = seq of the cmd being built (see s_ccframe). Set
	// before the guard, so it is tracked even without a module.
	s_ccframe = (unsigned int)cls.netchan.outgoing_sequence;

	if (!s_csqc.loaded || !s_csqc.inited || s_csqc.errored || s_csqc.func_input <= 0) {
		return;
	}

	CSQC_Client_PatchFrames();

	CSQC_Client_SetTime();

	// input_sequence = seq of the current cmd; same numbering as
	// clientcommandframe.
	if (s_csqc.in_sequence >= 0) {
		vm->globals[s_csqc.in_sequence] = CSQC_Client_ClientCmdFrame();
	}

	// input_timelength = msec/1000 * gamespeed (1, or 0 on server pause).
	if (s_csqc.in_timelength >= 0) {
		vm->globals[s_csqc.in_timelength] = cmd->msec / 1000.0f * ((cl.paused & PAUSED_SERVER) ? 0.0f : 1.0f);
	}
	if (s_csqc.in_angles >= 0) {
		vm->globals[s_csqc.in_angles + 0] = cmd->angles[0];
		vm->globals[s_csqc.in_angles + 1] = cmd->angles[1];
		vm->globals[s_csqc.in_angles + 2] = cmd->angles[2];
	}
	if (s_csqc.in_movevalues >= 0) {
		vm->globals[s_csqc.in_movevalues + 0] = cmd->forwardmove;
		vm->globals[s_csqc.in_movevalues + 1] = cmd->sidemove;
		vm->globals[s_csqc.in_movevalues + 2] = cmd->upmove;
	}
	if (s_csqc.in_buttons >= 0) {
		vm->globals[s_csqc.in_buttons] = cmd->buttons;
	}
	if (s_csqc.in_impulse >= 0) {
		vm->globals[s_csqc.in_impulse] = cmd->impulse;
	}

	if (!CSQC_Client_Exec(s_csqc.func_input)) {
		return;  // errored - frames disabled, cmd untouched
	}

	// input_* globals -> cmd (every declared field is written back).
	if (s_csqc.in_timelength >= 0) {
		cmd->msec = (byte)boundf(1, vm->globals[s_csqc.in_timelength] * 1000.0f, 255);
	}
	if (s_csqc.in_angles >= 0) {
		cmd->angles[0] = vm->globals[s_csqc.in_angles + 0];
		cmd->angles[1] = vm->globals[s_csqc.in_angles + 1];
		cmd->angles[2] = vm->globals[s_csqc.in_angles + 2];
	}
	if (s_csqc.in_movevalues >= 0) {
		cmd->forwardmove = (short)boundf(-32768, vm->globals[s_csqc.in_movevalues + 0], 32767);
		cmd->sidemove = (short)boundf(-32768, vm->globals[s_csqc.in_movevalues + 1], 32767);
		cmd->upmove = (short)boundf(-32768, vm->globals[s_csqc.in_movevalues + 2], 32767);
	}
	if (s_csqc.in_buttons >= 0) {
		cmd->buttons = (byte)boundf(0, vm->globals[s_csqc.in_buttons], 255);
	}
	if (s_csqc.in_impulse >= 0) {
		cmd->impulse = (byte)boundf(0, vm->globals[s_csqc.in_impulse], 255);
	}
}

/*
=================
CSQC_Client_RecordInput / CSQC_Client_ApplyInput

#345: history of sent usercmds. CL_SendCmd records each sent cmd
(CSQC_Client_RecordInput); builtin #345(seq) asks for it and fills the input_*
globals (CSQC_Client_ApplyInput).

seq = mirror of cls.netchan.outgoing_sequence (the client message number at write
time; Netchan_Transmit increments after writing the header). This is the number the
server acknowledges (servercommandframe = incoming_acknowledged = cl.parsecount) -
the window (servercommandframe, clientcommandframe] is consistent in one numbering.
The history is a ring of CSQC_INHIST and records only the live CL_SendCmd path
(demos/MVD are not recorded). clientcommandframe = s_ccframe = the last recorded
seq, so #345(clientcommandframe) hits the ring, also outside CSQC_Input_Frame.
=================
*/
void CSQC_Client_RecordInput(usercmd_t *cmd)
{
	unsigned int seq;

	seq = (unsigned int)cls.netchan.outgoing_sequence;
	s_last_seq = seq;
	s_inhist[seq % CSQC_INHIST].seq = seq;
	s_inhist[seq % CSQC_INHIST].cmd = *cmd;
	if (s_csqc.loaded && !s_csqc.errored) {
		if (s_csqc.in_sequence >= 0) {
			s_csqc.vm.globals[s_csqc.in_sequence] = seq;
		}
		if (s_csqc.g_ccframe >= 0) {
			s_csqc.vm.globals[s_csqc.g_ccframe] = seq;
		}
	}
}

static void CSQC_Client_FillInputFromCmd(usercmd_t *cmd)
{
	pr1vm_t *vm = &s_csqc.vm;

	// x gamespeed as in CSQC_Client_InputFrame.
	if (s_csqc.in_timelength >= 0) {
		vm->globals[s_csqc.in_timelength] = cmd->msec / 1000.0f * ((cl.paused & PAUSED_SERVER) ? 0.0f : 1.0f);
	}
	if (s_csqc.in_angles >= 0) {
		vm->globals[s_csqc.in_angles + 0] = cmd->angles[0];
		vm->globals[s_csqc.in_angles + 1] = cmd->angles[1];
		vm->globals[s_csqc.in_angles + 2] = cmd->angles[2];
	}
	if (s_csqc.in_movevalues >= 0) {
		vm->globals[s_csqc.in_movevalues + 0] = cmd->forwardmove;
		vm->globals[s_csqc.in_movevalues + 1] = cmd->sidemove;
		vm->globals[s_csqc.in_movevalues + 2] = cmd->upmove;
	}
	if (s_csqc.in_buttons >= 0) {
		vm->globals[s_csqc.in_buttons] = cmd->buttons;
	}
	if (s_csqc.in_impulse >= 0) {
		vm->globals[s_csqc.in_impulse] = cmd->impulse;
	}
}

int CSQC_Client_ApplyInput(unsigned int seq)
{
	unsigned int i;
	csqc_inrec_t *r;

	if (!s_csqc.loaded || s_csqc.errored) {
		return 0;
	}
	if (!seq) {
		return 0;
	}
	// On server pause the window frames are not applied. The range
	// (servercommandframe, clientcommandframe] is the module's contract (see the
	// EXT_CSQC_1 spec) and not checked here, only ring liveness.
	if ((cl.paused & PAUSED_SERVER) && seq >= (unsigned)CSQC_Client_ServerCmdFrame()) {
		return 0;
	}
	for (i = 0; i < CSQC_INHIST; i++) {
		r = &s_inhist[i];
		if (r->seq == seq) {
			CSQC_Client_FillInputFromCmd(&r->cmd);
			if (s_csqc.in_sequence >= 0) {
				s_csqc.vm.globals[s_csqc.in_sequence] = seq;
			}
			return 1;
		}
	}
	return 0;
}

/*
=================
CSQC_VectorAngles

Port of FTE VectorAngles with optional up->roll. When `meshpitch` is set the
pitch is inverted (r_meshpitch = -1, FTE VectorAngles(..., true)) - parity for
#51 vectoangles. forward is the direction; up may be NULL;
result[3] = (pitch, yaw, roll). Helper for #51 (csqc_builtins.c).
=================
*/
void CSQC_VectorAngles(const float *forward, const float *up, float *result, qbool meshpitch)
{
	float yaw, pitch, roll;

	if (forward[1] == 0 && forward[0] == 0) {
		if (forward[2] > 0) {
			pitch = -M_PI * 0.5f;
			yaw = up ? (float)atan2(-up[1], -up[0]) : 0;
		}
		else {
			pitch = M_PI * 0.5f;
			yaw = up ? (float)atan2(up[1], up[0]) : 0;
		}
		roll = 0;
	}
	else {
		float cp, sp, cy, sy;
		yaw = (float)atan2(forward[1], forward[0]);
		pitch = -(float)atan2(forward[2], sqrt(forward[0] * forward[0] + forward[1] * forward[1]));
		if (up) {
			float tleft[3], tup[3];
			cp = (float)cos(pitch);
			sp = (float)sin(pitch);
			cy = (float)cos(yaw);
			sy = (float)sin(yaw);
			tleft[0] = -sy;
			tleft[1] = cy;
			tleft[2] = 0;
			tup[0] = sp * cy;
			tup[1] = sp * sy;
			tup[2] = cp;
			roll = -(float)atan2(up[0] * tleft[0] + up[1] * tleft[1] + up[2] * tleft[2],
				up[0] * tup[0] + up[1] * tup[1] + up[2] * tup[2]);
		}
		else {
			roll = 0;
		}
	}
	pitch *= (float)(180 / M_PI);
	yaw *= (float)(180 / M_PI);
	roll *= (float)(180 / M_PI);
	if (meshpitch) {
		pitch = -pitch;  // roll unchanged
	}
	if (pitch < 0) {
		pitch += 360;
	}
	if (yaw < 0) {
		yaw += 360;
	}
	if (roll < 0) {
		roll += 360;
	}
	result[0] = pitch;
	result[1] = yaw;
	result[2] = roll;
}

/*
=================
CSQC_Client_MakeVectors

#1 makevectors: from an angle vector write the module's v_forward/v_right/v_up
globals. No-op if the module did not declare them.
=================
*/
void CSQC_Client_MakeVectors(float *ang)
{
	pr1vm_t *vm = &s_csqc.vm;
	float *f, *r, *u;

	if (!s_csqc.loaded || !s_csqc.inited || s_csqc.errored) {
		return;
	}
	if (s_csqc.g_vfwd < 0 || s_csqc.g_vright < 0 || s_csqc.g_vup < 0) {
		return;
	}
	f = &vm->globals[s_csqc.g_vfwd];
	r = &vm->globals[s_csqc.g_vright];
	u = &vm->globals[s_csqc.g_vup];
	AngleVectors(ang, f, r, u);
}

/*
=================
CSQC_Client_VectorVectors

#432 vectorvectors: normalize the given direction into the module's v_forward and
build v_right = (f.y, -f.x, 0) normalized (0,-1,0 for a vertical f) and
v_up = right x forward. PerpendicularVector is not used: its (0,0,z) edge case
differs. No-op if the module did not declare the globals.
=================
*/
void CSQC_Client_VectorVectors(float *dir)
{
	pr1vm_t *vm = &s_csqc.vm;
	float *f, *r, *u;
	float right[3];

	if (!s_csqc.loaded || !s_csqc.inited || s_csqc.errored) {
		return;
	}
	if (s_csqc.g_vfwd < 0 || s_csqc.g_vright < 0 || s_csqc.g_vup < 0) {
		return;
	}
	f = &vm->globals[s_csqc.g_vfwd];
	r = &vm->globals[s_csqc.g_vright];
	u = &vm->globals[s_csqc.g_vup];

	VectorCopy(dir, f);
	VectorNormalize(f);

	if (!f[0] && !f[1]) {
		right[0] = 0;
		right[1] = f[2] ? -1 : 0;
		right[2] = 0;
	}
	else {
		right[0] = f[1];
		right[1] = -f[0];
		right[2] = 0;
		VectorNormalize(right);
	}
	VectorCopy(right, r);
	CrossProduct(right, f, u);
}

/*
=================
CSQC_Client_HasInputEvent / CSQC_Client_InputEvent

Delivery of input events to the module (CSQC_InputEvent). Called from keys.c
(keys/clicks/wheel: down in game, up also outside the game for keys whose down was
delivered - reserved keys never reach here) and in_sdl2.c (mouse: MOUSEDELTA in the
ordinary mode; MOUSEABS - from CSQC_Client_Frame with a module cursor). A module return
!= 0 means "event handled" (the engine does not apply it).
=================
*/
qbool CSQC_Client_HasInputEvent(void)
{
	return s_csqc.loaded && s_csqc.inited && !s_csqc.errored && s_csqc.func_inputevent > 0;
}

int CSQC_Client_InputEvent(int evtype, float a, float b, float c)
{
	pr1vm_t *vm = &s_csqc.vm;

	if (!CSQC_Client_HasInputEvent()) {
		return 0;
	}
	// The module works in the QC/DP key domain; translate the internal ezq keynum ->
	// QC for key events (mouse/deltas - no translation). Key events are tracked so a
	// key-up delivered outside the game still reaches the module (no stuck keys),
	// while an up without a delivered down (or a reserved key) is dropped.
	if (evtype == IE_KEYDOWN || evtype == IE_KEYUP) {
		int key = (int)a;

		if (key < 0 || key >= UNKNOWN) {
			key = 0; // out-of-range keynum
		}
		if (evtype == IE_KEYDOWN) {
			s_csqc.csqckeysdown[key] = 1;
		}
		else {
			if (!s_csqc.csqckeysdown[key]) {
				return 0; // no delivered down - do not leak the up to the module
			}
			s_csqc.csqckeysdown[key] = 0;
		}
		a = CSQC_Client_KeynumToQC(key);
	}
	vm->globals[OFS_PARM0] = evtype;
	vm->globals[OFS_PARM1] = a;
	vm->globals[OFS_PARM2] = b;
	vm->globals[OFS_PARM3] = c;
	if (!CSQC_Client_Exec(s_csqc.func_inputevent)) {
		return 0;
	}
	return (int)vm->globals[OFS_RETURN];
}

/*
=================
CSQC_Client_RunPlayerPhysics

#347 runstandardplayerphysics(ent) on the client PM path (PM_PlayerMove, as in
cl_pred.c):

- the movement command comes from the input_* globals (the module calls
  getinputstate(seq) before #347); fallback - the last recorded usercmd (if input_*
  are not declared);
- the solid set is rebuilt inside the call (CL_SetSolidEntities + Players); entity
  fields .mins/.maxs/.gravity/.pmove_flags/.flags; writes .origin/.velocity/.angles,
  .flags (FL_ONGROUND), .pmove_flags (PMF_JUMP_HELD);
- chunks <=50 ms (as cl_pred.c) + deprecated pmove_org/vel/onground.

Limits of the QW PM:
- no skipent: the entity named by .entnum is left out of the solid set instead.
  Predicted player boxes are part of the set (QW players are not packet entities,
  so without them nothing would block);
- .gravitydir is not supported (gravity is always -z);
- no PMF_LADDER (QW has no ladders);
- .waterlevel is computed internally but not written back.
The entity box maps onto the global player_mins/maxs (other players' box is the same).
=================
*/
#define CSQC_MV_WALK	3	// MOVETYPE_*
#define CSQC_MV_FLY		5
#define CSQC_MV_NOCLIP	8
#define CSQC_PMF_JUMP_HELD	1
#define CSQC_FL_ONGROUND	512
#define CSQC_PHYSICS_MAX_MSECS	1000	// defensive cap for #347 (module input_timelength)

static playermove_t s_physics_saved_pmove; // too large for the stack

void CSQC_Client_RunPlayerPhysics(int entnum)
{
	extern vec3_t player_mins, player_maxs;
	pr1vm_t *vm = &s_csqc.vm;
	float *base, *o, *v;
	vec3_t saved_mins, saved_maxs;
	playermove_t *saved_pmove;
	movevars_t saved_movevars;
	int msecs, mt, i, skipnum;

	if (!s_csqc.loaded || !s_csqc.inited || s_csqc.errored) {
		return;
	}
	if (entnum <= 0 || entnum >= CSQC_MAX_EDICTS || cls.state != ca_active) {
		return;  // slot 0 = world (read-only)
	}

	base = (float *)((byte *)vm->game_edicts + (size_t)entnum * vm->edict_size);
	skipnum = CSQC_Client_EntityEntNum(vm, entnum);

	// pmove/movevars carry the frame's prediction state (CL_PredictMove appends to
	// pmove.numphysent from the last parse); run on scratch and put them back.
	saved_pmove = &s_physics_saved_pmove;
	*saved_pmove = pmove;
	saved_movevars = movevars;
	memset(&pmove, 0, sizeof(pmove));

	if (s_csqc.in_timelength >= 0 || s_csqc.in_angles >= 0 || s_csqc.in_movevalues >= 0) {
		// msecs = input_timelength*1000; a zero/negative duration means no
		// physics step.
		if (s_csqc.in_timelength >= 0) {
			// Clamp so the float->int cast is well-defined (huge/NaN is UB) and
			// the substep loop cannot spin for an unbounded time; a real frame is
			// << 1 s.
			msecs = (int)boundf(0, vm->globals[s_csqc.in_timelength] * 1000.0f, CSQC_PHYSICS_MAX_MSECS);
		}
		else {
			msecs = 1;
		}
		if (s_csqc.in_angles >= 0) {
			VectorCopy(&vm->globals[s_csqc.in_angles], pmove.cmd.angles);
			VectorCopy(pmove.cmd.angles, pmove.angles);
		}
		if (s_csqc.in_movevalues >= 0) {
			pmove.cmd.forwardmove = (short)boundf(-32768, vm->globals[s_csqc.in_movevalues + 0], 32767);
			pmove.cmd.sidemove = (short)boundf(-32768, vm->globals[s_csqc.in_movevalues + 1], 32767);
			pmove.cmd.upmove = (short)boundf(-32768, vm->globals[s_csqc.in_movevalues + 2], 32767);
		}
		if (s_csqc.in_buttons >= 0) {
			pmove.cmd.buttons = (byte)boundf(0, vm->globals[s_csqc.in_buttons], 255);
		}
		if (s_csqc.in_impulse >= 0) {
			pmove.cmd.impulse = (byte)boundf(0, vm->globals[s_csqc.in_impulse], 255);
		}
	}
	else {
		if (!s_last_seq) {
			goto restore;
		}
		pmove.cmd = s_inhist[s_last_seq % CSQC_INHIST].cmd;
		VectorCopy(pmove.cmd.angles, pmove.angles);
		msecs = pmove.cmd.msec;
	}

	if (s_csqc.f_origin >= 0) {
		VectorCopy(base + s_csqc.f_origin, pmove.origin);
	}
	else {
		VectorClear(pmove.origin);
	}
	if (s_csqc.f_velocity >= 0) {
		VectorCopy(base + s_csqc.f_velocity, pmove.velocity);
	}
	else {
		VectorClear(pmove.velocity);
	}
	if (s_csqc.f_flags >= 0) {
		pmove.onground = (((int)base[s_csqc.f_flags]) & CSQC_FL_ONGROUND) != 0;
	}

	// box ent -> global player_mins/maxs (ezq PM/SetSolidPlayers use the global
	// box); save and restore after the call.
	VectorCopy(player_mins, saved_mins);
	VectorCopy(player_maxs, saved_maxs);
	if (s_csqc.f_mins >= 0 && s_csqc.f_maxs >= 0) {
		float *mn = base + s_csqc.f_mins, *mx = base + s_csqc.f_maxs;
		if (mn[0] || mn[1] || mn[2] || mx[0] || mx[1] || mx[2]) {
			VectorCopy(mn, player_mins);
			VectorCopy(mx, player_maxs);
		}
	}

	mt = (s_csqc.f_movetype >= 0) ? (int)base[s_csqc.f_movetype] : CSQC_MV_WALK;
	switch (mt) {
		case CSQC_MV_FLY:
			pmove.pm_type = PM_FLY;
			break;
		case CSQC_MV_NOCLIP:
			pmove.pm_type = PM_SPECTATOR;
			break;
		default:
			pmove.pm_type = PM_NORMAL;
			break;
	}
	pmove.jump_held = (s_csqc.f_pmove_flags >= 0) ? (((int)base[s_csqc.f_pmove_flags] & CSQC_PMF_JUMP_HELD) != 0) : false;
	pmove.jump_msec = 0;
	pmove.waterjumptime = 0;

	// The server-sent entgravity belongs to the viewed player only.
	movevars.entgravity = (skipnum == cl.viewplayernum + 1) ? cl.entgravity : 1;
	movevars.maxspeed = cl.maxspeed;
	movevars.bunnyspeedcap = cl.bunnyspeedcap;
	if (s_csqc.f_gravity >= 0 && base[s_csqc.f_gravity] != 0) {
		movevars.entgravity = base[s_csqc.f_gravity];
	}

	// Solid set: world + packet entities + predicted player boxes, minus the
	// simulated entity itself (by .entnum). A pure CSQC entity skips nothing, so it
	// collides with the local player too.
	CL_SetSolidEntities();
	if (skipnum > 0) {
		int n = 1;
		for (i = 1; i < pmove.numphysent; i++) {
			if (pmove.physents[i].info != skipnum) {
				pmove.physents[n++] = pmove.physents[i];
			}
		}
		pmove.numphysent = n;
	}
	CL_SetSolidPlayers((skipnum >= 1 && skipnum <= MAX_CLIENTS) ? skipnum - 1 : -1);

	while (msecs > 0) {
		int step = (msecs > 50) ? 50 : msecs;
		pmove.cmd.msec = step;
		PM_PlayerMove();
		msecs -= step;
	}

	o = (s_csqc.f_origin >= 0) ? base + s_csqc.f_origin : NULL;
	v = (s_csqc.f_velocity >= 0) ? base + s_csqc.f_velocity : NULL;
	if (o) {
		VectorCopy(pmove.origin, o);
	}
	if (v) {
		VectorCopy(pmove.velocity, v);
	}
	if (s_csqc.f_angles >= 0) {
		VectorCopy(pmove.angles, base + s_csqc.f_angles);
		// model-space pitch: -pitch/3 (QW player models)
		base[s_csqc.f_angles] = -base[s_csqc.f_angles] / 3;
	}
	if (s_csqc.f_flags >= 0) {
		float *fl = base + s_csqc.f_flags;
		*fl = (float)(pmove.onground
				? (((int)*fl) | CSQC_FL_ONGROUND)
				: (((int)*fl) & ~CSQC_FL_ONGROUND));
	}
	if (s_csqc.f_pmove_flags >= 0) {
		base[s_csqc.f_pmove_flags] = (float)(pmove.jump_held ? CSQC_PMF_JUMP_HELD : 0);
	}

	// deprecated globals
	if (s_csqc.p_org >= 0) {
		for (i = 0; i < 3; i++) {
			vm->globals[s_csqc.p_org + i] = pmove.origin[i];
		}
	}
	if (s_csqc.p_vel >= 0) {
		for (i = 0; i < 3; i++) {
			vm->globals[s_csqc.p_vel + i] = pmove.velocity[i];
		}
	}
	if (s_csqc.p_onground >= 0) {
		vm->globals[s_csqc.p_onground] = pmove.onground;
	}

	VectorCopy(saved_mins, player_mins);
	VectorCopy(saved_maxs, player_maxs);
	CSQC_Client_LinkBounds(vm, entnum);

restore:
	pmove = *saved_pmove;
	movevars = saved_movevars;
}

static csqc_buf_t *CSQC_Client_BufAt(int handle)
{
	if (handle < 1 || handle > s_numbufs || !s_bufs[handle - 1].inuse) {
		return NULL;
	}
	return &s_bufs[handle - 1];
}

static void CSQC_Client_BufClear(csqc_buf_t *b)
{
	int i;
	for (i = 0; i < b->num; i++) {
		if (b->str[i]) {
			Q_free(b->str[i]);
		}
	}
	Q_free(b->str);
	b->str = NULL;
	b->num = b->cap = 0;
}

int CSQC_Client_BufCreate(void)
{
	int i;

	for (i = 0; i < s_numbufs; i++) {
		if (!s_bufs[i].inuse) {
			break;
		}
	}
	if (i == s_numbufs) {
		csqc_buf_t *nb = (csqc_buf_t *)Q_realloc(s_bufs, sizeof(*s_bufs) * (s_numbufs + 1));
		if (!nb) {
			return -1;
		}
		s_bufs = nb;
		s_numbufs++;
	}
	memset(&s_bufs[i], 0, sizeof(s_bufs[i]));
	s_bufs[i].inuse = true;
	return i + 1;
}

void CSQC_Client_BufDel(int handle)
{
	csqc_buf_t *b = CSQC_Client_BufAt(handle);
	if (b) {
		CSQC_Client_BufClear(b);
		b->inuse = false;
	}
}

int CSQC_Client_BufGetSize(int handle)
{
	csqc_buf_t *b = CSQC_Client_BufAt(handle);
	return b ? b->num : 0;
}

#define CSQC_MAX_BUF_INDEX	(1024 * 1024)	// sanity limit

static qbool CSQC_Client_BufEnsure(csqc_buf_t *b, int idx)
{
	char **ns;
	int ncap;

	if (idx < b->cap) {
		return true;
	}
	if (idx > CSQC_MAX_BUF_INDEX) {
		Con_DPrintf("CSQC: string buffer index %d outside sanity range\n", idx);
		return false;
	}
	ncap = idx + 256;
	ns = (char **)Q_realloc(b->str, sizeof(char *) * ncap);
	if (!ns) {
		return false;
	}
	memset(ns + b->cap, 0, sizeof(char *) * (ncap - b->cap));
	b->str = ns;
	b->cap = ncap;
	return true;
}

static qbool CSQC_Client_BufStore(csqc_buf_t *b, int idx, const char *s)
{
	char *c;

	if (idx < 0 || !CSQC_Client_BufEnsure(b, idx)) {
		return false;
	}
	c = Q_strdup(s ? s : "");
	if (!c) {
		return false;
	}
	Q_free(b->str[idx]);
	b->str[idx] = c;
	if (idx >= b->num) {
		b->num = idx + 1;
	}
	return true;
}

// order != 0: append at num; order == 0: first hole below num, else append.
int CSQC_Client_BufAdd(int handle, const char *s, int order)
{
	csqc_buf_t *b = CSQC_Client_BufAt(handle);
	int idx;

	if (!b) {
		return -1;
	}
	idx = b->num;
	if (!order) {
		for (idx = 0; idx < b->num; idx++) {
			if (!b->str[idx]) {
				break;
			}
		}
	}
	return CSQC_Client_BufStore(b, idx, s) ? idx : -1;
}

const char *CSQC_Client_BufGet(int handle, int idx)
{
	csqc_buf_t *b = CSQC_Client_BufAt(handle);

	if (!b || idx < 0 || idx >= b->num) {
		return NULL;
	}
	return b->str[idx];
}

int CSQC_Client_BufSet(int handle, int idx, const char *s)
{
	csqc_buf_t *b = CSQC_Client_BufAt(handle);

	return b ? CSQC_Client_BufStore(b, idx, s) : 0;
}

// Leaves a hole; later indices and num stay put.
int CSQC_Client_BufFree(int handle, int idx)
{
	csqc_buf_t *b = CSQC_Client_BufAt(handle);

	if (!b || idx < 0 || idx >= b->num) {
		return 0;
	}
	Q_free(b->str[idx]);
	b->str[idx] = NULL;
	return 1;
}

int CSQC_Client_BufCopy(int from, int to)
{
	csqc_buf_t *f = CSQC_Client_BufAt(from);
	csqc_buf_t *t = CSQC_Client_BufAt(to);
	int i;

	if (!f || !t) {
		return 0;
	}
	if (f == t) {
		return 1;
	}
	CSQC_Client_BufClear(t);
	if (!f->num) {
		return 1;
	}
	t->str = (char **)Q_calloc(f->num, sizeof(char *));
	if (!t->str) {
		return 0;
	}
	t->cap = t->num = f->num;
	for (i = 0; i < f->num; i++) {
		t->str[i] = f->str[i] ? Q_strdup(f->str[i]) : NULL;
	}
	return 1;
}

// qsort has no context argument; the client buffer sort is single-threaded and
// non-reentrant, so the current call's parameters live here.
static int s_bufsort_prefixlen;
static int s_bufsort_backward;

static int CSQC_Client_BufSortCmp(const void *pa, const void *pb)
{
	const char *a = *(const char *const *)pa;
	const char *b = *(const char *const *)pb;
	int cmp;

	cmp = (s_bufsort_prefixlen > 0) ? strncmp(a, b, (size_t)s_bufsort_prefixlen) : strcmp(a, b);
	return s_bufsort_backward ? -cmp : cmp;
}

int CSQC_Client_BufSort(int handle, int prefixlen, int backward)
{
	csqc_buf_t *b = CSQC_Client_BufAt(handle);
	int s, d;

	if (!b) {
		return 0;
	}
	// Holes are dropped, shrinking num.
	for (s = d = 0; s < b->num; s++) {
		if (b->str[s]) {
			b->str[d++] = b->str[s];
		}
	}
	for (s = d; s < b->num; s++) {
		b->str[s] = NULL;
	}
	b->num = d;
	if (b->num > 1) {
		s_bufsort_prefixlen = prefixlen;
		s_bufsort_backward = backward;
		qsort(b->str, (size_t)b->num, sizeof(b->str[0]), CSQC_Client_BufSortCmp);
	}
	return 1;
}

// Heap result (caller frees), NULL for a bad handle. Holes are skipped and glue
// only separates non-empty output.
char *CSQC_Client_BufImplode(int handle, const char *glue)
{
	csqc_buf_t *b = CSQC_Client_BufAt(handle);
	size_t gl, len = 0, o = 0;
	char *out;
	int i;

	if (!b) {
		return NULL;
	}
	if (!glue) {
		glue = "";
	}
	gl = strlen(glue);
	for (i = 0; i < b->num; i++) {
		if (b->str[i]) {
			len += (len ? gl : 0) + strlen(b->str[i]);
		}
	}

	out = (char *)Q_malloc(len + 1);
	if (!out) {
		return NULL;
	}
	for (i = 0; i < b->num; i++) {
		size_t l;

		if (!b->str[i]) {
			continue;
		}
		if (o) {
			memcpy(out + o, glue, gl);
			o += gl;
		}
		l = strlen(b->str[i]);
		memcpy(out + o, b->str[i], l);
		o += l;
	}
	out[o] = 0;
	return out;
}

void CSQC_Client_BufReset(void)
{
	int i;

	for (i = 0; i < s_numbufs; i++) {
		if (s_bufs[i].inuse) {
			CSQC_Client_BufClear(&s_bufs[i]);
		}
	}
	Q_free(s_bufs);
	s_bufs = NULL;
	s_numbufs = 0;
}

void CSQC_Client_Disconnect(void)
{
	int i;

	// #69 module statics hold model pointers that die with this map/module
	// (CL_ClearState -> Host_ClearMemory frees the models right after this
	// returns), so they are cleared even on the deferred-teardown path below.
	CSQCVM_ClearStatics();
	CSQCVM_ClearParticleEffects();

	// Leaving the map/server: a held `begin` belongs to the old connection.
	s_prebegin.done = s_prebegin.held = false;

	// The client VM can be executing when this runs: a builtin may raise an engine
	// Host_Error (e.g. precache_model on a found non-model file -> Mod_ForName ->
	// Host_Error -> CL_Disconnect -> here). Running func_shutdown / PR1VM_UnLoad now
	// would re-enter the VM; a nested module error longjmps to the *outer*
	// PR1VM_ExecuteProgram frame, abandoning Host_Error before it clears `inerror` -
	// the next Host_Error then hits Sys_Error("recursively entered"). Detach the
	// classic mirrors (so this VM is no longer PR1VM_Active()) and defer the actual
	// teardown to the next disconnect, when the VM is not executing.
	if (PR1VM_Active() == &s_csqc.vm) {
		PR1VM_RestoreContext(&s_csqc.vm);
		s_csqc.vm.abortbuf = NULL; // drop the abandoned frame's unwind target
		s_csqc.errored = true;  // module defunct: never executed again before unload
		return;
	}

	if (s_csqc.loaded) {
		if (s_csqc.inited && !s_csqc.errored) {
			s_in_shutdown = true;
			CSQC_Client_Exec(s_csqc.func_shutdown);
			s_in_shutdown = false;
		}
		PR1VM_UnLoad(&s_csqc.vm);
	}
	// The VM no longer references the csprogs buffer after UnLoad (this also
	// covers a module that failed after PR1VM_LoadClientV6, e.g. in CSQC_Init).
	Q_free(s_csqc.module_data);
	s_csqc.module_data = NULL;
	Q_free(s_csqc.globals_owned);
	s_csqc.globals_owned = NULL;
	CSQC_Client_ClearCommands();
	// before the memset, while the pointers are still in place
	CSQC_Client_FreeArena();
	s_cursormode.usecursor = false;
	s_cursormode.cursorimage[0] = 0;
	s_cursormode.scale = 0;
	s_mousemove_x = s_mousemove_y = 0;
	s_sens_scale = 1;
	CSQC_Client_BufReset();
	CSQC_Client_DeltaReset();
	CSQC_Client_ViewReset();
	s_scene_rendered = false;
	CSQC_Client_ModelReset();
	// #630: module binds must not outlive the session; #93/#72: module cvars are
	// removed.
	for (i = 0; i < UNKNOWN + 256; i++) {
		if (Key_IsModuleBind(i)) {
			Key_Unbind(i);
		}
	}
	CSQC_Client_ClearModuleCvars();
	s_wire_model_count = 0;  // distinct-name caps are per session
	s_module_sound_count = 0;
	s_dl_queue_head = s_dl_queue_count = 0;
	// Engine-transfer bookkeeping is connection-scoped: a stale pending csprogs
	// request would otherwise fire its no-progress timeout on the next server.
	memset(&s_dl, 0, sizeof(s_dl));
	CSQC_Client_FreeStringPool(); // before the memset
	CSQC_Client_DrawFontScaleReset();
	memset(&s_csqc, 0, sizeof(s_csqc));
	s_csqc.func_init = s_csqc.func_world = s_csqc.func_update = s_csqc.func_console = s_csqc.func_shutdown = -1;
	s_csqc.func_entupdate = s_csqc.func_entremove = s_csqc.func_parseevent = s_csqc.func_parsetempent = -1;
	s_csqc.func_entspawn = -1;
	s_csqc.mayread = false;
	s_csqc.func_input = -1;
	s_csqc.func_inputevent = -1;
	s_csqc.func_startframe = s_csqc.func_endframe = -1;
	s_csqc.global_time = -1;
	s_csqc.global_gamespeed = -1;
	s_csqc.global_self = -1;
	s_csqc.global_other = -1;
	s_csqc.global_physics_mode = -1;
	s_csqc.field_entnum = -1;
	s_csqc.f_origin = s_csqc.f_velocity = s_csqc.f_angles = s_csqc.f_mins = s_csqc.f_maxs = -1;
	s_csqc.f_movetype = s_csqc.f_flags = s_csqc.f_gravity = s_csqc.f_pmove_flags = -1;
	s_csqc.f_modelindex = s_csqc.f_skin = -1;
	s_csqc.f_frame = s_csqc.f_effects = s_csqc.f_colormap = s_csqc.f_drawmask = -1;
	s_csqc.f_think = s_csqc.f_nextthink = -1;
	s_csqc.g_localentnum = -1;
	s_csqc.in_timelength = s_csqc.in_angles = s_csqc.in_movevalues = -1;
	s_csqc.in_buttons = s_csqc.in_impulse = -1;
	s_csqc.in_sequence = -1;
	s_csqc.g_ccframe = s_csqc.g_scframe = -1;
	s_csqc.p_org = s_csqc.p_vel = s_csqc.p_onground = -1;
	s_csqc.g_vfwd = s_csqc.g_vright = s_csqc.g_vup = -1;
	s_csqc.g_view_angles = -1;
	s_csqc.g_frametime = s_csqc.g_cltime = s_csqc.g_maxclients = -1;
	s_csqc.g_player_localnum = s_csqc.g_intermission = -1;
	CSQC_Client_OffsetCacheReset();
	// outgoing_sequence restarts on reconnect; old records would alias new seqs
	memset(s_inhist, 0, sizeof(s_inhist));
	s_last_seq = 0;
	s_ccframe = 0;
	s_console_executing = false; // a longjmp'd console call must not wedge the guard
}
