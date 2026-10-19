/*
 csqc_builtins.c -- client PR1VM builtins (our csprogs.dat).

 Builtins for the client instance: numbers follow FTE's CSQC definitions (= #N),
 arguments are read from vm->globals[OFS_PARM0..], return value in OFS_RETURN,
 strings via CSQC_Client_GetString/PR1VM_ClientSetString on the active instance.

 Implemented here: the client builtin surface exposed to csprogs.dat -- strings/math,
 2D drawing, arena entities, traces, input, cvars, sound and registered no-op stubs.
*/

#include "qwsvdef.h"
#include "quakedef.h"
#include <time.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#endif
#include "keys.h"
#include "qsound.h"
#include "cl_tent.h"
#include "r_part_script.h"
#include "gl_model.h"
#include "tr_types.h"
#include "crc.h"
#include "sha1.h"
#include "screen.h"
#include "pr1vm.h"
#include "csqc_client.h"
#include "utils.h"
#include "sbar.h"
#include "net.h"
#include "wad.h"

static pr1vm_t *CSQCVM_Active(void)
{
	return PR1VM_Active();
}

// Pure float/vector server builtins reused on the client instance. They do not
// touch strings/edicts/sv state and use the G_* macros, which work because
// PR1VM_ExecuteProgram points pr_globals at the executing (client) VM's globals.
extern void PF_random(void);
extern void PF_normalize(void);
extern void PF_vlen(void);
extern void PF_vectoyaw(void);
extern void PF_vectoangles(void);
extern void PF_rint(void);
extern void PF_floor(void);
extern void PF_ceil(void);
extern void PF_fabs(void);
extern void PF_sin(void);
extern void PF_cos(void);
extern void PF_sqrt(void);
extern void PF_min(void);
extern void PF_max(void);
extern void PF_bound(void);

// One token store shared by every tokenizer (#441/#479/#514) and every reader
// (#442, `argc`, #515/#516): a token is an offset span into s_tok_src plus its
// parsed text, so argv and the index builtins cannot disagree.
#define CSQC_TOK_SRC 2048
#define CSQC_TOK_MAX 128
static char s_tok_src[CSQC_TOK_SRC];
static int s_tokn;
static int s_tok_start[CSQC_TOK_MAX];
static int s_tok_end[CSQC_TOK_MAX];
static char s_tok_text[CSQC_TOK_MAX][CSQC_TOK_SRC];

static char *CSQCVM_Str(int ofs)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return NULL;
	}
	return CSQC_Client_GetString(vm, *(int *)&vm->globals[ofs]);
}

static void CSQCVM_SetRetStr(char *s)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (vm) {
		PR1VM_ClientSetString(vm, (string_t *)&vm->globals[OFS_RETURN], s);
	}
}

// Concatenates string args from index `first` (PR1 convention: 3 float slots per
// argument) into a static buffer.
static char *CSQCVM_VarString(int first)
{
	pr1vm_t *vm = CSQCVM_Active();
	static char out[2048];
	int i;

	out[0] = 0;
	if (!vm) {
		return out;
	}
	for (i = first; i < vm->argc; i++) {
		char *s = CSQC_Client_GetString(vm, *(int *)&vm->globals[OFS_PARM0 + i * 3]);
		if (s) {
			strlcat(out, s, sizeof(out));
		}
	}
	return out;
}

// Entity values in the client VM are raw int byte offsets (N*edict_size), the
// interpreter's native convention (OP_STORE_ENT/PR1VM_ProgToEdict), not plain
// indices. All builtins must read entity args and write entity returns through
// these helpers; a float cast/assignment corrupts the bits.
static int CSQC_Ent_Of(pr1vm_t *vm, int parmofs);
static float *CSQC_Ent_Ofs(pr1vm_t *vm, int entnum, int fldofs);
static void CSQC_Ret_Entity(pr1vm_t *vm, int entnum);
static void CSQC_Add_One_Entity(int e, qbool run_predraw);

// SOLID/FL constants (csdefs.qc values).
#define CSQC_SOLID_NOT		0
#define CSQC_SOLID_TRIGGER	1
#define CSQC_SOLID_BSP		4
#define CSQC_SOLID_BSPTRIGGER	22
#define CSQC_FL_FLY			1
#define CSQC_FL_SWIM		2
#define CSQC_FL_MONSTER		32
#define CSQC_FL_ITEM		256
#define CSQC_FL_ONGROUND	512
#define CSQC_FL_PARTIALGROUND	1024
#define CSQC_FL_FINDABLE_NONSOLID	16384

/*
void(string s, ...) dprint = #25
*/
static void CSQC_Dprint(void)
{
	Con_DPrintf("%s", CSQCVM_VarString(0));
}

/*
 Port of Q_ftoa for #26 ftos: float -> string without losing significant digits,
 trimming trailing zeros.
*/
static void CSQC_Q_Ftoa(char *str, size_t maxlen, float in)
{
	unsigned int i = *((unsigned int *)&in);
	int signbit = (i & 0x80000000u) >> 31;
	int exp = (int)((i & 0x7F800000u) >> 23) - 127;
	int mantissa = (i & 0x007FFFFFu);
	char buf[64];
	char *p;

	if (exp == 128) {
		snprintf(buf, sizeof(buf), "%s%s", signbit ? "-" : "", mantissa == 0 ? "1.#INF" : "1.#NAN");
		strlcpy(str, buf, maxlen);
		return;
	}
	exp = -exp;
	exp = (int)(exp * 0.30102999957f); // base 2 -> base 10
	exp += 8;
	if (exp <= 0) {
		snprintf(buf, sizeof(buf), "%.0f", in);
	}
	else {
		char fmt[16];
		snprintf(fmt, sizeof(fmt), "%%.%if", exp);
		snprintf(buf, sizeof(buf), fmt, in);
		// trim trailing zeros and dot
		for (p = buf + strlen(buf) - 1; p > buf && *p == '0'; p--) {
			*p = '\0';
		}
		if (*p == '.') {
			*p = '\0';
		}
	}
	strlcpy(str, buf, maxlen);
}

/*
 string(float val) ftos = #26
 Integral values print as "%d"; others keep all significant digits.
*/
static void CSQC_Ftos(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float v;
	char buf[64];
	if (!vm) {
		return;
	}
	v = vm->globals[OFS_PARM0];
	if (v >= -2147483648.0f && v <= 2147483647.0f && v == (float)(int)v) {
		snprintf(buf, sizeof(buf), "%d", (int)v);
	}
	else {
		CSQC_Q_Ftoa(buf, sizeof(buf), v);
	}
	CSQCVM_SetRetStr(buf);
}

/*
void(string cmdname) registercommand = #352
*/
static void CSQC_Registercommand(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *cmd = CSQCVM_Str(OFS_PARM0);
	if (vm && cmd) {
		CSQC_Client_RegisterCommand(cmd);
	}
}

// Secret classes for bind text (#342): a bind is free text that may embed a
// password or token, so it is matched by substring. Bare "rcon" is not a class:
// rcon_status stays visible.
static const char *s_secret_classes[] = {
	"password",
	"passwd",
	"_token",
	"token_secret",
	"secret",
	"_authkey",
	"authkey",
	"_apikey",
	"api_key",
	"private_key",
	"access_key",
};

/*
 Read-guard for sensitive cvars (#45/#448/#482/#495): cvars flagged
 CVAR_NOUNSAFEEXPAND (rcon/password/token class) are hidden from the module.
 Neighbouring cvars (cl_timeout, cl_pext*, allow_scripts, ...) stay readable;
 the CVAR_NOTFROMSERVER write-guard covers a wider set.
*/
static qbool CSQC_CvarReadDenied(const char *name)
{
	cvar_t *var;

	if (!name) {
		return false;
	}
	var = Cvar_Find(name);
	return var && (var->flags & CVAR_NOUNSAFEEXPAND);
}

static qbool CSQC_BindSensitive(const char *s)
{
	int i;
	if (!s) {
		return false;
	}
	for (i = 0; i < (int)(sizeof(s_secret_classes) / sizeof(s_secret_classes[0])); i++) {
		if (strstr(s, s_secret_classes[i])) {
			return true;
		}
	}
	return false;
}

/*
 float(string varname) cvar = #45
 0 for unknown or read-guarded cvars.
*/
static void CSQC_Cvar(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *name;
	if (!vm) {
		return;
	}
	name = CSQCVM_Str(OFS_PARM0);
	vm->globals[OFS_RETURN] = (name && name[0] && !CSQC_CvarReadDenied(name)) ? Cvar_Value(name) : 0;
}

/*
 void(vector vang) makevectors = #1
 Writes the module's v_forward/v_right/v_up (no-op for globals it does not declare).
*/
static void CSQC_Makevectors(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	CSQC_Client_MakeVectors(&vm->globals[OFS_PARM0]);
}

/*
 void(vector dir) vectorvectors = #432
 Normalized dir -> v_forward, with orthogonal v_right/v_up.
*/
static void CSQC_Vectorvectors(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	CSQC_Client_VectorVectors(&vm->globals[OFS_PARM0]);
}

/*
 float() random = #7
 Result is in the open interval (0,1), never 0 or 1 (the server PF_random can
 return 1.0). Optional args: one scales the result, two (a, b) map it to a + r*(b-a).
*/
static void CSQC_Random(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float r;
	if (!vm) {
		return;
	}
	r = (float)(rand() & 0x7fff) / 0x8000 + (0.5f / 0x8000);
	if (vm->argc == 1) {
		r *= vm->globals[OFS_PARM0];
	}
	else if (vm->argc >= 2) {
		r = vm->globals[OFS_PARM0] + r * (vm->globals[OFS_PARM1] - vm->globals[OFS_PARM0]);
	}
	vm->globals[OFS_RETURN] = r;
}

/*
 float(vector v [, optional entity reference]) vectoyaw = #13
 Yaw truncated to whole degrees in [0,360). The optional entity (gravity axis) is
 ignored: the client tracks no gravitydir, so the identity axis is used.
*/
static void CSQC_Vectoyaw(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float *v;
	float x, y, yaw;
	if (!vm) {
		return;
	}
	v = &vm->globals[OFS_PARM0];
	x = v[0];
	y = v[1];
	if (y == 0 && x == 0) {
		yaw = 0;
	}
	else {
		yaw = (float)(int)(atan2(y, x) * 180 / M_PI);
		if (yaw < 0) {
			yaw += 360;
		}
	}
	vm->globals[OFS_RETURN] = yaw;
}

/*
 vector(vector fwd [, optional vector up]) vectoangles = #51
 Pitch is inverted (model pitch convention); the optional up vector yields roll.
*/
static void CSQC_Vectoangles(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	const float *forward;
	float *up;
	float result[3];

	if (!vm) {
		return;
	}
	forward = &vm->globals[OFS_PARM0];
	up = (vm->argc >= 2) ? &vm->globals[OFS_PARM0 + 3] : NULL;

	CSQC_VectorAngles(forward, up, result, true);
	vm->globals[OFS_RETURN] = result[0];
	vm->globals[OFS_RETURN + 1] = result[1];
	vm->globals[OFS_RETURN + 2] = result[2];
}

/*
 Parse one token with COM_StringParse semantics. qctokenize selects the module flavour:
 tokenize/tokenizebyseparator split the QC punctuation set and treat single quotes
 as quoting, while tokenize_console keeps them as text and drops block comments.
 Returns the position after the token, or NULL at the end of the input.
*/
static const char *CSQC_Parse_Token(const char *data, char *token, int tokenlen, qbool qctokenize)
{
	int c;
	int len = 0;
	qbool punct;

	token[0] = 0;
	if (!data) {
		return NULL;
	}

	for (;;) {
		while ((c = (unsigned char)*data), c <= ' ' && c != '\n') {
			if (!c) {
				return NULL;
			}
			data++;
		}
		if (c == '/' && data[1] == '/') {
			while (*data && *data != '\n') {
				data++;
			}
			continue;
		}
		if (c == '/' && data[1] == '*' && !qctokenize) {
			data += 2;
			while (*data) {
				if (*data == '*' && data[1] == '/') {
					data += 2;
					break;
				}
				data++;
			}
			continue;
		}
		break;
	}

	// A newline is its own token even for the console flavour: the only way to
	// reach one here is through a `//` comment (the caller skips plain whitespace).
	if (c == '\n') {
		token[0] = '\n';
		token[1] = 0;
		return data + 1;
	}

	if (c == '"') {
		data++;
		for (;;) {
			if (len >= tokenlen - 1) {
				token[len] = 0;
				return data;
			}
			c = (unsigned char)*data++;
			if (c == '"') {
				c = (unsigned char)*data;
				if (c != '"') {
					token[len] = 0;
					return data;
				}
				data++;
			}
			if (!c) {
				token[len] = 0;
				return data - 1;
			}
			token[len++] = (char)c;
		}
	}

	if (c == '\'' && qctokenize) {
		data++;
		for (;;) {
			if (len >= tokenlen - 1) {
				token[len] = 0;
				return data;
			}
			c = (unsigned char)*data++;
			if (c == '\'') {
				if (*data != '\'') {
					token[len] = 0;
					return data;
				}
				data++;
				token[len++] = '\'';
				continue;
			}
			if (!c) {
				token[len] = 0;
				return data - 1;
			}
			token[len++] = (char)c;
		}
	}

	punct = qctokenize && (c == '{' || c == '}' || c == ')' || c == '(' || c == ']' || c == '[' || c == '\'' || c == ':' || c == ',' || c == ';');
	if (punct) {
		token[0] = (char)c;
		token[1] = 0;
		return data + 1;
	}

	do {
		if (len >= tokenlen - 1) {
			break;
		}
		token[len++] = (char)c;
		data++;
		c = (unsigned char)*data;
	} while (c > 32 && !(qctokenize && (c == '{' || c == '}' || c == ')' || c == '(' || c == ']' || c == '[' || c == '\'' || c == ':' || c == ',' || c == ';')));
	token[len] = 0;
	return data;
}

/*
 Tokenize into the single shared store, skipping all bytes <= ' ' before each token.
 Limits: source 2048 bytes, 128 tokens, 2048 bytes per parsed token.
*/
static int CSQC_Tokenize_Source(const char *s, qbool qctokenize)
{
	const char *p;
	char *start;

	s_tokn = 0;
	if (!s) {
		s_tok_src[0] = 0;
		return 0;
	}
	strlcpy(s_tok_src, s, sizeof(s_tok_src));
	start = s_tok_src;
	p = start;
	while (s_tokn < CSQC_TOK_MAX) {
		while (*p && (unsigned char)*p <= ' ') {
			p++;
		}
		if (!*p) {
			break;
		}
		s_tok_start[s_tokn] = (int)(p - start);
		p = CSQC_Parse_Token(p, s_tok_text[s_tokn], CSQC_TOK_SRC, qctokenize);
		if (!p) {
			break;
		}
		s_tok_end[s_tokn] = (int)(p - start);
		s_tokn++;
	}
	return s_tokn;
}

/*
float(string s) tokenize = #441
*/
static void CSQC_Tokenize(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN] = CSQC_Tokenize_Source(CSQCVM_Str(OFS_PARM0), true);
}

/*
string(float n) argv = #442
*/
static void CSQC_Argv(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int n;
	if (!vm) {
		return;
	}
	n = (int)vm->globals[OFS_PARM0];
	if (n < 0) {
		n += s_tokn;
	}
	CSQCVM_SetRetStr((n < 0 || n >= s_tokn) ? "" : s_tok_text[n]);
}

/*
float() argc = #0 (name-mapped only)
*/
static void CSQC_Argc(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN] = s_tokn;
}

/*
 string(string s1, optional string s2, ...) strcat = #115
 The result is sized to fit (no fixed length cap); at most 16 arguments.
*/
static void CSQC_Strcat(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *parts[16];
	size_t lens[16];
	size_t total = 0, pos = 0;
	char *buf;
	int n, i;

	if (!vm) {
		return;
	}
	n = vm->argc;
	if (n <= 0) {
		n = 1;
	}
	if (n > 16) {
		n = 16;
	}
	// PR1 params are 3 float slots per argument
	for (i = 0; i < n; i++) {
		parts[i] = CSQC_Client_GetString(vm, *(int *)&vm->globals[OFS_PARM0 + i * 3]);
		lens[i] = parts[i] ? strlen(parts[i]) : 0;
		total += lens[i];
	}
	buf = (char *)Q_malloc(total + 1);
	for (i = 0; i < n; i++) {
		if (lens[i]) {
			memcpy(buf + pos, parts[i], lens[i]);
		}
		pos += lens[i];
	}
	buf[total] = 0;
	CSQCVM_SetRetStr(buf);
	Q_free(buf);
}

/*
 float(string s1, string sub, optional float startidx) strstrofs = #221
 0-based position or -1; a start outside [0,len] yields -1.
*/
static void CSQC_Strstrofs(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *hay, *needle, *p;
	int start, len;

	if (!vm) {
		return;
	}
	hay = CSQC_Client_GetString(vm, *(int *)&vm->globals[OFS_PARM0]);
	needle = CSQC_Client_GetString(vm, *(int *)&vm->globals[OFS_PARM1]);
	start = (vm->argc > 2) ? (int)vm->globals[OFS_PARM2] : 0;
	if (!hay) {
		hay = "";
	}
	if (!needle) {
		needle = "";
	}
	len = strlen(hay);
	if (start != 0 && (start < 0 || start > len)) {
		vm->globals[OFS_RETURN] = -1;
		return;
	}
	p = strstr(hay + start, needle);
	vm->globals[OFS_RETURN] = p ? (p - hay) : -1;
}

/*
 string(float ccase, float redalpha, float redchars, string str, ...) strconv = #224
 Bulk case/colour conversion. ccase 0 same/1 lower/2 upper; redalpha 0 same/1 white/2 red/
 5 alternate/6 alternate-alternate; redchars is the same for digits. The string
 argument is a vararg starting at position 3.
 */
static int CSQC_Chrconv_Number(int i, int base, int conv)
{
	i -= base;
	switch (conv) {
		default:
		case 5:
		case 6:
		case 0:
			break;
		case 1:
			base = '0';
			break;
		case 2:
			base = '0' + 128;
			break;
		case 3:
			base = '0' - 30;
			break;
		case 4:
			base = '0' + 128 - 30;
			break;
	}
	return i + base;
}

static int CSQC_Chrconv_Punct(int i, int base, int conv)
{
	i -= base;
	switch (conv) {
		default:
		case 0:
			break;
		case 1:
			base = 0;
			break;
		case 2:
			base = 128;
			break;
	}
	return i + base;
}

static int CSQC_Chrchar_Alpha(int i, int basec, int baset, int convc, int convt, int charnum)
{
	i -= baset + basec;
	switch (convt) {
		default:
		case 0:
			break;
		case 1:
			baset = 0;
			break;
		case 2:
			baset = 128;
			break;
		case 5:
		case 6:
			baset = 128 * ((charnum & 1) == (convt - 5));
			break;
	}
	switch (convc) {
		default:
		case 0:
			break;
		case 1:
			basec = 'a';
			break;
		case 2:
			basec = 'A';
			break;
	}
	return i + basec + baset;
}

static void CSQC_Strconv(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int ccase, redalpha, rednum, len, i;
	const unsigned char *string;
	unsigned char resbuf[2048];
	unsigned char *result = resbuf;

	if (!vm) {
		return;
	}
	ccase = (int)vm->globals[OFS_PARM0];
	redalpha = (int)vm->globals[OFS_PARM1];
	rednum = (int)vm->globals[OFS_PARM2];
	string = (const unsigned char *)CSQCVM_VarString(3);
	len = strlen((const char *)string);
	if (len >= (int)sizeof(resbuf)) {
		len = sizeof(resbuf) - 1;
	}

	for (i = 0; i < len; i++, string++, result++) {
		if (*string >= '0' && *string <= '9') {
			*result = CSQC_Chrconv_Number(*string, '0', rednum);
		}
		else if (*string >= '0' + 128 && *string <= '9' + 128) {
			*result = CSQC_Chrconv_Number(*string, '0' + 128, rednum);
		}
		else if (*string >= '0' + 128 - 30 && *string <= '9' + 128 - 30) {
			*result = CSQC_Chrconv_Number(*string, '0' + 128 - 30, rednum);
		}
		else if (*string >= '0' - 30 && *string <= '9' - 30) {
			*result = CSQC_Chrconv_Number(*string, '0' - 30, rednum);
		}
		else if (*string >= 'a' && *string <= 'z') {
			*result = CSQC_Chrchar_Alpha(*string, 'a', 0, ccase, redalpha, i);
		}
		else if (*string >= 'A' && *string <= 'Z') {
			*result = CSQC_Chrchar_Alpha(*string, 'A', 0, ccase, redalpha, i);
		}
		else if (*string >= 'a' + 128 && *string <= 'z' + 128) {
			*result = CSQC_Chrchar_Alpha(*string, 'a', 128, ccase, redalpha, i);
		}
		else if (*string >= 'A' + 128 && *string <= 'Z' + 128) {
			*result = CSQC_Chrchar_Alpha(*string, 'A', 128, ccase, redalpha, i);
		}
		else if ((*string & 127) < 16 || !redalpha) {
			*result = *string;
		}
		else if (*string < 128) {
			*result = CSQC_Chrconv_Punct(*string, 0, redalpha);
		}
		else {
			*result = CSQC_Chrconv_Punct(*string, 128, redalpha);
		}
	}
	*result = 0;
	CSQCVM_SetRetStr((char *)resbuf);
}

/*
 float(float property, ...) getproperty = #309
 Reads the current engine render state (r_refdef/cl/vid), not values the module
 set via #303. Set-only flags (DRAWWORLD etc.) read as 0.
 */
#define CSQC_VF_MIN		1	// viewport top-left (x,y)
#define CSQC_VF_MIN_X		2
#define CSQC_VF_MIN_Y		3
#define CSQC_VF_SIZE		4	// viewport width/height
#define CSQC_VF_SIZE_X		5
#define CSQC_VF_SIZE_Y		6
#define CSQC_VF_VIEWPORT	7	// (width, height)
#define CSQC_VF_FOV		8	// (fov_x, fov_y)
#define CSQC_VF_FOV_X		9
#define CSQC_VF_FOV_Y		10
#define CSQC_VF_ORIGIN		11
#define CSQC_VF_ORIGIN_X	12
#define CSQC_VF_ORIGIN_Y	13
#define CSQC_VF_ORIGIN_Z	14
#define CSQC_VF_ANGLES		15
#define CSQC_VF_ANGLES_X	16
#define CSQC_VF_ANGLES_Y	17
#define CSQC_VF_ANGLES_Z	18
#define CSQC_VF_CL_VIEWANGLES	33
#define CSQC_VF_CL_VIEWANGLES_X	34
#define CSQC_VF_CL_VIEWANGLES_Y	35
#define CSQC_VF_CL_VIEWANGLES_Z	36
#define CSQC_VF_AFOV		203
#define CSQC_VF_SCREENVSIZE	204
#define CSQC_VF_SCREENPSIZE	205

static void CSQC_Getproperty(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float prop;
	float *r;

	if (!vm) {
		return;
	}
	prop = vm->globals[OFS_PARM0];
	r = &vm->globals[OFS_RETURN];
	r[0] = r[1] = r[2] = 0;
	// No connection-state gate: CL_MakeActive runs CSQC_Init before cls.state
	// becomes ca_active, and a module reading VF_SCREENVSIZE there must get the
	// real size. Every case below reads plain engine structs; r_refdef values may
	// be stale before the first rendered frame.

	switch ((int)prop) {
		case CSQC_VF_SCREENVSIZE:
			// virtual size (vid.width/height; already scaled by r_conscale).
			r[0] = vid.width;
			r[1] = vid.height;
			break;
		case CSQC_VF_SCREENPSIZE:
			// physical framebuffer size
			r[0] = glConfig.vidWidth;
			r[1] = glConfig.vidHeight;
			break;
		case CSQC_VF_FOV:
			r[0] = r_refdef.fov_x;
			r[1] = r_refdef.fov_y;
			break;
		case CSQC_VF_FOV_X:
			r[0] = r_refdef.fov_x;
			break;
		case CSQC_VF_FOV_Y:
			r[0] = r_refdef.fov_y;
			break;
		case CSQC_VF_AFOV:
			// no stored afov; approximate with the fov cvar.
			r[0] = Cvar_Value("fov");
			break;
		case CSQC_VF_ORIGIN:
			VectorCopy(r_refdef.vieworg, r);
			break;
		case CSQC_VF_ORIGIN_X:
			r[0] = r_refdef.vieworg[0];
			break;
		case CSQC_VF_ORIGIN_Y:
			r[0] = r_refdef.vieworg[1];
			break;
		case CSQC_VF_ORIGIN_Z:
			r[0] = r_refdef.vieworg[2];
			break;
		case CSQC_VF_ANGLES:
			VectorCopy(r_refdef.viewangles, r);
			break;
		case CSQC_VF_ANGLES_X:
			r[0] = r_refdef.viewangles[0];
			break;
		case CSQC_VF_ANGLES_Y:
			r[0] = r_refdef.viewangles[1];
			break;
		case CSQC_VF_ANGLES_Z:
			r[0] = r_refdef.viewangles[2];
			break;
		case CSQC_VF_CL_VIEWANGLES:
			VectorCopy(cl.viewangles, r);
			break;
		case CSQC_VF_CL_VIEWANGLES_X:
			r[0] = cl.viewangles[0];
			break;
		case CSQC_VF_CL_VIEWANGLES_Y:
			r[0] = cl.viewangles[1];
			break;
		case CSQC_VF_CL_VIEWANGLES_Z:
			r[0] = cl.viewangles[2];
			break;
		case CSQC_VF_VIEWPORT:
			r[0] = r_refdef.vrect.width;
			r[1] = r_refdef.vrect.height;
			break;
		case CSQC_VF_MIN:
			r[0] = r_refdef.vrect.x;
			r[1] = r_refdef.vrect.y;
			break;
		case CSQC_VF_MIN_X:
			r[0] = r_refdef.vrect.x;
			break;
		case CSQC_VF_MIN_Y:
			r[0] = r_refdef.vrect.y;
			break;
		case CSQC_VF_SIZE:
			r[0] = r_refdef.vrect.width;
			r[1] = r_refdef.vrect.height;
			break;
		case CSQC_VF_SIZE_X:
			r[0] = r_refdef.vrect.width;
			break;
		case CSQC_VF_SIZE_Y:
			r[0] = r_refdef.vrect.height;
			break;
		default:
			// set-only flags (DRAWWORLD/PERSPECTIVE/...) and unmapped/DP-legacy -> 0
			break;
	}
}

/*
 vector(vector v) unproject = #310 / vector(vector v) project = #311.
 Screen<->world via the engine matrices. Failure/degenerate matrix -> '0 0 0'.
 */
static void CSQC_Project(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float sx = 0, sy = 0, sz = 0;
	if (!vm) {
		return;
	}
	if (!CSQC_Client_Project(&vm->globals[OFS_PARM0], &sx, &sy, &sz)) {
		sx = sy = sz = 0;
	}
	vm->globals[OFS_RETURN + 0] = sx;
	vm->globals[OFS_RETURN + 1] = sy;
	vm->globals[OFS_RETURN + 2] = sz;
}

static void CSQC_Unproject(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float world[3] = { 0, 0, 0 };
	if (!vm) {
		return;
	}
	if (!CSQC_Client_Unproject(vm->globals[OFS_PARM0], vm->globals[OFS_PARM0 + 1], vm->globals[OFS_PARM0 + 2], world)) {
		world[0] = world[1] = world[2] = 0;
	}
	vm->globals[OFS_RETURN + 0] = world[0];
	vm->globals[OFS_RETURN + 1] = world[1];
	vm->globals[OFS_RETURN + 2] = world[2];
}

/*
 void() clearscene = #300 / void(float mask) addentities = #301 /
 float(float property, ...) setproperty = #303 / void() renderscene = #304
 clearscene resets the #303 view properties and, in a scene takeover, the entity list.
 */
static void CSQC_Clearscene(void)
{
	CSQC_Client_ResetViewProps();
	if (CSQC_Client_SceneActive()) {
		CL_ClearScene();
	}
}
/*
 void(float mask) addentities = #301.
 mask&1 (MASK_ENGINE) adds the engine scene (world/players/entities), mask&2
 (MASK_STDVIEWMODEL) the engine view model; then every arena edict with
 `drawmask & mask` is added.
 */
static void CSQC_Addentities(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int mask, e;
	if (!vm || !CSQC_Client_SceneActive()) {
		return;
	}
	mask = (int)vm->globals[OFS_PARM0];
	if (mask & 1) {
		CL_EmitEntitiesKeepScene(); // merge without CL_ClearScene
	}
	if (mask & 2) {
		CSQC_Client_LinkViewModel();
	}
	for (e = 1; e < vm->num_edicts; e++) {
		float *dm;
		if (!CSQC_Client_EntUsed(e)) {
			continue;
		}
		dm = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_DRAWMASK));
		if (!dm || !((int)dm[0] & mask)) {
			continue;
		}
		CSQC_Add_One_Entity(e, true);
	}
}
/*
 float(float property, ...) setproperty = #303 (view subset).
 Handles VF_MIN/SIZE/VIEWPORT/FOV/ORIGIN/ANGLES (and _X/_Y/_Z); the values are
 applied to r_refdef after V_CalcRefdef when CSQC is active (1-frame lag:
 CSQC_UpdateView runs in the HUD phase). Other properties are no-ops.
 Returns 1 for a recognized VF_*, 0 otherwise.
 */
static void CSQC_Setproperty(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int prop, words, i;
	float args[6];
	if (!vm) {
		return;
	}
	prop = (int)vm->globals[OFS_PARM0];
	// after property: (argc-1) QC args, 3 words each; VF_VIEWPORT = vector+vector
	// (position+size) = 6 words.
	words = (vm->argc - 1) * 3;
	if (words < 0) {
		words = 0;
	}
	if (words > 6) {
		words = 6;
	}
	for (i = 0; i < words; i++) {
		args[i] = vm->globals[OFS_PARM0 + 3 + i];
	}
	vm->globals[OFS_RETURN] = CSQC_Client_SetViewProperty(prop, words, args);
}
static void CSQC_Renderscene(void)
{
	// Outside a scene takeover the engine draws the world itself.
	if (CSQC_Client_SceneActive()) {
		CSQC_Client_RenderScene();
	}
}

/*
 float(vector position, string text, vector size, vector rgb,
       float alpha, float drawflag) drawstring = #326

 Dual signature: with `argc >= 6` the form above (rgb=P3, alpha=P4, flag=P5),
 otherwise legacy DP (white, alpha=P3, flag=P4). Scale is size.x/8 (the font
 scales uniformly; size.y is ignored). Colour is independent of scr_coloredText.
 Returns -1 for a null string.
 */
static void CSQC_Drawstring(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float *g;
	int r, gg, b;
	float scale, alpha;
	char *s;
	if (!vm) {
		return;
	}
	g = vm->globals;
	s = CSQC_Client_GetString(vm, *(int *)&g[OFS_PARM0 + 3]);
	if (!s) {
		vm->globals[OFS_RETURN] = -1;
		return;
	}
	if (vm->argc >= 6) {
		r = (int)(bound(0, g[OFS_PARM0 + 9], 1) * 255.0f + 0.5f);
		gg = (int)(bound(0, g[OFS_PARM0 + 10], 1) * 255.0f + 0.5f);
		b = (int)(bound(0, g[OFS_PARM0 + 11], 1) * 255.0f + 0.5f);
		alpha = g[OFS_PARM0 + 12];
	}
	else {
		// legacy DP: (pos, text, size, alpha [, flag])
		r = gg = b = 255;
		alpha = g[OFS_PARM0 + 9];
	}
	// size.x over the 8px cell (0 => 1), times the drawfontscale x-multiplier.
	scale = ((g[OFS_PARM0 + 6] > 0) ? g[OFS_PARM0 + 6] / 8.0f : 1) * CSQC_Client_DrawFontScaleX(vm);
	CSQC_Client_DrawText(g[OFS_PARM0 + 0], g[OFS_PARM0 + 1], s, r, gg, b, alpha, scale);
}

/*
 float(float stnum) getstati = #330
 Returns the raw int bits of the stat, not a float; getstatf (#331) gives the
 numeric value. 0..31 come from cl.stats, 32..255 from extended stat storage.
*/
static void CSQC_Getstati(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	*(int *)&vm->globals[OFS_RETURN] = CSQC_Client_GetStatInt((int)vm->globals[OFS_PARM0]);
}

/*
 float(float stnum, optional float firstbit, optional float bitcount) getstatf = #331
   without extra args - the float stat value (statsf, from stat wire 79);
   with firstbit/bitcount - bit extraction from the int stat value.
*/
static void CSQC_Getstatf(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int stnum;
	if (!vm) {
		return;
	}
	stnum = (int)vm->globals[OFS_PARM0];
	if (stnum < 0 || stnum >= MAX_EXTENDED_CL_STATS) {
		vm->globals[OFS_RETURN] = 0;
		return;
	}
	if (vm->argc > 1) {
		// Exact int, not the float path: large ints lose low bits in float32.
		int val = CSQC_Client_GetStatInt(stnum);
		int first = (int)vm->globals[OFS_PARM1];
		int count = (vm->argc > 2) ? (int)vm->globals[OFS_PARM2] : 1;
		if (first < 0) {
			first = 0;
		}
		if (first > 31) {
			first = 31; // shift domain 0..31
		}
		if (count < 0) {
			count = 0;
		}
		if (first + count > 32) {
			count = 32 - first; // first+count <= 32
		}
		if (count > 31) { // keep (1 << count) from overflowing
			count = 31;
		}
		vm->globals[OFS_RETURN] = (float)((((unsigned int)val) & (((1u << count) - 1u) << first)) >> first);
	}
	else {
		vm->globals[OFS_RETURN] = CSQC_Client_GetStatFloat(stnum);
	}
}

/*
 string(float firststnum) getstats = #332
 String stat received via svc_fte_updatestatstring (stat wire 78). The legacy
 packed-int variant (4 int stats) is not implemented: the module only runs under
 FTE_PEXT_CSQC.
*/
static void CSQC_Getstats(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	CSQCVM_SetRetStr((char *)CSQC_Client_GetStatString((int)vm->globals[OFS_PARM0]));
}

/*
float(vector position, float character, vector size, vector rgb, float alpha,
     optional float drawflag) drawcharacter = #320
 Returns -1 for character 0, otherwise 1.
*/
static void CSQC_Drawcharacter(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float *g;
	float scale;
	if (!vm) {
		return;
	}
	g = vm->globals;
	scale = ((g[OFS_PARM0 + 6] > 0) ? g[OFS_PARM0 + 6] / 8.0f : 1) * CSQC_Client_DrawFontScaleX(vm);
	CSQC_Client_DrawCharacter(g[OFS_PARM0 + 0], g[OFS_PARM0 + 1], (int)g[OFS_PARM0 + 3],
		(int)(bound(0, g[OFS_PARM0 + 9], 1) * 255.0f + 0.5f),
		(int)(bound(0, g[OFS_PARM0 + 10], 1) * 255.0f + 0.5f),
		(int)(bound(0, g[OFS_PARM0 + 11], 1) * 255.0f + 0.5f),
		g[OFS_PARM0 + 12], scale);
	vm->globals[OFS_RETURN] = (g[OFS_PARM0 + 3] == 0) ? -1 : 1;
}

/*
float(vector position, string pic, vector size, vector rgb, float alpha,
     optional float drawflag) drawpic = #322
 Returns 1 if the pic was found, 0 otherwise.
*/
static void CSQC_Drawpic(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float *g;
	char *name;
	if (!vm) {
		return;
	}
	g = vm->globals;
	name = CSQC_Client_GetString(vm, *(int *)&g[OFS_PARM0 + 3]);
	vm->globals[OFS_RETURN] = CSQC_Client_DrawPic(g[OFS_PARM0 + 0], g[OFS_PARM0 + 1],
								  g[OFS_PARM0 + 6], g[OFS_PARM0 + 7], name,
								  (int)(bound(0, g[OFS_PARM0 + 9], 1) * 255.0f + 0.5f),
								  (int)(bound(0, g[OFS_PARM0 + 10], 1) * 255.0f + 0.5f),
								  (int)(bound(0, g[OFS_PARM0 + 11], 1) * 255.0f + 0.5f),
								  g[OFS_PARM0 + 12])
		? 1
		: 0;
}

/*
void(vector pos, vector sz, string pic, vector srcpos, vector srcsz, vector rgb,
     float alpha, optional float drawflag) drawsubpic = #328
 Always returns 1.
*/
static void CSQC_Drawsubpic(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float *g;
	char *name;
	if (!vm) {
		return;
	}
	g = vm->globals;
	name = CSQC_Client_GetString(vm, *(int *)&g[OFS_PARM0 + 6]);
	CSQC_Client_DrawSubPic(g[OFS_PARM0 + 0], g[OFS_PARM0 + 1],
		g[OFS_PARM0 + 3], g[OFS_PARM0 + 4], name,
		g[OFS_PARM0 + 9], g[OFS_PARM0 + 10], g[OFS_PARM0 + 12], g[OFS_PARM0 + 13],
		(int)(bound(0, g[OFS_PARM0 + 15], 1) * 255.0f + 0.5f),
		(int)(bound(0, g[OFS_PARM0 + 16], 1) * 255.0f + 0.5f),
		(int)(bound(0, g[OFS_PARM0 + 17], 1) * 255.0f + 0.5f),
		g[OFS_PARM0 + 18]);
	vm->globals[OFS_RETURN] = 1;
}

/*
float(vector position, vector size, vector rgb, float alpha,
     optional float drawflag) drawfill = #323
 Always returns 1.
*/
static void CSQC_Drawfill(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float *g;
	if (!vm) {
		return;
	}
	g = vm->globals;
	CSQC_Client_DrawFill(g[OFS_PARM0 + 0], g[OFS_PARM0 + 1],
		g[OFS_PARM0 + 3], g[OFS_PARM0 + 4],
		(int)(bound(0, g[OFS_PARM0 + 6], 1) * 255.0f + 0.5f),
		(int)(bound(0, g[OFS_PARM0 + 7], 1) * 255.0f + 0.5f),
		(int)(bound(0, g[OFS_PARM0 + 8], 1) * 255.0f + 0.5f),
		g[OFS_PARM0 + 9]);
	vm->globals[OFS_RETURN] = 1;
}

/*
void(float width, vector pos1, vector pos2, vector rgb, float alpha,
     optional float drawflag) drawline = #315
Width is ignored (hairline).
*/
static void CSQC_Drawline(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float *g;
	if (!vm) {
		return;
	}
	g = vm->globals;
	CSQC_Client_DrawLine(g[OFS_PARM0 + 3], g[OFS_PARM0 + 4], g[OFS_PARM0 + 6], g[OFS_PARM0 + 7],
		1,
		(int)(bound(0, g[OFS_PARM0 + 9], 1) * 255.0f + 0.5f),
		(int)(bound(0, g[OFS_PARM0 + 10], 1) * 255.0f + 0.5f),
		(int)(bound(0, g[OFS_PARM0 + 11], 1) * 255.0f + 0.5f),
		g[OFS_PARM0 + 12]);
}

/*
float(string text, float usecolours, optional vector fontsize) stringwidth = #327
*/
static void CSQC_Stringwidth(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *text;
	float fontsize_x, w;
	if (!vm) {
		return;
	}
	text = CSQC_Client_GetString(vm, *(int *)&vm->globals[OFS_PARM0]);
	// size.x is scaled by drawfontscale so the metric matches drawstring. Without
	// a size, 8 is used and the result is divided by 8 (DP compat: width in chars).
	fontsize_x = (vm->argc > 2) ? vm->globals[OFS_PARM0 + 6] : 8.0f;
	w = CSQC_Client_StringWidth(text ? text : "", vm->globals[OFS_PARM0 + 3] != 0, fontsize_x * CSQC_Client_DrawFontScaleX(vm));
	if (vm->argc <= 2) {
		w /= 8.0f;
	}
	vm->globals[OFS_RETURN] = w;
}

/*
 string(string name, optional float flags) precache_pic = #317
 Returns the argument's own string handle. Only with PRECACHE_PIC_TEST does a
 failed load return null. PRECACHE_PIC_DOWNLOAD is not supported.
*/
#define CSQC_PRECACHE_PIC_FROMWAD	1
#define CSQC_PRECACHE_PIC_TEST		512
static void CSQC_Precache_Pic(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *name;
	int flags;
	qbool ok;
	if (!vm) {
		return;
	}
	name = CSQCVM_Str(OFS_PARM0);
	flags = (vm->argc > 1) ? (int)vm->globals[OFS_PARM1] : 0;
	*(int *)&vm->globals[OFS_RETURN] = *(int *)&vm->globals[OFS_PARM0];
	if (!name || !name[0]) {
		return;
	}
	if (flags & CSQC_PRECACHE_PIC_FROMWAD) {
		ok = W_LumpExists(name);
	}
	else {
		ok = CSQC_Client_PrecachePic(name);
	}
	if (!ok && (flags & CSQC_PRECACHE_PIC_TEST)) {
		vm->globals[OFS_RETURN] = 0;
	}
}

/* float(string name) iscachedpic = #316 - pic already in cache (no load) */
static void CSQC_Iscachedpic(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *name;
	if (!vm) {
		return;
	}
	name = CSQCVM_Str(OFS_PARM0);
	vm->globals[OFS_RETURN] = name && CSQC_Client_IsCachedPic(name);
}

/* vector(string picname) drawgetimagesize = #318 - (w,h,0) of the loaded pic */
static void CSQC_Drawgetimagesize(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *name;
	float w = 0, h = 0;
	if (!vm) {
		return;
	}
	name = CSQCVM_Str(OFS_PARM0);
	if (name && CSQC_Client_PicSize(name, &w, &h)) {
		vm->globals[OFS_RETURN + 0] = w;
		vm->globals[OFS_RETURN + 1] = h;
		vm->globals[OFS_RETURN + 2] = 0;
	}
	else {
		vm->globals[OFS_RETURN + 0] = 0;
		vm->globals[OFS_RETURN + 1] = 0;
		vm->globals[OFS_RETURN + 2] = 0;
	}
}

/* void(string name) freepic = #319 - no-op: pics are shared with the engine */
static void CSQC_Freepic(void)
{
}

/*
 void(vector position, string text, vector scale, vector rgb, float alpha,
      optional float flag) drawrawstring = #321
 Layout like drawstring #326; "raw" text (no &c-prefix/parsing).
*/
static void CSQC_Drawrawstring(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float *g;
	int r, gg, b;
	float scale;
	char *s;
	if (!vm) {
		return;
	}
	g = vm->globals;
	s = CSQC_Client_GetString(vm, *(int *)&g[OFS_PARM0 + 3]);
	if (!s) {
		return;
	}
	r = (int)(bound(0, g[OFS_PARM0 + 9], 1) * 255.0f + 0.5f);
	gg = (int)(bound(0, g[OFS_PARM0 + 10], 1) * 255.0f + 0.5f);
	b = (int)(bound(0, g[OFS_PARM0 + 11], 1) * 255.0f + 0.5f);
	scale = ((g[OFS_PARM0 + 6] > 0) ? g[OFS_PARM0 + 6] / 8.0f : 1) * CSQC_Client_DrawFontScaleX(vm);
	CSQC_Client_DrawRawText(g[OFS_PARM0 + 0], g[OFS_PARM0 + 1], s, r, gg, b, g[OFS_PARM0 + 12], scale);
}

/* void(float x, float y, float width, float height) drawsetcliparea = #324 */
static void CSQC_Drawsetcliparea(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	CSQC_Client_SetClipArea(vm->globals[OFS_PARM0 + 0], vm->globals[OFS_PARM0 + 3],
		vm->globals[OFS_PARM0 + 6], vm->globals[OFS_PARM0 + 9]);
	vm->globals[OFS_RETURN] = 1;
}

/* void() drawresetcliparea = #325 */
static void CSQC_Drawresetcliparea(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	CSQC_Client_ResetClipArea();
	vm->globals[OFS_RETURN] = 1;
}

/*
void(vector pivot, string picname, vector size, vector mins, float angle,
     vector rgb, float alpha, optional float flag) drawrotpic_dp = #329
 No-op: the 2D path cannot draw a rotated textured quad.
*/
static void CSQC_Drawrotpic_Dp(void)
{
}

/*
 string(string fmt, ...) sprintf = #627
 Parses %[flags][width][.precision][length]conv; flags - 0 + ' ' #; width/precision
 are literal only (no '*'/'%$'). Length modifiers pick the argument domain: h is
 float, l/L is a raw int32, q/j/z/t are consumed (a 32-bit slot cannot widen to
 int64). Conversions d i u x X c s f g e + v ('x y z'). %i defaults to a raw-int
 read; every other conversion defaults to float. The VM stores functions, fields
 and entities as raw ints in float slots; entity values are byte offsets here, so
 print an edict number through num_for_edict (#512) instead of relying on %i.
 %o/%p/%S/%E/%F/%G/%V/%I are unsupported. An unknown conversion stops formatting
 and keeps the output produced so far.
*/
static char *CSQC_Sprintf_Put_Int(char *f, int v)
{
	char tmp[12];
	int n = 0;

	if (v <= 0) {
		*f++ = '0';
		return f;
	}
	while (v > 0 && n < (int)sizeof(tmp)) {
		tmp[n++] = (char)('0' + v % 10);
		v /= 10;
	}
	while (n > 0) {
		*f++ = tmp[--n];
	}
	return f;
}

static void CSQC_Sprintf(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char buf[2048];
	char tmp[512];
	const char *fmt, *p;
	int pn = 1;  // argument number after fmt (base = OFS_PARM0 + pn*3)
	size_t o = 0;

	if (!vm) {
		return;
	}
	fmt = CSQC_Client_GetString(vm, *(int *)&vm->globals[OFS_PARM0]);
	if (!fmt) {
		fmt = "";
	}

	for (p = fmt; *p && o < sizeof(buf) - 1; p++) {
		char conv, formatbuf[32], *f;
		int flags = 0, width = -1, prec = -1, haswidth = 0, isfloat = -1;

		if (*p != '%') {
			buf[o++] = *p;
			continue;
		}
		p++;
		if (*p == '%') {
			buf[o++] = '%';
			continue;
		}

		// flags: # alternate, 0 zeropad, - left, ' ' space, + sign
		for (;; p++) {
			if (*p == '#' || *p == '0' || *p == '-' || *p == ' ' || *p == '+') {
				flags |= 1 << (*p == '#' ? 0 : *p == '0' ? 1
								 : *p == '-'             ? 2
								 : *p == ' '             ? 3
														 : 4);
			}
			else {
				break;
			}
		}
		// width - literal number only (no '*')
		if (*p >= '1' && *p <= '9') {
			int nd = 0;
			width = 0;
			haswidth = 1;
			while (*p >= '0' && *p <= '9' && nd < 9) {
				width = width * 10 + (*p++ - '0'), nd++;
			}
			while (*p >= '0' && *p <= '9') {
				p++;
			}
			if (width > 2047) {
				width = 2047;
			}
		}
		// precision - literal number only
		if (*p == '.') {
			int nd = 0;
			p++;
			prec = 0;
			while (*p >= '0' && *p <= '9' && nd < 9) {
				prec = prec * 10 + (*p++ - '0'), nd++;
			}
			while (*p >= '0' && *p <= '9') {
				p++;
			}
			if (prec > 2047) {
				prec = 2047;
			}
		}
		// Length modifiers; repeats are allowed and the last one wins.
		for (;; p++) {
			if (*p == 'h') {
				isfloat = 1;
			}
			else if (*p == 'l' || *p == 'L') {
				isfloat = 0;
			}
			else if (*p != 'q' && *p != 'j' && *p != 'z' && *p != 't') {
				break;
			}
		}
		conv = *p;
		if (!conv) {
			break;
		}
		if (conv == 'i') {
			if (isfloat < 0) {
				isfloat = 0;
			}
		}
		else if (isfloat < 0) {
			isfloat = 1;
		}

		// build the C format: %[#][0][-][ ][+][width][.prec]
		f = formatbuf;
		*f++ = '%';
		if (conv != 's' && conv != 'c' && (flags & 1)) {
			*f++ = '#';
		}
		if (flags & 2) {
			*f++ = '0';
		}
		if (flags & 4) {
			*f++ = '-';
		}
		if (flags & 8) {
			*f++ = ' ';
		}
		if (flags & 16) {
			*f++ = '+';
		}
		if (haswidth) {
			f = CSQC_Sprintf_Put_Int(f, width);
		}
		if (prec >= 0) {
			*f++ = '.';
			f = CSQC_Sprintf_Put_Int(f, prec);
		}

		if (conv == 'v') {
			double x = 0, y = 0, z = 0;
			char vfmt[192];
			*f++ = 'g';
			*f = 0;
			if (pn < vm->argc) {
				int ofs = OFS_PARM0 + pn * 3;

				if (isfloat) {
					x = (double)vm->globals[ofs];
					y = (double)vm->globals[ofs + 1];
					z = (double)vm->globals[ofs + 2];
				}
				else {
					x = (double)*(int *)&vm->globals[ofs];
					y = (double)*(int *)&vm->globals[ofs + 1];
					z = (double)*(int *)&vm->globals[ofs + 2];
				}
			}
			pn++;
			snprintf(vfmt, sizeof(vfmt), "%s %s %s", formatbuf, formatbuf, formatbuf);
			snprintf(tmp, sizeof(tmp), vfmt, x, y, z);
		}
		else if (conv == 's') {
			const char *s = "";
			*f++ = 's';
			*f = 0;
			if (pn < vm->argc) {
				int off = *(int *)&vm->globals[OFS_PARM0 + pn * 3];
				char *gs = CSQC_Client_GetString(vm, off);
				// Validation: a non-negative offset must lie in the module's string
				// area; negative offsets are temporary tables.
				if (gs && off >= 0 && (unsigned)off >= (unsigned)vm->progs->numstrings) {
					gs = NULL;
				}
				if (gs) {
					s = gs;
				}
			}
			pn++;
			snprintf(tmp, sizeof(tmp), formatbuf, s);
		}
		else {
			int argok = (pn < vm->argc);
			int ofs = OFS_PARM0 + pn * 3;
			float fv = argok ? vm->globals[ofs] : 0;
			int iv = argok ? *(int *)&vm->globals[ofs] : 0;

			switch (conv) {
				case 'd':
				case 'i':
					*f++ = 'd';
					*f = 0;
					if (isfloat) {
						snprintf(tmp, sizeof(tmp), formatbuf, (int)fv);
					}
					else {
						snprintf(tmp, sizeof(tmp), formatbuf, iv);
					}
					pn++;
					break;
				case 'u':
					*f++ = 'u';
					*f = 0;
					snprintf(tmp, sizeof(tmp), formatbuf, isfloat ? (unsigned)(int)fv : (unsigned)iv);
					pn++;
					break;
				case 'x':
				case 'X':
					*f++ = conv;
					*f = 0;
					snprintf(tmp, sizeof(tmp), formatbuf, isfloat ? (unsigned)(int)fv : (unsigned)iv);
					pn++;
					break;
				case 'c':
					*f++ = 'c';
					*f = 0;
					snprintf(tmp, sizeof(tmp), formatbuf, isfloat ? (int)fv : iv);
					pn++;
					break;
				case 'f':
				case 'e':
				case 'g':
					*f++ = conv;
					*f = 0;
					snprintf(tmp, sizeof(tmp), formatbuf, isfloat ? (double)fv : (double)iv);
					pn++;
					break;
				default:
					goto finished;
			}
		}

		{
			size_t l = strlen(tmp);
			if (o + l >= sizeof(buf)) {
				l = sizeof(buf) - 1 - o;
			}
			memcpy(buf + o, tmp, l);
			o += l;
		}
	}
finished:
	buf[o] = 0;
	CSQCVM_SetRetStr(buf);
}

/*
 void(string evname, string evargs, ...) sendevent = #359
 Writes clcfte_qcrequest (81):
   [byte 81] then up to 6 args "[byte type][value]", then [byte 0 (ev_void
   terminator)] and [string evname].
 Types: 's'=1 ev_string+string, 'f'=2 ev_float+float, 'v'=3 ev_vector+3 floats,
 'i'=8 ev_integer+long (raw bits from the float slot), 'e'=4 ev_entity+entity
 (arena edict -> server number from the .entnum field; invalid/freed -> world(0)).
 An unknown type character (incl. '\0') ends the argument list; 'u'/'F'/'I'/'p'
 are not supported. Requires a connection with negotiated FTE_PEXT_CSQC: a server
 without CSQC would drop the client. The 200+seat byte is only written for seat > 0;
 the client is single-seat, so it is never written.
*/
#define CSQC_EV_VOID	0
#define CSQC_EV_STRING	1
#define CSQC_EV_FLOAT	2
#define CSQC_EV_VECTOR	3
#define CSQC_EV_ENTITY	4
#define CSQC_EV_INTEGER	8

/* Entity number in MSG_WriteEntity format. A negative number degenerates to
   world(0) rather than ending the game: the module must not crash the client. */
static void CSQC_Sendevent_Write_Entity(int entnum)
{
	if (entnum < 0) {
		entnum = 0;
	}
	if (entnum >= 0x8000) {
		MSG_WriteShort(&cls.netchan.message, (entnum >> 8) | 0x8000);
		MSG_WriteByte(&cls.netchan.message, entnum & 0xff);
	}
	else {
		MSG_WriteShort(&cls.netchan.message, entnum);
	}
}

static void CSQC_Sendevent(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	const char *evname, *argtypes;
	char c;
	int i;

	if (!vm) {
		return;
	}
	// Any connection, not ca_active: CSQC_Init runs from CL_MakeActive before
	// cls.state = ca_active, and events sent from CSQC_Init must reach the server.
	if (!cls.state) {
		return;
	}
#ifdef PROTOCOL_VERSION_FTE
	// Negotiated snapshot, not the live cvar.
	if (!(cls.fteprotocolextensions & FTE_PEXT_CSQC)) {
		return;
	}
#endif

	evname = CSQCVM_Str(OFS_PARM0);
	argtypes = CSQCVM_Str(OFS_PARM1);
	if (!evname || !argtypes) {
		return;
	}

	MSG_WriteByte(&cls.netchan.message, clcfte_qcrequest);

	for (i = 0; i < 6; i++) {
		int base = OFS_PARM2 + i * 3;
		c = argtypes[i];
		if (c == 's') {
			char *s = CSQC_Client_GetString(vm, *(int *)&vm->globals[base]);
			MSG_WriteByte(&cls.netchan.message, CSQC_EV_STRING);
			MSG_WriteString(&cls.netchan.message, s ? s : "");
		}
		else if (c == 'f') {
			MSG_WriteByte(&cls.netchan.message, CSQC_EV_FLOAT);
			MSG_WriteFloat(&cls.netchan.message, vm->globals[base]);
		}
		else if (c == 'v') {
			MSG_WriteByte(&cls.netchan.message, CSQC_EV_VECTOR);
			MSG_WriteFloat(&cls.netchan.message, vm->globals[base + 0]);
			MSG_WriteFloat(&cls.netchan.message, vm->globals[base + 1]);
			MSG_WriteFloat(&cls.netchan.message, vm->globals[base + 2]);
		}
		else if (c == 'i') {
			MSG_WriteByte(&cls.netchan.message, CSQC_EV_INTEGER);
			MSG_WriteLong(&cls.netchan.message, *(int *)&vm->globals[base]);
		}
		else if (c == 'e') {
			int slot = CSQC_Ent_Of(vm, base);
			float *f = (slot > 0 && CSQC_Client_EntUsed(slot))
				? CSQC_Ent_Ofs(vm, slot, CSQC_Client_FieldOfs(vm, CSQC_FLD_ENTNUM))
				: NULL;
			MSG_WriteByte(&cls.netchan.message, CSQC_EV_ENTITY);
			CSQC_Sendevent_Write_Entity(f ? (int)(*f) : 0);
		}
		else {
			break;
		}
	}

	MSG_WriteByte(&cls.netchan.message, CSQC_EV_VOID);
	MSG_WriteString(&cls.netchan.message, evname);
}

/*
 read* builtins: read from the current network message; valid only while the
 engine is dispatching a message to the module (otherwise the VM is aborted).
*/
static void CSQC_Readbyte(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	if (!CSQC_Client_MayRead()) {
		CSQC_Client_Abort("PF_ReadByte is not valid at this time");
		vm->globals[OFS_RETURN] = -1;
		return;
	}
	vm->globals[OFS_RETURN] = MSG_ReadByte();
}

/*
 float() readchar = #361
 Signed byte from the current network message.
*/
static void CSQC_Readchar(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	if (!CSQC_Client_MayRead()) {
		CSQC_Client_Abort("PF_ReadChar is not valid at this time");
		vm->globals[OFS_RETURN] = -1;
		return;
	}
	vm->globals[OFS_RETURN] = MSG_ReadChar();
}

static void CSQC_Readshort(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	if (!CSQC_Client_MayRead()) {
		CSQC_Client_Abort("PF_ReadShort is not valid at this time");
		vm->globals[OFS_RETURN] = -1;
		return;
	}
	vm->globals[OFS_RETURN] = MSG_ReadShort();
}

static void CSQC_Readlong(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	if (!CSQC_Client_MayRead()) {
		CSQC_Client_Abort("PF_ReadLong is not valid at this time");
		vm->globals[OFS_RETURN] = -1;
		return;
	}
	vm->globals[OFS_RETURN] = MSG_ReadLong();
}

/*
 float() readcoord = #364 / string() readstring = #366
*/
static void CSQC_Readcoord(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	if (!CSQC_Client_MayRead()) {
		CSQC_Client_Abort("PF_ReadCoord is not valid at this time");
		vm->globals[OFS_RETURN] = -1;
		return;
	}
	vm->globals[OFS_RETURN] = MSG_ReadCoord();
}

/*
 float() readangle = #365
*/
static void CSQC_Readangle(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	if (!CSQC_Client_MayRead()) {
		CSQC_Client_Abort("PF_ReadAngle is not valid at this time");
		vm->globals[OFS_RETURN] = -1;
		return;
	}
	vm->globals[OFS_RETURN] = MSG_ReadAngle();
}

static void CSQC_Readstring(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *s;
	if (!vm) {
		return;
	}
	if (!CSQC_Client_MayRead()) {
		CSQC_Client_Abort("PF_ReadString is not valid at this time");
		vm->globals[OFS_RETURN] = 0;
		return;
	}
	s = MSG_ReadString();
	PR1VM_ClientSetString(vm, (string_t *)&vm->globals[OFS_RETURN], s);
}

/*
 float() readfloat = #367
*/
static void CSQC_Readfloat(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	if (!CSQC_Client_MayRead()) {
		CSQC_Client_Abort("PF_ReadFloat is not valid at this time");
		vm->globals[OFS_RETURN] = -1;
		return;
	}
	vm->globals[OFS_RETURN] = MSG_ReadFloat();
}

/*
 PEXT2_REPLACEMENTDELTAS is not defined by qwprot. The client does not announce
 the bit, so the branch that tests it is forward-compat only.
*/
#ifndef PEXT2_REPLACEMENTDELTAS
#define PEXT2_REPLACEMENTDELTAS 0x00000008
#endif

/*
 Edict number from the current stream. With PEXT2_REPLACEMENTDELTAS: a short, and
 if bit 0x8000 is set, ((short & 0x7fff) << 8) | byte; otherwise an unsigned short.
*/
static int CSQC_Client_ReadEntityNum(void)
{
	if (cls.fteprotocolextensions2 & PEXT2_REPLACEMENTDELTAS) {
		int num = MSG_ReadShort();
		if (num & 0x8000) {
			num = ((num & 0x7fff) << 8) | MSG_ReadByte();
		}
		return num;
	}
	return (unsigned short)MSG_ReadShort();
}

static void CSQC_Readentitynum(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	if (!CSQC_Client_MayRead()) {
		CSQC_Client_Abort("PF_ReadEntityNum is not valid at this time");
		vm->globals[OFS_RETURN] = -1;
		return;
	}
	vm->globals[OFS_RETURN] = CSQC_Client_ReadEntityNum();
}

/*
 string(float keynum) keynumtostring = #340
 #340/#341 take and return QC/DP key codes, translated to and from engine keynums.
*/
static void CSQC_Keynumtostring(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	CSQCVM_SetRetStr(Key_KeynumToString(CSQC_Client_QCToKeynum((int)vm->globals[OFS_PARM0])));
}

/*
 float(string keyname) stringtokeynum = #341
 Empty string or unknown name -> -1.
*/
static void CSQC_Stringtokeynum(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *name = CSQCVM_Str(OFS_PARM0);
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN] = name ? CSQC_Client_KeynumToQC(Key_StringToKeynum(name)) : -1;
}

/*
 float() isdemo = #349
 0 - not demo; 1 - normal demo; 2 - MVD/QTV playback.
*/
static void CSQC_Isdemo(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN] = cls.mvdplayback ? 2 : (cls.demoplayback ? 1 : 0);
}

/*
 string(string key) serverkey = #354
 Synthetic keys ip/maxplayers/protocol/dlstate, otherwise serverinfo. protocol is a
 fixed "QuakeWorld[ demo]" string and dlstate is only the download percent.
*/
static const char *CSQC_Serverkey_Value(const char *key, char *buf, size_t bufsize)
{
	if (!strcmp(key, "ip")) {
		return cls.demoplayback ? cls.demoname : NET_AdrToString(cls.netchan.remote_address);
	}
	if (!strcmp(key, "maxplayers")) {
		snprintf(buf, bufsize, "%d", cl.sv_maxclients);
		return buf;
	}
	if (!strcmp(key, "protocol")) {
		return cls.demoplayback ? "QuakeWorld demo" : "QuakeWorld";
	}
	if (!strcmp(key, "dlstate")) {
		if (!cls.download) {
			return "";
		}
		snprintf(buf, bufsize, "%d", (int)cls.downloadpercent);
		return buf;
	}
	return Info_ValueForKey(cl.serverinfo, (char *)key);
}

static void CSQC_Serverkey(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *key = CSQCVM_Str(OFS_PARM0);
	char buf[64];
	const char *v;
	if (!vm) {
		return;
	}
	v = CSQC_Serverkey_Value(key ? key : "", buf, sizeof(buf));
	CSQCVM_SetRetStr((char *)(v ? v : ""));
}

// float(string key, optional float assumevalue) serverkeyfloat = #0
// The key as a number; assumevalue, else 0, when it has no value.
static void CSQC_Serverkeyfloat(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *key = CSQCVM_Str(OFS_PARM0);
	char buf[64];
	const char *v;
	if (!vm) {
		return;
	}
	v = CSQC_Serverkey_Value(key ? key : "", buf, sizeof(buf));
	if (v && v[0]) {
		vm->globals[OFS_RETURN] = (float)strtod(v, NULL);
	}
	else {
		vm->globals[OFS_RETURN] = (vm->argc > 1) ? vm->globals[OFS_PARM0 + 3] : 0;
	}
}

/*
 void(float sens) setsensitivityscaler = #346
 Temporary mouse sensitivity multiplier (e.g. for zoom).
*/
static void CSQC_Setsensitivityscaler(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	CSQC_Client_SetSensitivityScale(vm->globals[OFS_PARM0]);
}

/*
 float(float inputsequencenum) getinputstate = #345
 Fills the input_* globals from the sent usercmd history. seq is in the
 cls.netchan.outgoing_sequence domain; the meaningful range is
 (servercommandframe, clientcommandframe], which is not enforced. Returns 0 if seq
 is outside the 64-entry history ring or the game is paused. clientcommandframe is
 the last sent seq, so #345(clientcommandframe) returns the live pending frame
 outside CSQC_Input_Frame.
*/
static void CSQC_Getinputstate(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	unsigned int seq;
	if (!vm) {
		return;
	}
	seq = (unsigned int)vm->globals[OFS_PARM0];
	vm->globals[OFS_RETURN] = CSQC_Client_ApplyInput(seq);
}

/*
 void(entity ent) runstandardplayerphysics = #347
 PM_PlayerMove over the input_* globals (the module calls getinputstate(seq)
 first), against world+ent+players, reading .mins/.maxs/.gravity/.pmove_flags/.flags
 and writing .flags/.pmove_flags plus the deprecated pmove_org/vel/onground.
*/
static void CSQC_Runstandardplayerphysics(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int entnum;
	if (!vm) {
		return;
	}
	entnum = CSQC_Ent_Of(vm, OFS_PARM0);
	CSQC_Client_RunPlayerPhysics(entnum);
}

/*
 entity(float entnum) edict_num = #459
 Returns the entity value (raw int bits N*edict_size, not a float). Outside the
 arena range -> 0 (world).
*/
static void CSQC_Edict_Num(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int entnum;
	if (!vm) {
		return;
	}
	entnum = (int)vm->globals[OFS_PARM0];
	if (entnum < 0 || vm->max_edicts <= 0 || entnum >= vm->max_edicts) {
		CSQC_Ret_Entity(vm, 0);
		return;
	}
	CSQC_Ret_Entity(vm, entnum);
}

/*
 String buffers #460-469 (DP). Storage lives in csqc_client.c and deep-copies
 strings; these are thin wrappers.
*/
static int CSQCVM_ArgInt(int idx)
{
	pr1vm_t *vm = CSQCVM_Active();
	return (vm && vm->argc > idx) ? (int)vm->globals[OFS_PARM0 + idx * 3] : 0;
}

static char *CSQCVM_ArgStr(int idx)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm || vm->argc <= idx) {
		return NULL;
	}
	return CSQC_Client_GetString(vm, *(int *)&vm->globals[OFS_PARM0 + idx * 3]);
}

// strbuf(optional string type, optional float flags) buf_create = #460
// Only type "string" exists; anything else -> -1.
static void CSQC_Buf_Create(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *type;
	if (!vm) {
		return;
	}
	type = CSQCVM_ArgStr(0);
	if (vm->argc > 0 && strcasecmp(type ? type : "", "string")) {
		vm->globals[OFS_RETURN] = -1;
		return;
	}
	vm->globals[OFS_RETURN] = CSQC_Client_BufCreate();
}

// void(strbuf bufhandle) buf_del = #461
static void CSQC_Buf_Del(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (vm) {
		CSQC_Client_BufDel(CSQCVM_ArgInt(0));
	}
}

// float(strbuf bufhandle) buf_getsize = #462
static void CSQC_Buf_Getsize(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (vm) {
		vm->globals[OFS_RETURN] = CSQC_Client_BufGetSize(CSQCVM_ArgInt(0));
	}
}

// void(strbuf bufhandle_from, strbuf bufhandle_to) buf_copy = #463
static void CSQC_Buf_Copy(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (vm) {
		CSQC_Client_BufCopy(CSQCVM_ArgInt(0), CSQCVM_ArgInt(1));
	}
}

// void(strbuf bufhandle, float sortprefixlen, float backward) buf_sort = #464
static void CSQC_Buf_Sort(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (vm) {
		CSQC_Client_BufSort(CSQCVM_ArgInt(0), CSQCVM_ArgInt(1), CSQCVM_ArgInt(2) != 0);
	}
}

// string(strbuf bufhandle, string glue) buf_implode = #465
static void CSQC_Buf_Implode(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *s;
	if (!vm) {
		return;
	}
	s = CSQC_Client_BufImplode(CSQCVM_ArgInt(0), CSQCVM_ArgStr(1));
	CSQCVM_SetRetStr(s ? s : "");
	Q_free(s);
}

// string(strbuf bufhandle, float string_index) bufstr_get = #466
static void CSQC_Bufstr_Get(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	const char *s;
	if (!vm) {
		return;
	}
	s = CSQC_Client_BufGet(CSQCVM_ArgInt(0), CSQCVM_ArgInt(1));
	CSQCVM_SetRetStr((char *)(s ? s : ""));
}

// void(strbuf bufhandle, float string_index, string str) bufstr_set = #467
static void CSQC_Bufstr_Set(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *s;
	if (!vm) {
		return;
	}
	s = CSQCVM_ArgStr(2);
	CSQC_Client_BufSet(CSQCVM_ArgInt(0), CSQCVM_ArgInt(1), s ? s : "");
}

// float(strbuf bufhandle, string str, float order) bufstr_add = #468
static void CSQC_Bufstr_Add(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *s;
	if (!vm) {
		return;
	}
	s = CSQCVM_ArgStr(1);
	vm->globals[OFS_RETURN] = CSQC_Client_BufAdd(CSQCVM_ArgInt(0), s ? s : "", CSQCVM_ArgInt(2));
}

// void(strbuf bufhandle, float string_index) bufstr_free = #469
static void CSQC_Bufstr_Free(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (vm) {
		CSQC_Client_BufFree(CSQCVM_ArgInt(0), CSQCVM_ArgInt(1));
	}
}

/*
 void(string soundname, optional float channel, optional float volume) localsound = #177
 The channel is ignored; volume is 0..1 (default 1).
*/
static void CSQC_Localsound(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float *g;
	char *name;
	float vol;
	if (!vm) {
		return;
	}
	g = vm->globals;
	name = CSQC_Client_GetString(vm, *(int *)&g[OFS_PARM0]);
	vol = (vm->argc > 2) ? g[OFS_PARM0 + 6] : 1;
	if (name && name[0]) {
		S_LocalSoundWithVol(name, vol);
	}
}

/*
 float(vector org, float radius, vector lightcolours, optional float style, ...)
 dynamiclight_add = #305
 A custom-colour dlight for the frame being drawn. style/cubemap/pflags are ignored.
 Returns the dlight slot index + 1, or 0 if none is free.
*/
static void CSQC_Dynamiclight_Add(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float *g;
	dlight_t *dl;
	if (!vm) {
		return;
	}
	g = vm->globals;
	dl = CL_AllocDlight(0);
	if (!dl) {
		vm->globals[OFS_RETURN] = 0;
		return;
	}
	VectorCopy(&g[OFS_PARM0], dl->origin);
	dl->radius = g[OFS_PARM0 + 3];
	// Already expired, so CL_DecayLights frees it after this frame is drawn; the
	// module adds its lights again every frame.
	dl->die = cl.time - 0.1f;
	dl->type = lt_custom;
	dl->bubble = 0;
	dl->color[0] = (byte)bound(0, g[OFS_PARM0 + 6] * 255.0f, 255);
	dl->color[1] = (byte)bound(0, g[OFS_PARM0 + 7] * 255.0f, 255);
	dl->color[2] = (byte)bound(0, g[OFS_PARM0 + 8] * 255.0f, 255);
	vm->globals[OFS_RETURN] = (float)(int)(dl - cl_dlights) + 1;
}

/*
 Particles #335-337, #374 and #502. A name the server numbered with svc_fte_precache
 has that number as its handle. Any other name gets -i into a per-module table;
 every name gets a slot, so 0 means only "table full". A name resolves to the loaded
 particle sets' effect, looked up again whenever the sets reload. Without one, the
 classic effects below stand in under their usual names (mostly the DP effectinfo
 names), matched case-insensitively, for both kinds of handle. A trail name stands
 for the classic trail the engine draws for the same event.
*/
typedef enum {
	CSQC_PE_TRAIL,  // arg = trail_type_t
	CSQC_PE_POINT,  // R_RunParticleEffect, arg = colour, count * mult
	CSQC_PE_EXPLOSION,
	CSQC_PE_TAREXPLOSION,
	CSQC_PE_LAVASPLASH,
	CSQC_PE_TELEPORT
} csqc_pekind_t;

typedef struct
{
	const char *name;
	int kind, arg, mult;
} csqc_peffect_def_t;

typedef struct
{
	char *name;
	const csqc_peffect_def_t *def;  // classic stand-in, NULL for none
	int type;  // scripted effect type, valid while refresh is pscript_refreshcount
	int refresh;
} CSQC_Peffect_T;

static const csqc_peffect_def_t s_peffect_defs[] = {
	{ "tr_rocket", CSQC_PE_TRAIL, ROCKET_TRAIL, 0 },
	{ "tr_altrocket", CSQC_PE_TRAIL, ALT_ROCKET_TRAIL, 0 },
	{ "tr_slightblood", CSQC_PE_TRAIL, BIG_BLOOD_TRAIL, 0 },
	{ "tr_grenade", CSQC_PE_TRAIL, GRENADE_TRAIL, 0 },
	{ "tr_blood", CSQC_PE_TRAIL, BLOOD_TRAIL, 0 },
	{ "tr_wizspike", CSQC_PE_TRAIL, TRACER1_TRAIL, 0 },
	{ "tr_knightspike", CSQC_PE_TRAIL, TRACER2_TRAIL, 0 },
	{ "tr_vorespike", CSQC_PE_TRAIL, VOOR_TRAIL, 0 },
	{ "te_tarexplosion", CSQC_PE_TAREXPLOSION, 0, 0 },
	{ "te_lavasplash", CSQC_PE_LAVASPLASH, 0, 0 },
	{ "te_explosion", CSQC_PE_EXPLOSION, 0, 0 },
	{ "te_teleport", CSQC_PE_TELEPORT, 0, 0 },
	{ "te_qwgunshot", CSQC_PE_POINT, 0, 20 },
	{ "te_qwblood", CSQC_PE_POINT, 73, 20 },
	{ "te_lightningblood", CSQC_PE_POINT, 225, 50 },
};

#define CSQC_MAX_PEFFECTS	1024
static CSQC_Peffect_T s_peffects[CSQC_MAX_PEFFECTS]; // [0] unused
static int s_npeffects = 1;

void CSQCVM_ClearParticleEffects(void)
{
	int i;

	for (i = 1; i < s_npeffects; i++) {
		Q_free(s_peffects[i].name);
	}
	memset(s_peffects, 0, sizeof(s_peffects));
	s_npeffects = 1;
}

static const csqc_peffect_def_t *CSQC_Peffect_Def(const char *name)
{
	int i;

	for (i = 0; i < (int)(sizeof(s_peffect_defs) / sizeof(s_peffect_defs[0])); i++) {
		if (!strcasecmp(name, s_peffect_defs[i].name)) {
			return &s_peffect_defs[i];
		}
	}
	return NULL;
}

// float(string effectname) particleeffectnum = #335
static void CSQC_Particleeffectnum(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	CSQC_Peffect_T *pe;
	char *name;
	int i;
	if (!vm) {
		return;
	}
	name = CSQCVM_Str(OFS_PARM0);
	if (!name) {
		name = "";
	}
	i = CL_FindServerParticleEffect(name);
	if (i) {
		vm->globals[OFS_RETURN] = i;
		return;
	}
	for (i = 1; i < s_npeffects; i++) {
		if (!strcasecmp(name, s_peffects[i].name)) {
			vm->globals[OFS_RETURN] = -i;
			return;
		}
	}
	if (s_npeffects == CSQC_MAX_PEFFECTS) {
		vm->globals[OFS_RETURN] = 0;
		return;
	}
	pe = &s_peffects[s_npeffects];
	pe->name = Q_strdup(name);
	pe->def = CSQC_Peffect_Def(name);
	pe->type = PScript_FindParticleType(name);
	pe->refresh = pscript_refreshcount;
	vm->globals[OFS_RETURN] = -s_npeffects;
	s_npeffects++;
}

typedef struct
{
	int type;  // scripted effect type, P_INVALID for none
	const csqc_peffect_def_t *def;  // classic stand-in, NULL for none
	const char *name;
} csqc_peffect_ref_t;

// Resolves a handle of either kind; false for one that names no effect.
static qbool CSQC_Peffect_Resolve(float h, csqc_peffect_ref_t *ref)
{
	CSQC_Peffect_T *pe;
	int i;

	// Keeps the conversion to int defined, NaN included.
	if (!(h > -CSQC_MAX_PEFFECTS && h < 0x10000)) {
		return false;
	}
	i = (int)h;
	if (i > 0) {
		ref->name = CL_ServerParticleEffectName(i);
		if (!ref->name) {
			return false;
		}
		ref->type = CL_ServerParticleEffectType(i);
		ref->def = CSQC_Peffect_Def(ref->name);
		return true;
	}
	i = -i;
	if (i < 1 || i >= s_npeffects) {
		return false;
	}
	pe = &s_peffects[i];
	if (pe->refresh != pscript_refreshcount) {
		pe->type = PScript_FindParticleType(pe->name);
		pe->refresh = pscript_refreshcount;
	}
	ref->type = pe->type;
	ref->def = pe->def;
	ref->name = pe->name;
	return true;
}

// The classic stand-in for an effect the particle sets do not draw.
static void CSQC_Peffect_Classic(const csqc_peffect_def_t *def, vec3_t org, vec3_t dir, float count)
{
	if (!def) {
		return;
	}
	switch (def->kind) {
		case CSQC_PE_POINT:
			R_RunParticleEffect(org, dir, def->arg, (int)boundf(1, count * def->mult, 4096));
			break;
		case CSQC_PE_EXPLOSION:
			R_ParticleExplosion(org);
			break;
		case CSQC_PE_TAREXPLOSION:
			R_BlobExplosion(org);
			break;
		case CSQC_PE_LAVASPLASH:
			R_LavaSplash(org);
			break;
		case CSQC_PE_TELEPORT:
			R_TeleportSplash(org);
			break;
	}
}

/*
 void(entity ent, float effectnum, vector start, vector end) trailparticles = #336
 Also accepts the DP order (effectnum, entity, ...): an entity value is a small
 non-negative int (slot * edict_size), while a handle's float bits are negative or
 huge as an int. A scripted trail continues from the entity's last call; the world
 has no trail state. Classic point effects spawn at end.
*/
static void CSQC_Trailparticles(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	csqc_peffect_ref_t ref;
	float *g, ef;
	int ent;
	if (!vm) {
		return;
	}
	g = vm->globals;
	if ((unsigned int)*(int *)&g[OFS_PARM1] >= (unsigned int)vm->max_edicts * (unsigned int)vm->edict_size) {
		ef = g[OFS_PARM1];
		ent = *(int *)&g[OFS_PARM0];
	}
	else {
		ef = g[OFS_PARM0];
		ent = *(int *)&g[OFS_PARM1];
	}
	if (!CSQC_Peffect_Resolve(ef, &ref)) {
		return;
	}
	if (CSQC_Client_EntTrail(ent / vm->edict_size, &g[OFS_PARM0 + 6], &g[OFS_PARM0 + 9], ref.type)) {
		return;
	}
	if (ref.def && ref.def->kind == CSQC_PE_TRAIL) {
		R_ParticleTrail(&g[OFS_PARM0 + 6], &g[OFS_PARM0 + 9], (trail_type_t)ref.def->arg);
	}
	else {
		CSQC_Peffect_Classic(ref.def, &g[OFS_PARM0 + 9], vec3_origin, 1);
	}
}

// void(float effectnum, vector origin, optional vector dir, optional float count)
// pointparticles = #337
static void CSQC_Pointparticles(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	csqc_peffect_ref_t ref;
	float *g, *org, *dir, count;
	if (!vm) {
		return;
	}
	g = vm->globals;
	if (!CSQC_Peffect_Resolve(g[OFS_PARM0], &ref)) {
		return;
	}
	org = &g[OFS_PARM0 + 3];
	dir = (vm->argc > 2) ? &g[OFS_PARM0 + 6] : vec3_origin;
	count = (vm->argc > 3) ? g[OFS_PARM0 + 9] : 1;
	// Classic trail effects have no point form; nothing is drawn.
	if (!PScript_RunParticleEffectState(org, dir, count, ref.type, NULL)) {
		CSQC_Peffect_Classic(ref.def, org, dir, count);
	}
}

/*
 void(float effectindex, entity own, vector org_from, vector org_to, vector dir_from,
 vector dir_to, float countmultiplier, optional float flags) boxparticles = #502
 Scripted effects only: a cube between the corners, each particle moving in a
 direction between dir_from and dir_to, or with PARTICLES_DRAWASTRAIL (128) a trail
 between the corners. The other flags are ignored.
*/
static void CSQC_Boxparticles(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	csqc_peffect_ref_t ref;
	float *g;
	int flags;
	if (!vm) {
		return;
	}
	g = vm->globals;
	if (!CSQC_Peffect_Resolve(g[OFS_PARM0], &ref)) {
		return;
	}
	flags = (vm->argc > 7) ? (int)g[OFS_PARM0 + 21] : 0;
	if (flags & 128) {
		PScript_ParticleTrail(&g[OFS_PARM0 + 6], &g[OFS_PARM0 + 9], ref.type, 1, 0, NULL);
	}
	else {
		PScript_RunParticleCube(ref.type, &g[OFS_PARM0 + 6], &g[OFS_PARM0 + 9], &g[OFS_PARM0 + 12], &g[OFS_PARM0 + 15],
			g[OFS_PARM0 + 18], 0, true);
	}
}

/*
 string(float efnum, float body) particleeffectquery = #374
 body 0 gives the registered name. The config text of an effect is not kept, so
 other bodies give "".
*/
static void CSQC_Particleeffectquery(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	csqc_peffect_ref_t ref;
	if (!vm) {
		return;
	}
	CSQCVM_SetRetStr((CSQC_Peffect_Resolve(vm->globals[OFS_PARM0], &ref) && !vm->globals[OFS_PARM1]) ? (char *)ref.name : "");
}

/*
 te_* effects #219, #239, #405-427, #433 and #457. They run the engine's temp entity
 effects, scripted or with its own particles, light and sound, as if the server had
 sent them. Where the engine draws nothing without a particle set, classic particles
 stand in.
*/
static unsigned int s_te_rnd = 1;
static float CSQC_Te_Rand01(void)
{
	s_te_rnd = s_te_rnd * 1103515245u + 12345u;
	return (float)((s_te_rnd >> 8) & 0xffff) / 65535.0f;
}

static void CSQC_Te_Bbox_Effect(float *mn, float *mx, float *vel, int how, int color)
{
	int i;
	for (i = 0; i < how && i < 512; i++) {
		vec3_t p;
		p[0] = mn[0] + (mx[0] - mn[0]) * CSQC_Te_Rand01();
		p[1] = mn[1] + (mx[1] - mn[1]) * CSQC_Te_Rand01();
		p[2] = mn[2] + (mx[2] - mn[2]) * CSQC_Te_Rand01();
		R_RunParticleEffect(p, vel, color, 1);
	}
}

// A count from the module, in the range of the network message's field.
static int CSQC_Te_Count(float count, int max)
{
	return (int)boundf(1, count, max);
}

static void CSQC_Te_Colour(const float *rgb, byte *colour)
{
	int i;
	for (i = 0; i < 3; i++) {
		colour[i] = (byte)boundf(0, rgb[i] * 255, 255);
	}
}

/*
 void(vector org, string modelname, float startframe, float endframe, float framerate) effect = #404
 endframe is the number of frames to play, 0 for through the model's last frame.
*/
static void CSQC_Effect(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float *g;
	model_t *model;
	int idx;
	if (!vm) {
		return;
	}
	g = vm->globals;
	idx = CSQC_Client_ModelIndex(CSQCVM_Str(OFS_PARM1));
	model = idx ? CSQC_Client_ModelForIndex(idx) : NULL;
	if (!model) {
		return;
	}
	// Checked as floats so the conversions to int are defined.
	if (!(g[OFS_PARM0 + 6] >= 0 && g[OFS_PARM0 + 6] < model->numframes) || !(g[OFS_PARM0 + 9] >= 0)) {
		return;
	}
	CL_ModelEffect(&g[OFS_PARM0], model, (int)g[OFS_PARM0 + 6], (int)boundf(0, g[OFS_PARM0 + 9], model->numframes),
		g[OFS_PARM0 + 12]);
}

// #405 te_blood(org, dir, howmany)
static void CSQC_Te_Blood(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float *g;
	int count;
	if (!vm) {
		return;
	}
	g = vm->globals;
	count = CSQC_Te_Count(g[OFS_PARM0 + 6], 255);
	if (!CL_TE_DirectedEffect(TEDP_BLOOD, &g[OFS_PARM0], &g[OFS_PARM0 + 3], count)) {
		CL_TE_Blood(&g[OFS_PARM0], count);
	}
}
// #406 te_bloodshower(mins, maxs, explosionspeed, howmany)
static void CSQC_Te_Bloodshower(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float *g;
	vec3_t vel;
	int count;
	if (!vm) {
		return;
	}
	g = vm->globals;
	VectorSet(vel, 0, 0, -g[OFS_PARM0 + 6]);
	count = CSQC_Te_Count(g[OFS_PARM0 + 9], 65535);
	if (!CL_TE_ParticleBox(TEDP_BLOODSHOWER, &g[OFS_PARM0], &g[OFS_PARM0 + 3], vel, count, 0, false)) {
		CSQC_Te_Bbox_Effect(&g[OFS_PARM0], &g[OFS_PARM0 + 3], vel, count, 73);
	}
}
// #407 te_explosionrgb(org, colour)
static void CSQC_Te_Explosionrgb(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	byte colour[3];
	if (!vm) {
		return;
	}
	CSQC_Te_Colour(&vm->globals[OFS_PARM0 + 3], colour);
	CL_TE_ExplosionRGB(&vm->globals[OFS_PARM0], colour);
}
// #408 te_particlecube(mins, maxs, vel, howmany, colour, gravity, jitter), #409
// te_particlerain and #410 te_particlesnow(mins, maxs, vel, howmany, colour)
static void CSQC_Te_Box(int type)
{
	pr1vm_t *vm = CSQCVM_Active();
	float *g;
	int count, colour;
	if (!vm) {
		return;
	}
	g = vm->globals;
	count = CSQC_Te_Count(g[OFS_PARM0 + 9], 65535);
	colour = (int)boundf(0, g[OFS_PARM0 + 12], 255);
	if (!CL_TE_ParticleBox(type, &g[OFS_PARM0], &g[OFS_PARM0 + 3], &g[OFS_PARM0 + 6], count, colour, type == TEDP_PARTICLECUBE && g[OFS_PARM0 + 15])) {
		CSQC_Te_Bbox_Effect(&g[OFS_PARM0], &g[OFS_PARM0 + 3], &g[OFS_PARM0 + 6], count, colour);
	}
}
static void CSQC_Te_Particlecube(void)
{
	CSQC_Te_Box(TEDP_PARTICLECUBE);
}
static void CSQC_Te_Particlerain(void)
{
	CSQC_Te_Box(TEDP_PARTICLERAIN);
}
static void CSQC_Te_Particlesnow(void)
{
	CSQC_Te_Box(TEDP_PARTICLESNOW);
}
// #411 te_spark(org, vel, howmany), #457 te_flamejet(org, vel, howmany)
static void CSQC_Te_Directed(int type)
{
	pr1vm_t *vm = CSQCVM_Active();
	float *g;
	int count;
	if (!vm) {
		return;
	}
	g = vm->globals;
	count = CSQC_Te_Count(g[OFS_PARM0 + 6], 255);
	if (!CL_TE_DirectedEffect(type, &g[OFS_PARM0], &g[OFS_PARM0 + 3], count)) {
		R_RunParticleEffect(&g[OFS_PARM0], &g[OFS_PARM0 + 3], 0, count);
	}
}
static void CSQC_Te_Spark(void)
{
	CSQC_Te_Directed(TEDP_SPARK);
}
static void CSQC_Te_Flamejet(void)
{
	CSQC_Te_Directed(TEDP_FLAMEJET);
}
// #418 te_gunshot(org, optional count)
static void CSQC_Te_Gunshot(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (vm) {
		CL_TE_Gunshot(&vm->globals[OFS_PARM0], (vm->argc > 1) ? CSQC_Te_Count(vm->globals[OFS_PARM0 + 3], 255) : 1);
	}
}
// #239 te_bloodqw(org, optional count)
static void CSQC_Te_Bloodqw(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (vm) {
		CL_TE_Blood(&vm->globals[OFS_PARM0], (vm->argc > 1) ? CSQC_Te_Count(vm->globals[OFS_PARM0 + 3], 255) : 1);
	}
}
static void CSQC_Te_Effect(int type)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (vm) {
		CL_TempEntityEffect(type, &vm->globals[OFS_PARM0]);
	}
}
// Spike impacts play no ricochet sound.
static void CSQC_Te_Spike_Impact(int type)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (vm) {
		CL_TE_SpikeImpact(type, &vm->globals[OFS_PARM0]);
	}
}
static void CSQC_Te_Spike(void)
{
	CSQC_Te_Spike_Impact(TE_SPIKE);
}
static void CSQC_Te_Superspike(void)
{
	CSQC_Te_Spike_Impact(TE_SUPERSPIKE);
}
static void CSQC_Te_Spikequad(void)
{
	CSQC_Te_Spike_Impact(TEDP_SPIKEQUAD);
}
static void CSQC_Te_Superspikequad(void)
{
	CSQC_Te_Spike_Impact(TEDP_SUPERSPIKEQUAD);
}
static void CSQC_Te_Wizspike(void)
{
	CSQC_Te_Effect(TE_WIZSPIKE);
}
static void CSQC_Te_Knightspike(void)
{
	CSQC_Te_Effect(TE_KNIGHTSPIKE);
}
static void CSQC_Te_Explosion(void)
{
	CSQC_Te_Effect(TE_EXPLOSION);
}
static void CSQC_Te_Tarexplosion(void)
{
	CSQC_Te_Effect(TE_TAREXPLOSION);
}
static void CSQC_Te_Lavasplash(void)
{
	CSQC_Te_Effect(TE_LAVASPLASH);
}
static void CSQC_Te_Teleport(void)
{
	CSQC_Te_Effect(TE_TELEPORT);
}
static void CSQC_Te_Gunshotquad(void)
{
	CSQC_Te_Effect(TEDP_GUNSHOTQUAD);
}
static void CSQC_Te_Explosionquad(void)
{
	CSQC_Te_Effect(TEDP_EXPLOSIONQUAD);
}
static void CSQC_Te_Lightningblood(void)
{
	CSQC_Te_Effect(TE_LIGHTNINGBLOOD);
}
static void CSQC_Te_Plasmaburn(void)
{
	CSQC_Te_Effect(TEDP_PLASMABURN);
}
static void CSQC_Te_Smallflash(void)
{
	CSQC_Te_Effect(TEDP_SMALLFLASH);
}
// #417 te_customflash(org, radius, lifetime, colour)
static void CSQC_Te_Customflash(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float *g;
	byte colour[3];
	if (!vm) {
		return;
	}
	g = vm->globals;
	CSQC_Te_Colour(&g[OFS_PARM0 + 9], colour);
	CL_TE_CustomFlash(&g[OFS_PARM0], g[OFS_PARM0 + 3], g[OFS_PARM0 + 6], colour);
}
// #427 te_explosion2(org, colorstart, colorlength)
static void CSQC_Te_Explosion2(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float *g;
	if (!vm) {
		return;
	}
	g = vm->globals;
	CL_TE_Explosion2(&g[OFS_PARM0], (int)g[OFS_PARM0 + 3], (int)g[OFS_PARM0 + 6]);
}

/*
 Beams #428-431 (te_lightning1/2/3, te_beam). A missing beam model (e.g.
 progs/beam.mdl) is checked up front and only warns: CL_CreateBeam itself would
 load it with crash=true and drop the connection.
*/
static custom_model_id_t CSQC_BeamModelId(int type)
{
	switch (type) {
		case 1: return custom_model_bolt;
		case 2: return custom_model_bolt2;
		case 3: return custom_model_bolt3;
		case 4:
		default: return custom_model_beam;
	}
}

static const char *CSQC_BeamModelName(int type)
{
	switch (type) {
		case 1: return "progs/bolt.mdl";
		case 2: return "progs/bolt2.mdl";
		case 3: return "progs/bolt3.mdl";
		case 4:
		default: return "progs/beam.mdl";
	}
}

static void CSQC_Te_Beam_Type(int type)
{
	pr1vm_t *vm = CSQCVM_Active();
	float *g;
	int entnum;
	if (!vm) {
		return;
	}
	g = vm->globals;
	if (!Mod_CustomModel(CSQC_BeamModelId(type), false)) {
		Con_Printf("CSQC: te_beam type %d: %s not found - effect skipped\n", type, CSQC_BeamModelName(type));
		return;
	}
	entnum = CSQC_Ent_Of(vm, OFS_PARM0);
	// Shift the CSQC beam owner past MAX_EDICTS so it never replaces a server beam
	// with the same numeric index (0 = world stays 0).
	entnum += (entnum ? MAX_EDICTS : 0);
	CL_CreateBeam(type, entnum, &g[OFS_PARM0 + 3], &g[OFS_PARM0 + 6]);
}
static void CSQC_Te_Lightning1(void) { CSQC_Te_Beam_Type(1); }
static void CSQC_Te_Lightning2(void) { CSQC_Te_Beam_Type(2); }
static void CSQC_Te_Lightning3(void) { CSQC_Te_Beam_Type(3); }
static void CSQC_Te_Beam(void) { CSQC_Te_Beam_Type(4); }

/*
 Scoreboard order for negative getplayerkeyvalue indices (-1 = top): every named
 slot, spectators last, by frags descending. Re-sorted once per frame. Sbar's own sort is not reused: it rewrites spectator
 frags and may sort by team.
*/
static int csqc_fragsort[MAX_CLIENTS];
static int csqc_fragsort_count;
static int csqc_fragsort_frame = -1;

static int CSQC_Fragsort_Weight(int i)
{
	return cl.players[i].spectator ? -999 : cl.players[i].frags;
}

static void CSQC_Sort_Frags(void)
{
	int i, j, n = 0;

	if (csqc_fragsort_frame == cls.framecount) {
		return;
	}
	csqc_fragsort_frame = cls.framecount;
	for (i = 0; i < MAX_CLIENTS; i++) {
		if (!cl.players[i].name[0]) {
			continue;
		}
		// insertion keeps equal weights in slot order
		for (j = n; j > 0 && CSQC_Fragsort_Weight(csqc_fragsort[j - 1]) < CSQC_Fragsort_Weight(i); j--) {
			csqc_fragsort[j] = csqc_fragsort[j - 1];
		}
		csqc_fragsort[j] = i;
		n++;
	}
	csqc_fragsort_count = n;
}

/*
 Player key lookup shared by #348 getplayerkeyvalue and #80 infokey: viewentity
 answers for empty slots too, ping/pl request a scoreboard ping update, anything
 unknown falls back to userinfo. Returns NULL for no value.
*/
void Request_Pings(void);

static const char *CSQC_Playerkey_Value(int pnum, const char *key, char *buf, size_t bufsize)
{
	player_info_t *pi;

	if (pnum < 0 || pnum >= MAX_CLIENTS || !key) {
		return NULL;
	}
	if (!strcmp(key, "viewentity")) {
		snprintf(buf, bufsize, "%d", pnum + 1);
		return buf;
	}
	pi = &cl.players[pnum];
	if (!pi->name[0]) {
		return NULL;
	}

	if (!strcmp(key, "ping")) {
		Request_Pings();
		snprintf(buf, bufsize, "%d", pi->ping);
	}
	else if (!strcmp(key, "frags")) {
		snprintf(buf, bufsize, "%d", pi->frags);
	}
	else if (!strcmp(key, "userid")) {
		snprintf(buf, bufsize, "%d", pi->userid);
	}
	else if (!strcmp(key, "pl")) {
		Request_Pings();
		snprintf(buf, bufsize, "%d", (int)pi->pl);
	}
	else if (!strcmp(key, "activetime")) {
		snprintf(buf, bufsize, "%f", (cls.demoplayback ? cls.demotime : cls.realtime) - pi->entertime);
	}
	else if (!strcmp(key, "topcolor_rgb") || !strcmp(key, "bottomcolor_rgb")) {
		// Team-forced colours; >= 16 is a packed 0xRRGGBB value.
		int col = (key[0] == 't') ? (int)pi->topcolor : (int)pi->bottomcolor;
		if (col < 16) {
			int pal = Sbar_ColorForMap(col);
			snprintf(buf, bufsize, "'%g %g %g'",
				host_basepal[pal * 3 + 0] / 255.0,
				host_basepal[pal * 3 + 1] / 255.0,
				host_basepal[pal * 3 + 2] / 255.0);
		}
		else {
			snprintf(buf, bufsize, "'%g %g %g'", ((col >> 16) & 0xff) / 255.0, ((col >> 8) & 0xff) / 255.0, (col & 0xff) / 255.0);
		}
	}
	else if (!strcmp(key, "ignored")) {
		snprintf(buf, bufsize, "%d", (int)pi->ignored);
	}
	else if (!strcmp(key, "vignored")) {
		snprintf(buf, bufsize, "%d", (int)pi->vignored);
	}
	else if (!strcmp(key, "voipspeaking")) {
		snprintf(buf, bufsize, "%d", S_Voip_Speaking(pnum));
	}
	else if (!strcmp(key, "voiploudness")) {
		// Only the local capture level exists; no per-client loudness.
		if (pnum != cl.playernum) {
			return NULL;
		}
		snprintf(buf, bufsize, "%d", S_Voip_Loudness());
	}
	else if (!strcmp(key, "spectator")) {
		snprintf(buf, bufsize, "%d", (int)pi->spectator);
	}
	else if (!strcmp(key, "name")) {
		return pi->name;
	}
	else {
		return Info_ValueForKey(pi->userinfo, (char *)key);
	}
	return buf;
}

// The player key of the (playernum, keyname) arguments; a negative playernum is a
// scoreboard position (-1 = top). NULL for an empty slot, out of range or no value.
static const char *CSQC_Playerkey_Arg(pr1vm_t *vm, char *buf, size_t bufsize)
{
	int pnum = (int)vm->globals[OFS_PARM0];
	char *key = CSQCVM_Str(OFS_PARM1);

	if (pnum < 0) {
		CSQC_Sort_Frags();
		if (pnum >= -csqc_fragsort_count) {
			pnum = csqc_fragsort[-(pnum + 1)];
		}
	}
	return (key && key[0]) ? CSQC_Playerkey_Value(pnum, key, buf, bufsize) : NULL;
}

// string(float playernum, string keyname) getplayerkeyvalue = #348, "" for no value
static void CSQC_Getplayerkeyvalue(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char buf[64];
	const char *v;

	if (!vm) {
		return;
	}
	v = CSQC_Playerkey_Arg(vm, buf, sizeof(buf));
	CSQCVM_SetRetStr((char *)(v ? v : ""));
}

// float(float playernum, string keyname, optional float assumevalue) getplayerkeyfloat = #0
// The key as a number; assumevalue, else 0, when it has no value.
static void CSQC_Getplayerkeyfloat(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char buf[64];
	const char *v;

	if (!vm) {
		return;
	}
	v = CSQC_Playerkey_Arg(vm, buf, sizeof(buf));
	if (v && v[0]) {
		vm->globals[OFS_RETURN] = (float)strtod(v, NULL);
	}
	else {
		vm->globals[OFS_RETURN] = (vm->argc > 2) ? vm->globals[OFS_PARM0 + 6] : 0;
	}
}

/*
 void(float usecursor, optional string cursorimage, optional vector hotspot,
      optional float scale) setcursormode = #343
 While usecursor=1 and the module is active in-game, the mouse stays grabbed and
 SCR_DrawCursor draws the module's cursor (image/hotspot/scale); 0 returns the
 mouse to the engine. Scale is read from hotspot.z and the separate scale
 argument is ignored; scale <= 0 -> 1. Without a cursorimage argument only
 usecursor changes.
*/
static void CSQC_Setcursormode(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float *g;
	if (!vm) {
		return;
	}
	g = vm->globals;
	CSQC_Client_SetCursorMode(g[OFS_PARM0] != 0,
		vm->argc > 1 ? CSQC_Client_GetString(vm, *(int *)&g[OFS_PARM1]) : NULL,
		vm->argc > 2 ? g[OFS_PARM2] : 0,
		vm->argc > 2 ? g[OFS_PARM2 + 1] : 0,
		vm->argc > 2 ? g[OFS_PARM2 + 2] : 0);
}

/*
 vector() getmousepos = #344
 Cursor position while the module has setcursormode(1), otherwise the mouse motion
 accumulated since the previous call. The accumulator is reset either way.
*/
static void CSQC_Getmousepos(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float x = 0, y = 0;
	if (!vm) {
		return;
	}
	CSQC_Client_TakeMouseDelta(&x, &y);
	if (CSQC_Client_CursorRequested()) {
		CSQC_Client_GetCursorPos(&x, &y);
	}
	vm->globals[OFS_RETURN] = x;
	vm->globals[OFS_RETURN + 1] = y;
	vm->globals[OFS_RETURN + 2] = 0;
}

/*
 float(float x, float y) pow = #97
*/
static void CSQC_Pow(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN] = pow(vm->globals[OFS_PARM0], vm->globals[OFS_PARM1]);
}

/*
 vector() randomvec = #91
*/
static void CSQC_Randomvec(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float *r;
	if (!vm) {
		return;
	}
	r = &vm->globals[OFS_RETURN];
	do {
		r[0] = (rand() & 0x7fff) * (2.0 / 0x7fff) - 1.0;
		r[1] = (rand() & 0x7fff) * (2.0 / 0x7fff) - 1.0;
		r[2] = (rand() & 0x7fff) * (2.0 / 0x7fff) - 1.0;
	} while (DotProduct(r, r) >= 1);
}

/*
float(float x) asin = #471; float(float x) acos = #472
float(float x) atan = #473; float(float a, float b) atan2 = #474; float(float x) tan = #475
*/
static void CSQC_Asin(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN] = asin(vm->globals[OFS_PARM0]);
}
static void CSQC_Acos(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN] = acos(vm->globals[OFS_PARM0]);
}
static void CSQC_Atan(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN] = atan(vm->globals[OFS_PARM0]);
}
static void CSQC_Atan2(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN] = atan2(vm->globals[OFS_PARM0], vm->globals[OFS_PARM1]);
}
static void CSQC_Tan(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN] = tan(vm->globals[OFS_PARM0]);
}

/*
 float(float x, optional float base) log = #532
 Natural log; with a base argument, log(x)/log(base).
*/
static void CSQC_Log(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	double r;
	if (!vm) {
		return;
	}
	r = log(vm->globals[OFS_PARM0]);
	if (vm->argc > 1) {
		r /= log(vm->globals[OFS_PARM1]);
	}
	vm->globals[OFS_RETURN] = (float)r;
}

/*
float(float v) anglemod = #102 - in [0,360).
*/
// Non-iterative, so +inf or huge values cannot hang the client: NaN/inf -> 0.
static float CSQC_Anglemod_Value(float v)
{
	if (v != v) {
		return 0;
	}
	if (v >= 1e30f || v <= -1e30f) {
		return 0;
	}
	v = (float)fmod(v, 360.0);
	if (v < 0) {
		v += 360;
	}
	return v;
}

static void CSQC_Anglemod(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN] = CSQC_Anglemod_Value(vm->globals[OFS_PARM0]);
}

/*
float(float a, float n) mod = #245 - a - n*(int)(a/n); division by 0 -> warning + 0.
*/
static void CSQC_Mod(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float a, n;
	if (!vm) {
		return;
	}
	a = vm->globals[OFS_PARM0];
	n = vm->globals[OFS_PARM1];
	if (n == 0) {
		Con_Printf("CSQC mod: mod by zero\n");
		vm->globals[OFS_RETURN] = 0;
	}
	else {
		vm->globals[OFS_RETURN] = a - n * (float)(int)(a / n);
	}
}

/*
 float(float number, float quantity) bitshift = #218
 quantity<0 -> shift right by -quantity, otherwise left.
*/
static void CSQC_Bitshift(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int mask, shift;
	if (!vm) {
		return;
	}
	mask = (int)vm->globals[OFS_PARM0];
	shift = (int)vm->globals[OFS_PARM1];
	if (shift < 0) {
		mask >>= -shift;
	}
	else {
		mask <<= shift;
	}
	vm->globals[OFS_RETURN] = mask;
}

/*
 float(float insensitive, string str, ...) crc16 = #494
 CRC16-CCITT (poly 0x1021, init 0xffff, xor 0x0000) of the concatenated string
 args; insensitive lowercases letters first.
*/
static void CSQC_Crc16(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	unsigned short crc;
	char buf[4096];
	int i, insens, len = 0;
	const char *s;

	if (!vm) {
		return;
	}
	insens = (int)vm->globals[OFS_PARM0];
	buf[0] = 0;
	for (i = 1; i < vm->argc; i++) {
		s = CSQC_Client_GetString(vm, *(int *)&vm->globals[OFS_PARM0 + i * 3]);
		if (s) {
			len += snprintf(buf + len, sizeof(buf) - len, "%s", s);
		}
		if (len >= (int)sizeof(buf) - 1) {
			break;
		}
	}
	CRC_Init(&crc);
	// hash exactly the bytes that fit: on truncation snprintf reports the longer
	// would-be length.
	len = (int)strlen(buf);
	for (i = 0; i < len; i++) {
		CRC_ProcessByte(&crc, insens ? tolower((int)(unsigned char)buf[i]) : (unsigned char)buf[i]);
	}
	vm->globals[OFS_RETURN] = CRC_Value(crc);
}

/*
 float(optional float timer) gettimef = #519 - time in seconds.
 timer 0/none/unknown - cls.realtime (frame time; scales with cl_demospeed),
 1 - wall-clock with ms precision, 5 - sim time (cl.time).
*/
// Local prototype: sys.h conflicts with quakedef.h (dllfunction_t).
double Sys_DoubleTime(void);
static void CSQC_Gettimef(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int timer;
	if (!vm) {
		return;
	}
	timer = (vm->argc > 0) ? (int)vm->globals[OFS_PARM0] : 0;
	switch (timer) {
		case 1:
			vm->globals[OFS_RETURN] = (float)((double)(long long)(Sys_DoubleTime() * 1000.0) / 1000.0);
			break;
		case 5:
			vm->globals[OFS_RETURN] = (float)cl.time;
			break;
		default:
			vm->globals[OFS_RETURN] = (float)cls.realtime;
			break;
	}
}

/*
 int/hex conversions (#259-262). int-typed params/returns are raw int bits in the
 slot, not floats.
*/

/*
string(int input) itos = #260 - "%d".
*/
static void CSQC_Itos(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char buf[32];
	if (!vm) {
		return;
	}
	snprintf(buf, sizeof(buf), "%d", *(int *)&vm->globals[OFS_PARM0]);
	CSQCVM_SetRetStr(buf);
}

/*
int(string input) stoi = #259 - atoi (returns int bits).
*/
static void CSQC_Stoi(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *s = CSQCVM_Str(OFS_PARM0);
	if (!vm) {
		return;
	}
	*(int *)&vm->globals[OFS_RETURN] = atoi(s ? s : "");
}

/*
string(int input) htos = #262 - "%08x" (always 8 chars, no prefix).
*/
static void CSQC_Htos(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char buf[32];
	if (!vm) {
		return;
	}
	snprintf(buf, sizeof(buf), "%08x", *(unsigned int *)&vm->globals[OFS_PARM0]);
	CSQCVM_SetRetStr(buf);
}

/*
int(string input) stoh = #261 - strtoul base 16 (returns int bits).
*/
static void CSQC_Stoh(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *s = CSQCVM_Str(OFS_PARM0);
	if (!vm) {
		return;
	}
	*(int *)&vm->globals[OFS_RETURN] = (int)strtoul(s ? s : "", NULL, 16);
}

/*
 string(string cvarname) cvar_defstring = #482
 Creates the cvar if missing; returns its default value ("" if none or read-guarded).
 A cvar made by registercvar(name, value) reports value as its default.
*/
static void CSQC_Cvar_Defstring(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *name = CSQCVM_Str(OFS_PARM0);
	cvar_t *v;
	if (!vm) {
		return;
	}
	if (CSQC_CvarReadDenied(name)) {
		CSQCVM_SetRetStr("");
		return;
	}
	v = (name && name[0]) ? Cvar_Find(name) : NULL;
	if (!v && name && name[0]) {
		v = Cvar_Create(name, "", 0);
	}
	CSQCVM_SetRetStr((v && v->defaultvalue) ? v->defaultvalue : "");
}

/*
 float(string cvarname) cvar_type = #495
 Flags: EXISTS=1 SAVED=2 PRIVATE=4 ENGINE=8 HASDESCRIPTION=16 READONLY=32.
 SAVED = CVAR_ARCHIVE|CVAR_USER_ARCHIVE; PRIVATE = CVAR_NOTFROMSERVER or
 read-guarded (CVAR_NOUNSAFEEXPAND); ENGINE = not user/mod created; READONLY =
 CVAR_ROM. HASDESCRIPTION is never set (cvars have no description).
*/
static void CSQC_Cvar_Type(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *name = CSQCVM_Str(OFS_PARM0);
	cvar_t *v;
	int ret = 0;
	if (!vm) {
		return;
	}
	v = (name && name[0]) ? Cvar_Find(name) : NULL;
	if (v) {
		ret |= 1; // EXISTS
		if (v->flags & (CVAR_ARCHIVE | CVAR_USER_ARCHIVE)) {
			ret |= 2; // SAVED
		}
		if (v->flags & CVAR_ROM) {
			ret |= 32; // READONLY
		}
		if (!(v->flags & (CVAR_USER_CREATED | CVAR_MOD_CREATED))) {
			ret |= 8; // ENGINE
		}
	}
	if ((v && (v->flags & CVAR_NOTFROMSERVER)) || CSQC_CvarReadDenied(name)) {
		ret |= 4; // PRIVATE
	}
	vm->globals[OFS_RETURN] = ret;
}

/*
 string(string cvarname) cvar_description = #518
 Always "": cvar_t has no description.
*/
static void CSQC_Cvar_Description(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	CSQCVM_SetRetStr("");
}

/*
 Simple string builtins, with byte (ASCII) semantics; no UTF-8 handling.
*/

/*
 float(string str, optional float index) str2chr = #222 - char code; index<0 - from
 the end; outside [0,len) -> 0.
*/
static void CSQC_Str2chr(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *s = CSQCVM_Str(OFS_PARM0);
	int len, idx;
	if (!vm) {
		return;
	}
	s = s ? s : "";
	len = strlen(s);
	idx = (vm->argc > 1) ? (int)vm->globals[OFS_PARM1] : 0;
	if (idx < 0) {
		idx = len + idx;
	}
	if (idx < 0 || idx >= len) {
		vm->globals[OFS_RETURN] = 0;
	}
	else {
		vm->globals[OFS_RETURN] = (float)(unsigned char)s[idx];
	}
}

/*
string(float chr, ...) chr2str = #223 - string from char codes (each argument).
*/
static void CSQC_Chr2str(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char buf[128];
	int i, n = 0;
	if (!vm) {
		return;
	}
	for (i = 0; i < vm->argc && i < 64 && n < (int)sizeof(buf) - 1; i++) {
		buf[n++] = (char)(int)vm->globals[OFS_PARM0 + i * 3];
	}
	buf[n] = 0;
	CSQCVM_SetRetStr(buf);
}

/*
 string(float pad, string str1, ...) strpad = #225 - pads the concatenated strings to
 width |pad|: pad>0 - right, pad<0 - left.
*/
static void CSQC_Strpad(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char buf[4096], *d;
	int pad, len = 0, i;
	const char *s;

	if (!vm) {
		return;
	}
	pad = (int)vm->globals[OFS_PARM0];
	buf[0] = 0;
	d = buf;
	for (i = 1; i < vm->argc; i++) {
		s = CSQC_Client_GetString(vm, *(int *)&vm->globals[OFS_PARM0 + i * 3]);
		if (!s) {
			continue;
		}
		len = strlen(s);
		if (d - buf + len >= (int)sizeof(buf) - 1) {
			len = (int)sizeof(buf) - 1 - (d - buf);
		}
		memcpy(d, s, len);
		d += len;
		*d = 0;
	}
	if (pad < 0) {
		pad = -pad - (int)(d - buf);
		if (pad > (int)sizeof(buf) - 1 - (d - buf)) {
			pad = (int)sizeof(buf) - 1 - (d - buf);
		}
		if (pad > 0) {
			memmove(buf + pad, buf, (size_t)(d - buf) + 1);
			memset(buf, ' ', pad);
		}
	}
	else {
		pad -= (int)(d - buf);
		if (pad < 0) {
			pad = 0;
		}
		if (d - buf + pad >= (int)sizeof(buf) - 1) {
			pad = (int)sizeof(buf) - 1 - (d - buf);
		}
		memset(d, ' ', pad);
		d[pad] = 0;
	}
	CSQCVM_SetRetStr(buf);
}

/*
 string(infostring old, string key, string value) infoadd = #226
 string(infostring info, string key) infoget = #227
 QW infostrings (\key\value).
*/
// No prototype in common.h.
void Info_SetValueForStarKey(char *s, char *key, char *value, int maxsize);
static void CSQC_Infoadd(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *info, *key, *val;
	char buf[1024];
	if (!vm) {
		return;
	}
	info = CSQCVM_Str(OFS_PARM0);
	key = CSQCVM_Str(OFS_PARM1);
	val = CSQCVM_VarString(2);
	strlcpy(buf, info ? info : "", sizeof(buf));
	Info_SetValueForStarKey(buf, key ? key : "", val ? val : "", sizeof(buf));
	CSQCVM_SetRetStr(buf);
}
static void CSQC_Infoget(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *info = CSQCVM_Str(OFS_PARM0);
	char *key = CSQCVM_Str(OFS_PARM1);
	if (!vm) {
		return;
	}
	CSQCVM_SetRetStr(Info_ValueForKey(info ? info : "", key ? key : ""));
}

/*
 float(string s1, string s2, optional float len, optional float s1ofs, optional float s2ofs)
 strcmp/strncmp = #228
*/
static void CSQC_Strncmp(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	const char *a, *b;
	int len, aofs, bofs;
	int alen, blen;
	if (!vm) {
		return;
	}
	a = CSQCVM_Str(OFS_PARM0) ? CSQCVM_Str(OFS_PARM0) : "";
	b = CSQCVM_Str(OFS_PARM1) ? CSQCVM_Str(OFS_PARM1) : "";
	if (vm->argc <= 2) {
		vm->globals[OFS_RETURN] = strcmp(a, b);
		return;
	}
	len = (int)vm->globals[OFS_PARM2];
	aofs = (vm->argc > 3) ? (int)vm->globals[OFS_PARM3] : 0;
	bofs = (vm->argc > 4) ? (int)vm->globals[OFS_PARM4] : 0;
	alen = strlen(a);
	blen = strlen(b);
	if (aofs < 0 || (aofs && aofs > alen)) {
		aofs = alen;
	}
	if (bofs < 0 || (bofs && bofs > blen)) {
		bofs = blen;
	}
	vm->globals[OFS_RETURN] = strncmp(a + aofs, b + bofs, len);
}

/*
float(string s1, string s2) strcasecmp = #229
 float(string s1, string s2, float len, optional float s1ofs, optional float s2ofs)
 strncasecmp = #230
*/
static void CSQC_Strncasecmp(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	const char *a, *b;
	int len, aofs, bofs;
	int alen, blen;
	if (!vm) {
		return;
	}
	a = CSQCVM_Str(OFS_PARM0) ? CSQCVM_Str(OFS_PARM0) : "";
	b = CSQCVM_Str(OFS_PARM1) ? CSQCVM_Str(OFS_PARM1) : "";
	if (vm->argc <= 2) {
		vm->globals[OFS_RETURN] = strcasecmp(a, b);
		return;
	}
	len = (int)vm->globals[OFS_PARM2];
	aofs = (vm->argc > 3) ? (int)vm->globals[OFS_PARM3] : 0;
	bofs = (vm->argc > 4) ? (int)vm->globals[OFS_PARM4] : 0;
	alen = strlen(a);
	blen = strlen(b);
	if (aofs < 0 || (aofs && aofs > alen)) {
		aofs = alen;
	}
	if (bofs < 0 || (bofs && bofs > blen)) {
		bofs = blen;
	}
	vm->globals[OFS_RETURN] = strncasecmp(a + aofs, b + bofs, len);
}

/*
string(string s) strtolower = #480 / strtoupper = #481 - ASCII only.
*/
static void CSQC_Strtolower(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	const char *s = CSQCVM_Str(OFS_PARM0);
	char buf[8192];
	int i, n;
	if (!vm) {
		return;
	}
	n = strlen(s ? s : "");
	if (n >= (int)sizeof(buf)) {
		n = (int)sizeof(buf) - 1;
	}
	for (i = 0; i < n; i++) {
		buf[i] = tolower((int)(unsigned char)s[i]);
	}
	buf[n] = 0;
	CSQCVM_SetRetStr(buf);
}
static void CSQC_Strtoupper(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	const char *s = CSQCVM_Str(OFS_PARM0);
	char buf[8192];
	int i, n;
	if (!vm) {
		return;
	}
	n = strlen(s ? s : "");
	if (n >= (int)sizeof(buf)) {
		n = (int)sizeof(buf) - 1;
	}
	for (i = 0; i < n; i++) {
		buf[i] = toupper((int)(unsigned char)s[i]);
	}
	buf[n] = 0;
	CSQCVM_SetRetStr(buf);
}

/*
 Colour markup stripping for #476 strlennocol / #477 strdecolorize.
 Strips `&cRGB` (valid 3-hex) / `&r` and the `^` colour/state codes: q3 colours
 `^0-9`, `^xRGB`, `^&XX` (extended FG/BG), states `^b/^d/^m/^a/^h/^s/^r`, escape
 `^^`; an unknown or dangling `^` stays literal. Not handled: links `^[..^]`,
 charset `u8:`/`k8:`, `^Uxxxx`/`^{xxxx}`. Returns the result length in bytes;
 out==NULL is allowed (count only).
*/
static int CSQCVM_IsExtCode(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || c == '-';
}

static int CSQCVM_StripColor(const char *in, char *out, size_t outsize)
{
	size_t n = 0;

	if (!in) {
		in = "";
	}
	for (; *in; in++) {
		if (in[0] == '&' && in[1] == 'c' && HexToInt(in[2]) >= 0 && HexToInt(in[3]) >= 0 && HexToInt(in[4]) >= 0) {
			in += 4;
			continue;
		}
		if (in[0] == '&' && in[1] == 'r') {
			in += 1;
			continue;
		}
		if (in[0] == '^') {
			char c1 = in[1];

			if (c1 >= '0' && c1 <= '9')   // ^0..^9 q3 colour
			{
				in += 1;
				continue;
			}
			if (c1 == 'x')      // ^xRGB valid -> strip 5; invalid -> strip "^x"
			{
				if (HexToInt(in[2]) >= 0 && HexToInt(in[3]) >= 0 && HexToInt(in[4]) >= 0) {
					in += 4;
				}
				else {
					in += 1;
				}
				continue;
			}
			if (c1 == '&')      // ^&XX extended FG/BG
			{
				if (CSQCVM_IsExtCode(in[2]) && CSQCVM_IsExtCode(in[3])) {
					in += 3;
					continue;
				}
				// invalid: '^' stays a literal, '&' is handled on the next iteration
			}
			else if (c1 == 'b' || c1 == 'd' || c1 == 'm' || c1 == 'a' || c1 == 'h' || c1 == 's' || c1 == 'r') {
				in += 1;
				continue;
			}
			else if (c1 == '^')     // ^^ -> ^
			{
				if (out && outsize && n + 1 < outsize) {
					out[n] = '^';
				}
				n++;
				in += 1;
				continue;
			}
			// unknown / end / out-of-scope: '^' literal, next char processed normally
		}
		if (out && outsize && n + 1 < outsize) {
			out[n] = *in;
		}
		n++;
	}
	if (out && outsize) {
		out[(n < outsize) ? n : outsize - 1] = 0;
	}
	return (int)n;
}

/*
 float(string s) strlennocol = #476 - length without colour codes.
*/
static void CSQC_Strlennocol(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN] = (float)CSQCVM_StripColor(CSQCVM_Str(OFS_PARM0), NULL, 0);
}

/*
 string(string s) strdecolorize = #477
*/
static void CSQC_Strdecolorize(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char buf[8192];

	if (!vm) {
		return;
	}
	CSQCVM_StripColor(CSQCVM_Str(OFS_PARM0), buf, sizeof(buf));
	CSQCVM_SetRetStr(buf);
}

/*
 string(string input, string token) instr = #206
 token is the concatenation of all args from param 1. Returns input from the first
 match onward, or "" if not found.
*/
static void CSQC_Instr(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	const char *s1, *s2, *sub;

	if (!vm) {
		return;
	}
	s1 = CSQCVM_Str(OFS_PARM0);
	s2 = CSQCVM_VarString(1);
	if (!s1 || !s2) {
		CSQCVM_SetRetStr("");
		return;
	}
	sub = strstr(s1, s2);
	CSQCVM_SetRetStr(sub ? (char *)sub : "");
}

/*
 string(string search, string replace, string subject) strreplace = #484
 string(string search, string replace, string subject) strireplace = #485
 (4096-byte buffer, non-recursive replacement).
*/
static void CSQC_Strreplace(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	const char *search, *replace, *sub;
	char buf[4096], *result = buf;
	int searchlen, replacelen;
	if (!vm) {
		return;
	}
	search = CSQCVM_Str(OFS_PARM0) ? CSQCVM_Str(OFS_PARM0) : "";
	replace = CSQCVM_Str(OFS_PARM1) ? CSQCVM_Str(OFS_PARM1) : "";
	sub = CSQCVM_Str(OFS_PARM2) ? CSQCVM_Str(OFS_PARM2) : "";
	searchlen = strlen(search);
	replacelen = strlen(replace);
	if (searchlen) {
		while (*sub && result < buf + sizeof(buf) - replacelen - 2) {
			if (!strncmp(sub, search, searchlen)) {
				sub += searchlen;
				memcpy(result, replace, replacelen);
				result += replacelen;
			}
			else {
				*result++ = *sub++;
			}
		}
		*result = 0;
	}
	else {
		strlcpy(buf, sub, sizeof(buf));
	}
	CSQCVM_SetRetStr(buf);
}
static void CSQC_Strireplace(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	const char *search, *replace, *sub;
	char buf[4096], *result = buf;
	int searchlen, replacelen;
	if (!vm) {
		return;
	}
	search = CSQCVM_Str(OFS_PARM0) ? CSQCVM_Str(OFS_PARM0) : "";
	replace = CSQCVM_Str(OFS_PARM1) ? CSQCVM_Str(OFS_PARM1) : "";
	sub = CSQCVM_Str(OFS_PARM2) ? CSQCVM_Str(OFS_PARM2) : "";
	searchlen = strlen(search);
	replacelen = strlen(replace);
	if (searchlen) {
		while (*sub && result < buf + sizeof(buf) - replacelen - 2) {
			if (!strncasecmp(sub, search, searchlen)) {
				sub += searchlen;
				memcpy(result, replace, replacelen);
				result += replacelen;
			}
			else {
				*result++ = *sub++;
			}
		}
		*result = 0;
	}
	else {
		strlcpy(buf, sub, sizeof(buf));
	}
	CSQCVM_SetRetStr(buf);
}

/*
 void(string err, ...) error = #10
 developer!=0 - print and continue; developer==0 - fatal (host_error). No stack or
 self/edict dump.
*/
static void CSQC_Error(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *s = CSQCVM_VarString(0);
	if (!vm) {
		return;
	}
	Con_Printf("CSQC error: %s\n", s ? s : "");
	if (!developer.value && vm->host_error) {
		vm->host_error(vm, s ? s : "error");
	}
}

/*
 void(string err, ...) objerror = #11
 developer!=0 - print and continue; developer==0 - print and abort the module
 (CSQC_Client_Abort). No self/edict dump.
*/
static void CSQC_Objerror(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *s = CSQCVM_VarString(0);
	if (!vm) {
		return;
	}
	Con_Printf("CSQC objerror: %s\n", s ? s : "");
	if (!developer.value) {
		CSQC_Client_Abort(s ? s : "objerror");
	}
}

/*
 void(string str) localcmd = #46
 Executes the string with the trust level of server-sent commands (svc_stufftext):
 via cbuf_svc and the cl_remote_capabilities allowlist, not the unrestricted
 cbuf_main. A '\n' is appended because Cbuf_ExecuteEx never runs a cbuf_svc line
 without a trailing newline.
*/
static void CSQC_Localcmd(void)
{
	char *s = CSQCVM_VarString(0);
	// Reject $: Cmd_ExpandString runs before the allowlist and would substitute
	// e.g. $rcon_password into a say/cmd line, bypassing the cvar read-guard.
	if (s && strchr(s, '$')) {
		Con_DPrintf("CSQC: localcmd with $ expansion rejected\n");
		return;
	}
	if (s && s[0]) {
		Cbuf_AddTextEx(&cbuf_svc, va("%s\n", s));
	}
}

/*
 void(string cvarname, string value) cvar_set = #72
 A missing cvar is created. A CVAR_NOTFROMSERVER cvar is silently not written:
 the module is an untrusted source.
*/
static void CSQC_Cvar_Set(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *name, *val;
	cvar_t *var;
	if (!vm) {
		return;
	}
	name = CSQCVM_Str(OFS_PARM0);
	val = CSQCVM_Str(OFS_PARM1);
	if (!name || !name[0]) {
		return;
	}
	var = Cvar_Find(name);
	if (!var) {
		// FindOrGet: create if missing; validate/cap and keep it out of config.cfg
		// (CVAR_MOD_CREATED is honoured by DumpVariables), delete on unload.
		if (!CSQC_Client_RegisterModuleCvar(name)) {
			return;
		}
		var = Cvar_Create(name, "", CVAR_MOD_CREATED);
	}
	if (var && !(var->flags & CVAR_NOTFROMSERVER)) {
		Cvar_Set(var, val ? val : "");
	}
}

/*
 float(string name, string value) registercvar = #93
 Creates an engine variable (shared namespace) if it does not exist; returns 1/0.
*/
static void CSQC_Registercvar(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *name, *value;
	if (!vm) {
		return;
	}
	name = CSQCVM_Str(OFS_PARM0);
	value = CSQCVM_Str(OFS_PARM1);
	if (!name || !name[0]) {
		vm->globals[OFS_RETURN] = 0;
		return;
	}
	if (Cvar_Find(name)) {
		vm->globals[OFS_RETURN] = 0;
		return;
	}
	if (!CSQC_Client_RegisterModuleCvar(name)) {
		vm->globals[OFS_RETURN] = 0;
		return;
	}
	Cvar_Create(name, value ? value : "", CVAR_MOD_CREATED);
	vm->globals[OFS_RETURN] = 1;
}

/*
 float(string ext) checkextension = #99
 A name is advertised only when all its member builtins are registered and not
 stubs. Names use the canonical spellings (including the '_' of _DP_TE_*).
 EXT_CSQC and EXT_CSQC_SHARED are protocol, not builtins: they report the negotiated
 FTE_PEXT_CSQC bit.
 The exceptions list holds names advertised without a builtin check: extensions
 that have no builtins at all (behavioural guarantees). Names with a missing or
 stub member are simply absent: FRIK_FILE, FTE_QC_INTCONV,
 DP_QC_FS_SEARCH(_PACKFILE), DP_QC_GETSURFACE, DP_QC_WHICHPACK, DP_QC_URI_ESCAPE,
 KRIMZON_SV_PARSECLIENTCOMMAND, QW_ENGINE.
*/
struct csqc_ext_map_s {
	const char *name;
	const int *members; // module builtin numbers that must all be implemented
	int count;
};

static const int csqc_ext_sincossqrtpow[] = { 60, 61, 62, 97 };
static const int csqc_ext_minmaxbound[] = { 94, 95, 96 };
static const int csqc_ext_randomvec[] = { 91 };
static const int csqc_ext_registercvar[] = { 93 };
static const int csqc_ext_cvar_string[] = { 448 };
static const int csqc_ext_cvar_defstring[] = { 482 };
static const int csqc_ext_cvar_type[] = { 495 };
static const int csqc_ext_edict_num[] = { 459 };
static const int csqc_ext_etos[] = { 65 };
static const int csqc_ext_findfloat[] = { 98 };
static const int csqc_ext_strftime[] = { 478 };
static const int csqc_ext_strreplace[] = { 484, 485 };
static const int csqc_ext_tokenizebyseparator[] = { 479 };
static const int csqc_ext_sprintf[] = { 627 };
static const int csqc_ext_string_case[] = { 480, 481 };
static const int csqc_ext_string_color[] = { 476, 477 };
static const int csqc_ext_crc16[] = { 494 };
static const int csqc_ext_asinacos[] = { 471, 472, 473, 474, 475 };
static const int csqc_ext_changepitch[] = { 63 };
static const int csqc_ext_vectorvectors[] = { 432 };
static const int csqc_ext_tracebox[] = { 90 };
static const int csqc_ext_entitydata[] = { 496, 497, 498, 499, 500 };
static const int csqc_ext_bitshift[] = { 218 };
static const int csqc_ext_te_blood[] = { 405 };
static const int csqc_ext_te_bloodshower[] = { 406 };
static const int csqc_ext_te_explosionrgb[] = { 407 };
static const int csqc_ext_te_particlecube[] = { 408 };
static const int csqc_ext_te_particlerain[] = { 409 };
static const int csqc_ext_te_particlesnow[] = { 410 };
static const int csqc_ext_te_spark[] = { 411 };
static const int csqc_ext_te_smallflash[] = { 416 };
static const int csqc_ext_te_customflash[] = { 417 };
static const int csqc_ext_te_quadeffects[] = { 412, 413, 414, 415 };
static const int csqc_ext_te_flamejet[] = { 457 };
static const int csqc_ext_te_plasmaburn[] = { 433 };
static const int csqc_ext_te_standard_dp[] = { 418, 419, 420, 421, 422, 423, 424, 425, 426, 427, 428, 429, 430, 431 };
static const int csqc_ext_te_standard_fte[] = { 418, 419, 420, 421, 422, 423, 424, 425, 426, 428, 429, 430, 219, 239 };
static const int csqc_ext_digest_sha1[] = { 639 };
static const int csqc_ext_strings[] = {
	81, 114, 115, 116, 117, 118, 119,
	221, 222, 223, 224, 225, 226, 227, 228, 229, 230
};
static const int csqc_ext_findchain[] = { 402 };
static const int csqc_ext_findchainfloat[] = { 403 };
static const int csqc_ext_findflags[] = { 449 };
static const int csqc_ext_findchainflags[] = { 450 };
static const int csqc_ext_stringbuffers[] = { 460, 461, 462, 463, 464, 465, 466, 467, 468, 469 };
static const int csqc_ext_copyentity[] = { 400 };
static const int csqc_ext_zq_strings[] = { 81, 114, 115, 116, 117, 118, 119 };
static const int csqc_ext_checkcommand[] = { 294 };
static const int csqc_ext_calltimeofday[] = { 231 };
static const int csqc_ext_effect[] = { 404 };
static const int csqc_ext_pointsound[] = { 483 };
static const int csqc_ext_print[] = { 339 };
static const int csqc_ext_infokey[] = { 80, 81 };
static const int csqc_ext_rotatemoves[] = { 638 };

#define CSQC_COUNTOF(a) ((int)(sizeof (a) / sizeof ((a)[0])))

static const struct csqc_ext_map_s csqc_ext_map[] = {
	{ "DP_QC_SINCOSSQRTPOW", csqc_ext_sincossqrtpow, CSQC_COUNTOF(csqc_ext_sincossqrtpow) },
	{ "DP_QC_MINMAXBOUND", csqc_ext_minmaxbound, CSQC_COUNTOF(csqc_ext_minmaxbound) },
	{ "DP_QC_RANDOMVEC", csqc_ext_randomvec, CSQC_COUNTOF(csqc_ext_randomvec) },
	{ "DP_REGISTERCVAR", csqc_ext_registercvar, CSQC_COUNTOF(csqc_ext_registercvar) },
	{ "DP_QC_CVAR_STRING", csqc_ext_cvar_string, CSQC_COUNTOF(csqc_ext_cvar_string) },
	{ "DP_QC_CVAR_DEFSTRING", csqc_ext_cvar_defstring, CSQC_COUNTOF(csqc_ext_cvar_defstring) },
	{ "DP_QC_CVAR_TYPE", csqc_ext_cvar_type, CSQC_COUNTOF(csqc_ext_cvar_type) },
	{ "DP_QC_EDICT_NUM", csqc_ext_edict_num, CSQC_COUNTOF(csqc_ext_edict_num) },
	{ "DP_QC_ETOS", csqc_ext_etos, CSQC_COUNTOF(csqc_ext_etos) },
	{ "DP_QC_FINDFLOAT", csqc_ext_findfloat, CSQC_COUNTOF(csqc_ext_findfloat) },
	{ "DP_QC_STRFTIME", csqc_ext_strftime, CSQC_COUNTOF(csqc_ext_strftime) },
	{ "DP_QC_STRREPLACE", csqc_ext_strreplace, CSQC_COUNTOF(csqc_ext_strreplace) },
	{ "DP_QC_TOKENIZEBYSEPARATOR", csqc_ext_tokenizebyseparator, CSQC_COUNTOF(csqc_ext_tokenizebyseparator) },
	{ "DP_QC_SPRINTF", csqc_ext_sprintf, CSQC_COUNTOF(csqc_ext_sprintf) },
	{ "DP_QC_STRING_CASE_FUNCTIONS", csqc_ext_string_case, CSQC_COUNTOF(csqc_ext_string_case) },
	{ "DP_QC_STRINGCOLORFUNCTIONS", csqc_ext_string_color, CSQC_COUNTOF(csqc_ext_string_color) },
	{ "DP_QC_CRC16", csqc_ext_crc16, CSQC_COUNTOF(csqc_ext_crc16) },
	{ "DP_QC_ASINACOSATANATAN2TAN", csqc_ext_asinacos, CSQC_COUNTOF(csqc_ext_asinacos) },
	{ "DP_QC_CHANGEPITCH", csqc_ext_changepitch, CSQC_COUNTOF(csqc_ext_changepitch) },
	{ "DP_QC_VECTORVECTORS", csqc_ext_vectorvectors, CSQC_COUNTOF(csqc_ext_vectorvectors) },
	{ "DP_QC_TRACEBOX", csqc_ext_tracebox, CSQC_COUNTOF(csqc_ext_tracebox) },
	{ "DP_QC_ENTITYDATA", csqc_ext_entitydata, CSQC_COUNTOF(csqc_ext_entitydata) },
	{ "EXT_BITSHIFT", csqc_ext_bitshift, CSQC_COUNTOF(csqc_ext_bitshift) },
	{ "DP_TE_BLOOD", csqc_ext_te_blood, CSQC_COUNTOF(csqc_ext_te_blood) },
	{ "_DP_TE_BLOODSHOWER", csqc_ext_te_bloodshower, CSQC_COUNTOF(csqc_ext_te_bloodshower) },
	{ "DP_TE_EXPLOSIONRGB", csqc_ext_te_explosionrgb, CSQC_COUNTOF(csqc_ext_te_explosionrgb) },
	{ "DP_TE_PARTICLECUBE", csqc_ext_te_particlecube, CSQC_COUNTOF(csqc_ext_te_particlecube) },
	{ "DP_TE_PARTICLERAIN", csqc_ext_te_particlerain, CSQC_COUNTOF(csqc_ext_te_particlerain) },
	{ "DP_TE_PARTICLESNOW", csqc_ext_te_particlesnow, CSQC_COUNTOF(csqc_ext_te_particlesnow) },
	{ "DP_TE_SPARK", csqc_ext_te_spark, CSQC_COUNTOF(csqc_ext_te_spark) },
	{ "DP_TE_SMALLFLASH", csqc_ext_te_smallflash, CSQC_COUNTOF(csqc_ext_te_smallflash) },
	{ "DP_TE_CUSTOMFLASH", csqc_ext_te_customflash, CSQC_COUNTOF(csqc_ext_te_customflash) },
	{ "_DP_TE_QUADEFFECTS1", csqc_ext_te_quadeffects, CSQC_COUNTOF(csqc_ext_te_quadeffects) },
	{ "_DP_TE_FLAMEJET", csqc_ext_te_flamejet, CSQC_COUNTOF(csqc_ext_te_flamejet) },
	{ "_DP_TE_PLASMABURN", csqc_ext_te_plasmaburn, CSQC_COUNTOF(csqc_ext_te_plasmaburn) },
	{ "DP_TE_STANDARDEFFECTBUILTINS", csqc_ext_te_standard_dp, CSQC_COUNTOF(csqc_ext_te_standard_dp) },
	{ "FTE_TE_STANDARDEFFECTBUILTINS", csqc_ext_te_standard_fte, CSQC_COUNTOF(csqc_ext_te_standard_fte) },
	{ "FTE_QC_DIGEST_SHA1", csqc_ext_digest_sha1, CSQC_COUNTOF(csqc_ext_digest_sha1) },
	{ "FTE_STRINGS", csqc_ext_strings, CSQC_COUNTOF(csqc_ext_strings) },
	{ "DP_QC_FINDCHAIN", csqc_ext_findchain, CSQC_COUNTOF(csqc_ext_findchain) },
	{ "DP_QC_FINDCHAINFLOAT", csqc_ext_findchainfloat, CSQC_COUNTOF(csqc_ext_findchainfloat) },
	{ "DP_QC_FINDFLAGS", csqc_ext_findflags, CSQC_COUNTOF(csqc_ext_findflags) },
	{ "DP_QC_FINDCHAINFLAGS", csqc_ext_findchainflags, CSQC_COUNTOF(csqc_ext_findchainflags) },
	{ "DP_QC_STRINGBUFFERS", csqc_ext_stringbuffers, CSQC_COUNTOF(csqc_ext_stringbuffers) },
	{ "DP_QC_COPYENTITY", csqc_ext_copyentity, CSQC_COUNTOF(csqc_ext_copyentity) },
	{ "ZQ_QC_STRINGS", csqc_ext_zq_strings, CSQC_COUNTOF(csqc_ext_zq_strings) },
	{ "FTE_QC_CHECKCOMMAND", csqc_ext_checkcommand, CSQC_COUNTOF(csqc_ext_checkcommand) },
	{ "FTE_CALLTIMEOFDAY", csqc_ext_calltimeofday, CSQC_COUNTOF(csqc_ext_calltimeofday) },
	{ "DP_SV_EFFECT", csqc_ext_effect, CSQC_COUNTOF(csqc_ext_effect) },
	{ "DP_SV_POINTSOUND", csqc_ext_pointsound, CSQC_COUNTOF(csqc_ext_pointsound) },
	{ "DP_SV_PRINT", csqc_ext_print, CSQC_COUNTOF(csqc_ext_print) },
	{ "FTE_QC_INFOKEY", csqc_ext_infokey, CSQC_COUNTOF(csqc_ext_infokey) },
	{ "DP_CSQC_ROTATEMOVES", csqc_ext_rotatemoves, CSQC_COUNTOF(csqc_ext_rotatemoves) },
	{ NULL, NULL, 0 }
};

static const char *csqc_ext_exceptions[] = {
	"DP_QC_MULTIPLETEMPSTRINGS",  // temp strings are GC'd, never overwritten
	"DP_QC_UNLIMITEDTEMPSTRINGS",
	"DP_QC_VECTOANGLES_WITH_ROLL",  // #51 takes the optional up vector
	"DP_CON_SET",
	"FTE_QC_HARDWARECURSORS",   // #343 image/hotspot/scale
	NULL
};

// Every member registered and not a no-op stub.
static qbool CSQC_Ext_All_Implemented(pr1vm_t *vm, const int *members, int count)
{
	int i, num;
	for (i = 0; i < count; i++) {
		num = members[i];
		if (num <= 0 || num >= vm->numbuiltins || !vm->builtins[num]) {
			return false;
		}
		if (PR1VM_IsBuiltinStub(vm, num)) {
			return false;
		}
	}
	return true;
}

static void CSQC_Checkextension(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *ext;
	int i;
	if (!vm) {
		return;
	}
	ext = CSQCVM_Str(OFS_PARM0);
	vm->globals[OFS_RETURN] = 0;
	if (!ext) {
		return;
	}
	if (!strcmp(ext, "EXT_CSQC") || !strcmp(ext, "EXT_CSQC_SHARED")) {
		vm->globals[OFS_RETURN] = !!(cls.fteprotocolextensions & FTE_PEXT_CSQC);
		return;
	}
	for (i = 0; csqc_ext_exceptions[i]; i++) {
		if (!strcmp(ext, csqc_ext_exceptions[i])) {
			vm->globals[OFS_RETURN] = 1;
			return;
		}
	}
	for (i = 0; csqc_ext_map[i].name; i++) {
		if (!strcmp(ext, csqc_ext_map[i].name)) {
			vm->globals[OFS_RETURN] = CSQC_Ext_All_Implemented(vm, csqc_ext_map[i].members, csqc_ext_map[i].count);
			return;
		}
	}
}

/*
 float(func_t funcref) checkbuiltin = #0 (name-mapped)
 1 only when the referenced function is a registered, non-stub builtin.
*/
static void CSQC_Checkbuiltin(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int funcref, binum;
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN] = 0;
	// func_t is a raw int in the float slot; a float cast would truncate a small
	// index (a denormal bit pattern) to 0.
	funcref = *(int *)&vm->globals[OFS_PARM0];
	if (funcref <= 0 || !vm->functions || !vm->progs || funcref >= vm->progs->numfunctions) {
		return;
	}
	binum = vm->functions[funcref].first_statement;
	if (binum >= 0) {
		return;     // not a builtin
	}
	binum = -binum;
	if (binum <= 0 || binum >= vm->numbuiltins || !vm->builtins[binum]) {
		return;
	}
	if (PR1VM_IsBuiltinStub(vm, binum)) {
		return;
	}
	vm->globals[OFS_RETURN] = 1;
}

/*
 Slot for a `#0` name the module declares but the engine does not implement:
 aborts on the first call, while loading still succeeds. first_statement cannot
 stay 0 (the interpreter would enter it as a body), so unresolved names map here.
 All such names share the slot, so the error takes the name from the function the
 running call statement names.
*/
static void CSQC_Named_Unknown(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	const char *name = NULL, *caller = NULL;
	dstatement_t *st;
	int fnum;

	if (vm && vm->xstatement >= 0 && vm->xstatement < vm->progs->numstatements) {
		st = &vm->statements[vm->xstatement];
		if (st->op >= OP_CALL0 && st->op <= OP_CALL8) {
			fnum = *(int *)&vm->globals[(unsigned short)st->a];
			if (fnum > 0 && fnum < vm->progs->numfunctions) {
				name = CSQC_Client_GetString(vm, vm->functions[fnum].s_name);
			}
		}
		if (vm->xfunction) {
			caller = CSQC_Client_GetString(vm, vm->xfunction->s_name);
		}
	}
	PR_RunError("CSQC builtin \"%s\" is not implemented (called from %s).\nCSQC is not compatible.",
		name ? name : "?", caller ? caller : "?");
}

/*
 Resolve `#0` (name-mapped) builtins of a freshly loaded client csprogs: a function
 with first_statement == 0 (i > 0) has no body - its slot is assigned by name.
 Known names map to their engine slot; unknown names map to CSQC_Named_Unknown.
 After this call no i > 0 function keeps first_statement == 0: the interpreter
 would enter it as a body. Called by the v6 loader after PR1VM_LoadData.
*/
static const struct
{
	const char *name;
	int num;
} csqc_named_builtins[] = {
	{ "checkbuiltin", CSQC_BUILTIN_CHECKBUILTIN },
	{ "argc", CSQC_BUILTIN_ARGC },
	{ "getplayerkeyfloat", CSQC_BUILTIN_GETPLAYERKEYFLOAT },
	{ "serverkeyfloat", CSQC_BUILTIN_SERVERKEYFLOAT },
	{ NULL, 0 }
};

void CSQCVM_ResolveNamedBuiltins(pr1vm_t *vm)
{
	int i, j;
	if (!vm || !vm->functions || !vm->progs) {
		return;
	}
	for (i = 1; i < vm->progs->numfunctions; i++) {
		const char *name;
		if (vm->functions[i].first_statement != 0) {
			continue;
		}
		name = CSQC_Client_GetString(vm, vm->functions[i].s_name);
		if (name) {
			for (j = 0; csqc_named_builtins[j].name; j++) {
				if (!strcmp(name, csqc_named_builtins[j].name)) {
					vm->functions[i].first_statement = -csqc_named_builtins[j].num;
					break;
				}
			}
		}
		if (vm->functions[i].first_statement == 0) {
			if (name) {
				Con_DPrintf("Unknown csqc builtin: %s\n", name);
			}
			vm->functions[i].first_statement = -CSQC_BUILTIN_NAMED_UNKNOWN;
		}
	}
}

/*
 void() calltimeofday = #231
 Calls the module's "timeofday" function, if any, with local time:
 (sec, min, hour, day, mon (0-based), year, date string).
*/
static void CSQC_Calltimeofday(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	dfunction_t *f;
	time_t t;
	struct tm *ltm;
	char str[128];
	if (!vm) {
		return;
	}
	f = PR1VM_FindFunction(vm, "timeofday");
	if (!f) {
		return;
	}
	t = time(NULL);
	ltm = localtime(&t);
	if (!ltm) {
		return;
	}
	vm->globals[OFS_PARM0] = (float)ltm->tm_sec;
	vm->globals[OFS_PARM1] = (float)ltm->tm_min;
	vm->globals[OFS_PARM2] = (float)ltm->tm_hour;
	vm->globals[OFS_PARM3] = (float)ltm->tm_mday;
	vm->globals[OFS_PARM4] = (float)ltm->tm_mon;
	vm->globals[OFS_PARM5] = (float)(ltm->tm_year + 1900);
	strftime(str, sizeof(str), "%a %b %d, %H:%M:%S %Y", ltm);
	PR1VM_ClientSetString(vm, (int *)&vm->globals[OFS_PARM6], str);
	PR1VM_ExecuteProgram(vm, (func_t)(f - vm->functions));
}

/*
 String results are GC'd temp strings, collected after the outermost call once
 unreferenced.
*/

/*
string(vector v) vtos = #27 - "'%f %f %f'".
*/
static void CSQC_Vtos(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char buf[64];
	if (!vm) {
		return;
	}
	snprintf(buf, sizeof(buf), "'%f %f %f'", vm->globals[OFS_PARM0], vm->globals[OFS_PARM0 + 1], vm->globals[OFS_PARM0 + 2]);
	CSQCVM_SetRetStr(buf);
}

/*
float(string s) stof = #81
*/
static void CSQC_Stof(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *s = CSQCVM_Str(OFS_PARM0);
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN] = (float)strtod(s ? s : "", NULL);
}

/*
float(string s) strlen = #114
*/
static void CSQC_Strlen(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *s = CSQCVM_Str(OFS_PARM0);
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN] = s ? (float)strlen(s) : 0;
}

/*
 string(string s, float start, float count) substring = #116
 Negative start/length count from the end; start>=slen or length<=0 -> "".
*/
static void CSQC_Substring(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *s = CSQCVM_Str(OFS_PARM0);
	char buf[2048];
	int start, len, l;
	if (!vm) {
		return;
	}
	if (!s) {
		s = "";
	}
	start = (int)vm->globals[OFS_PARM1];
	len = (int)vm->globals[OFS_PARM2];
	l = strlen(s);
	if (start < 0) {
		start = l + start;
	}
	if (len < 0) {
		len = l - start + (len + 1);
	}
	if (start < 0) {
		start = 0;
	}
	if (start >= l || len <= 0 || l == 0) {
		CSQCVM_SetRetStr("");
		return;
	}
	l -= start;
	if (len > l) {
		len = l;
	}
	if (len > (int)sizeof(buf) - 1) {
		len = (int)sizeof(buf) - 1;
	}
	strlcpy(buf, s + start, (size_t)len + 1);
	CSQCVM_SetRetStr(buf);
}

/*
 vector(string s) stov = #117
 Parses the vtos format ("'x y z'"); missing components are 0.
*/
static void CSQC_Stov(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *p = CSQCVM_VarString(0);
	double v[3];
	int i;
	char *end;
	if (!vm) {
		return;
	}
	if (!p) {
		p = "";
	}
	for (i = 0; i < 3; i++) {
		while (*p == ' ' || *p == '\t') {
			p++;
		}
		if (*p == '\'' || *p == '"') {
			p++;
		}
		v[i] = strtod(p, &end);
		if (end == p) {
			v[i] = 0;
			while (*p && *p != ' ') {
				p++;
			}
		}
		else {
			p = end;
		}
	}
	vm->globals[OFS_RETURN] = (float)v[0];
	vm->globals[OFS_RETURN + 1] = (float)v[1];
	vm->globals[OFS_RETURN + 2] = (float)v[2];
}

/*
 string(string s, ...) strzone = #118
 Concatenation of up to 8 arguments as a GC'd temp string: lifetime is
 reference-based, no explicit free. Allocation failure -> 0.
*/
static void CSQC_Strzone(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int n, i, len = 0;
	char *buf;

	if (!vm) {
		return;
	}

	n = vm->argc;
	if (n < 0) {
		n = 0;
	}
	if (n > 8) {
		n = 8;
	}

	for (i = 0; i < n; i++) {
		char *s = CSQC_Client_GetString(vm, *(int *)&vm->globals[OFS_PARM0 + i * 3]);
		if (s) {
			len += strlen(s);
		}
	}

	buf = (char *)Q_malloc(len + 1);
	if (!buf) {
		vm->globals[OFS_RETURN] = 0;
		return;
	}

	len = 0;
	for (i = 0; i < n; i++) {
		char *s = CSQC_Client_GetString(vm, *(int *)&vm->globals[OFS_PARM0 + i * 3]);
		if (s) {
			int l = (int)strlen(s);
			memcpy(buf + len, s, l);
			len += l;
		}
	}
	buf[len] = 0;

	PR1VM_ClientSetString(vm, (int *)&vm->globals[OFS_RETURN], buf);
	Q_free(buf);
}

/*
 void(string s) strunzone = #119
 No-op: strzone results are collected by the temp-string GC.
*/
static void CSQC_Strunzone(void)
{
}

/*
string(string varname) cvar_string = #448
 Returns the latched value if one is pending; "" for unknown or read-guarded cvars.
*/
static void CSQC_Cvar_String(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *name = CSQCVM_Str(OFS_PARM0);
	cvar_t *var;
	if (!vm) {
		return;
	}
	if (!name) {
		name = "";
	}
	if (CSQC_CvarReadDenied(name)) {
		CSQCVM_SetRetStr("");
		return;
	}
	var = Cvar_Find(name);
	CSQCVM_SetRetStr(var ? (var->latchedString ? var->latchedString : var->string) : "");
}

/*
 void() breakpoint = #6
 No-op: the engine has no QC debugger.
*/
static void CSQC_Breakpoint(void)
{
}

/*
 void(entity e, float chan, string samp, float vol, float atten,
      optional float pitchpct, optional float flags, optional float timeofs) sound = #8
 Played at the entity origin (bbox centre for SOLID_BSP) on sound entity -slot, so
 CSQC channels never collide with server entity numbers. vol < 0 stops the channel.
 The origin is sampled once: sound channels cannot follow an entity. Pitch, flags and
 timeofs have no mixer support and are ignored.
*/
static void CSQC_Sound(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float *g, *o, *mn, *mx, *solid;
	vec3_t org;
	char *n;
	sfx_t *sfx;
	int e, chan;
	float vol, atten;
	if (!vm) {
		return;
	}
	g = vm->globals;
	e = CSQC_Ent_Of(vm, OFS_PARM0);
	chan = (int)g[OFS_PARM0 + 3];
	n = CSQCVM_Str(OFS_PARM2);
	vol = g[OFS_PARM0 + 9];
	atten = g[OFS_PARM0 + 12];

	if (vol < 0) {
		S_StopSound(-e, chan);
		return;
	}
	if (!n || !n[0]) {
		return;
	}

	VectorClear(org);
	o = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_ORIGIN));
	if (o) {
		VectorCopy(o, org);
	}
	solid = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_SOLID));
	mn = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_MINS));
	mx = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_MAXS));
	if (solid && (int)*solid == CSQC_SOLID_BSP && mn && mx) {
		VectorMA(org, 0.5f, mn, org);
		VectorMA(org, 0.5f, mx, org);
	}

	sfx = S_PrecacheSound(n);
	if (sfx) {
		S_StartSound(-e, chan, sfx, org, vol, atten);
	}
}

/*
 void(string str) precache_sound = #19/#76
 Precaches locally and queues a missing sound/<name> for download. OFS_RETURN is
 not written.
*/
static void CSQC_Precache_Sound(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *n = CSQCVM_Str(OFS_PARM0);
	if (vm && n && n[0]) {
		S_PrecacheSound(n);
		// `*` = sexed sound (not downloadable). The distinct-name cap keeps
		// S_FindName from hitting MAX_SFX.
		if (n[0] != '*' && CSQC_Client_AllowModuleSound(n)) {
			CSQC_Client_RequestDownload(va("sound/%s", n));
		}
	}
}

static void CSQC_Precache_Model(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *n = CSQCVM_Str(OFS_PARM0);
	if (vm && n && n[0]) {
		int idx = CSQC_Client_ModelIndex(n);
		// ModelIndex returns a stable index even for a missing model (NULL
		// placeholder), so a missing file shows as an unloaded model, not index 0.
		// Queue it for download.
		if (!CSQC_Client_ModelForIndex(idx) && n[0] != '*') {
			CSQC_Client_RequestDownload(n);
		}
	}
	CSQCVM_SetRetStr(n ? n : "");
}

/*
 float(string modelname, optional float queryonly) getmodelindex = #200.
 Sign-split index space: positive = server precache (cl.model_name), negative =
 csqc-only slot. queryonly!=0 - only look up (no load/registration); otherwise
 register a missing model as a csqc-only slot. The csqc layer is searched first.
*/
static void CSQC_Getmodelindex(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *n;
	int queryonly;
	if (!vm) {
		return;
	}
	n = CSQCVM_Str(OFS_PARM0);
	queryonly = (vm->argc > 1) ? (int)vm->globals[OFS_PARM1] : 0;
	vm->globals[OFS_RETURN] = (float)(queryonly ? CSQC_Client_ModelIndexKnown(n) : CSQC_Client_ModelIndex(n));
}

/*
 string(float mdlindex) modelnameforindex = #334.
 idx > 0 -> cl.model_name[idx], idx < 0 -> csqc slot, idx == 0 -> "".
*/
static void CSQC_Modelnameforindex(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	const char *name;
	int idx;

	if (!vm) {
		return;
	}
	idx = (int)vm->globals[OFS_PARM0];
	name = CSQC_Client_ModelNameForIndex(idx);
	CSQCVM_SetRetStr((char *)(name ? name : ""));
}

/*
 float(string) precache_file (#68/#77)
 1 if the file is present, 0 if it was queued for download (or could not be).
 */
static void CSQC_Precache_File(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *n;

	if (!vm) {
		return;
	}
	n = CSQCVM_Str(OFS_PARM0);
	if (!n || !n[0]) {
		vm->globals[OFS_RETURN] = 0;
		return;
	}
	vm->globals[OFS_RETURN] = CSQC_Client_RequestDownload(n) ? 1.0f : 0.0f;
}

/*
 void(vector pos, string samp, float vol, float atten) ambientsound = #74
 Looping static sound at pos. S_StaticSound takes the svc_spawnstaticsound wire
 scaling (vol * 255, atten * 64).
*/
static void CSQC_Ambientsound(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *n = CSQCVM_Str(OFS_PARM1);
	sfx_t *sfx;
	if (!vm || !n || !n[0]) {
		return;
	}
	sfx = S_PrecacheSound(n);
	if (sfx) {
		S_StaticSound(sfx, &vm->globals[OFS_PARM0], vm->globals[OFS_PARM0 + 6] * 255, vm->globals[OFS_PARM0 + 9] * 64);
	}
}

/*
void(vector pos, vector dir, float colour, float count) particle = #48
*/
static void CSQC_Particle(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float *g;
	if (!vm) {
		return;
	}
	g = vm->globals;
	if (!PScript_RunParticleEffect(&g[OFS_PARM0], &g[OFS_PARM0 + 3], (int)g[OFS_PARM0 + 6], (int)g[OFS_PARM0 + 9])) {
		R_RunParticleEffect(&g[OFS_PARM0], &g[OFS_PARM0 + 3], (int)g[OFS_PARM0 + 6], (int)g[OFS_PARM0 + 9]);
	}
}

/*
 void(float lightstyle, string stylestring, optional vector rgb) lightstyle = #35
 No-op: the client does not style lights.
*/
static void CSQC_Lightstyle(void)
{
}

/*
 void(float pause) setpause = #531
 No-op: the client cannot pause the server.
*/
static void CSQC_Setpause(void)
{
}

/*
 Basic entities on the arena. PR1 entity value = entnum*edict_size (int bits). Field
 types (eprint): code 1 = ev_string, 2 = ev_float, 3 = ev_vector, 4 = ev_entity.
*/

static int CSQC_Ent_Of(pr1vm_t *vm, int parmofs)
{
	if (!vm) {
		return 0;
	}
	return CSQC_Client_EntNum(vm, *(int *)&vm->globals[parmofs]);
}

static float *CSQC_Ent_Slot(pr1vm_t *vm, int entnum)
{
	if (!vm || !vm->game_edicts) {
		return NULL;
	}
	if (entnum < 0 || entnum >= vm->num_edicts) {
		return NULL;
	}
	return (float *)((byte *)vm->game_edicts + (size_t)entnum * vm->edict_size);
}

// Field access via a cached offset (CSQC_Client_FieldOfs): no fielddefs scan on
// the addentities hot path.
static float *CSQC_Ent_Ofs(pr1vm_t *vm, int entnum, int fldofs)
{
	float *slot;
	if (fldofs < 0) {
		return NULL;
	}
	slot = CSQC_Ent_Slot(vm, entnum);
	return slot ? &slot[fldofs] : NULL;
}

static void CSQC_Ret_Entity(pr1vm_t *vm, int entnum)
{
	*(int *)&vm->globals[OFS_RETURN] = entnum * vm->edict_size;
}

/*
 Arena edict -> entity_t. Returns false when the edict has no drawable model (or
 is filtered); *ent is then undefined. .renderflags maps to ent.renderfx (a subset
 of CSQCRF_*).
*/
static qbool CSQC_Edict_To_Entity(pr1vm_t *vm, int e, entity_t *ent)
{
	float *slot, *f;
	char *mname;
	model_t *model;
	int ofs;
	int playernum = -1; // network player index (colormap 1..MAX_CLIENTS)

	slot = CSQC_Ent_Slot(vm, e);
	if (!slot) {
		return false;
	}
	// .modelindex (positive server or negative csqc-only index), falling back to
	// the .model string.
	model = NULL;
	if ((ofs = CSQC_Client_FieldOfs(vm, CSQC_FLD_MODELINDEX)) >= 0) {
		int mi = (int)slot[ofs];
		if (mi != 0) {
			model = CSQC_Client_ModelForIndex(mi);
		}
	}
	if (!model) {
		if ((ofs = CSQC_Client_FieldOfs(vm, CSQC_FLD_MODEL)) < 0) {
			return false;
		}
		mname = CSQC_Client_GetString(vm, (string_t) * (int *)&slot[ofs]);
		if (!mname || !mname[0]) {
			return false;
		}
		// Resolve through the capped tolerant CSQC registry instead of the
		// uncapped Mod_FindName path: a distinct .model string per frame would
		// exhaust mod_known (Sys_Error) and a found non-model file would Host_Error.
		{
			int midx = CSQC_Client_ModelIndex(mname);
			model = (midx != 0) ? CSQC_Client_ModelForIndex(midx) : NULL;
		}
	}
	if (!model) {
		return false;
	}

	memset(ent, 0, sizeof(*ent));
	ent->model = model;
	ent->colormap = vid.colormap;
	// .colormap as player index (1..MAX_CLIENTS) -> that player's translation table
	// and scoreboard (skin), as the engine renders players. Like the engine, only
	// MOD_PLAYER models are skinned (heads/gibs are not). Values > MAX_CLIENTS (DP
	// colormap) keep vid.colormap and no scoreboard.
	if ((f = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_COLORMAP)))) {
		int cm = (int)f[0];
		if (cm > 0 && cm <= MAX_CLIENTS && model->modhint == MOD_PLAYER) {
			ent->colormap = cl.players[cm - 1].translations;
			ent->scoreboard = &cl.players[cm - 1];
			playernum = cm - 1;
		}
	}
	ent->oldframe = ent->frame;
	ent->framelerp = -1;
	if ((f = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_ORIGIN)))) {
		VectorCopy(f, ent->origin);
	}
	if ((f = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_ANGLES)))) {
		VectorCopy(f, ent->angles);
	}
	// CSQC .angles use the mesh pitch convention for mdl/md3 and plain
	// AngleVectors orientation for everything else. R_RotateForEntity rotates every
	// model by -pitch (the mesh convention), so mdl/md3 and oriented sprites take
	// .angles as-is and brush models flip the sign.
	if (model->type == mod_brush) {
		ent->angles[PITCH] = -ent->angles[PITCH];
	}
	if ((f = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_FRAME)))) {
		ent->frame = ent->oldframe = (int)f[0];
	}
	// .lerpfrac is the weight of .frame2, framelerp the weight of ent->frame (from
	// oldframe). Clamped to 0..1: the renderer cannot extrapolate.
	if ((f = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_LERPFRAC))) && f[0] != 0) {
		float lerpfrac = f[0];

		if ((f = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_FRAME2)))) {
			ent->oldframe = (int)f[0];
		}
		else {
			ent->oldframe = 0;
		}
		ent->framelerp = bound(0, 1 - lerpfrac, 1);
	}
	// Player frame interpolation with the CL_LinkPlayers formula, only when the
	// module does not blend itself (lerpfrac 0). Without it the pose would jump
	// when toggling csqc_delta, since the engine lerps player frames.
	else if (playernum >= 0) {
		centity_t *cent = &cl_entities[playernum + 1];
		if (cent->frametime >= 0 && cent->frametime <= cl.time) {
			ent->oldframe = cent->oldframe;
			ent->framelerp = (cl.time - cent->frametime) * 10;
		}
		else {
			ent->oldframe = ent->frame;
			ent->framelerp = -1;
		}
	}
	// cl_deadbodyfilter for CSQC-owned players, which skip the engine's filter in
	// CL_LinkPlayers. Same semantics, including the TF exception for mode 3.
	if (playernum >= 0 && model->modhint == MOD_PLAYER) {
		int i = ent->frame;
		if (cl_deadbodyfilter.value == 3 && !cl.teamfortress) {
			if (ISDEAD(i)) {
				return false;
			}
		}
		if (cl_deadbodyfilter.value == 2) {
			if (ISDEAD(i)) {
				return false;
			}
		}
		else if (cl_deadbodyfilter.value == 1) {
			if (i == 49 || i == 60 || i == 69 || i == 84 || i == 93 || i == 102) {
				return false;
			}
		}
	}
	if ((f = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_SKIN)))) {
		ent->skinnum = (int)f[0];
	}
	if ((f = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_EFFECTS)))) {
		ent->effects = (int)f[0];
	}
	if ((f = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_ALPHA)))) {
		ent->alpha = f[0];
	}
	// Uniform render scale; 0 means unscaled to the renderer and culling.
	if ((f = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_SCALE)))) {
		ent->scale = f[0];
	}
	// CSQCRF_* -> RF_*: DEPTHHACK/EXTERNALMODEL/FIRSTPERSON/USEAXIS have no
	// equivalent and are ignored.
	if ((f = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_RENDERFLAGS)))) {
		int rflags = (int)f[0];
		if (rflags & 1) {  // CSQCRF_VIEWMODEL
			ent->renderfx |= RF_WEAPONMODEL | RF_NOSHADOW;
		}
		if (rflags & 32) { // CSQCRF_NOSHADOW
			ent->renderfx |= RF_NOSHADOW;
		}
		if (rflags & 8) {  // CSQCRF_ADDITIVE
			ent->renderfx |= RF_ADDITIVEBLEND;
		}
	}
	return true;
}

/*
 Arena edict -> scene (#301/#302): optional .predraw, conversion, then PVS/frustum
 cull.
*/
static void CSQC_Add_One_Entity(int e, qbool run_predraw)
{
	pr1vm_t *vm = CSQCVM_Active();
	entity_t ent;
	float *f;

	if (!vm || e <= 0 || !CSQC_Client_EntUsed(e)) {
		return;
	}

	// Not added if .predraw returns != PREDRAW_AUTOADD(0) or removes the edict.
	// The function field is raw int bits; a float read would give 0.
	if (run_predraw && (f = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_PREDRAW))) && *(int *)&f[0] > 0) {
		qbool removed = false;
		float pret = CSQC_Client_CallPredraw(e, *(int *)&f[0], &removed);
		if (removed || pret != 0) {
			return;
		}
	}

	if (!CSQC_Edict_To_Entity(vm, e, &ent)) {
		return;
	}

	// Placed at renderscene, and never culled.
	if (ent.renderfx & RF_WEAPONMODEL) {
		CSQC_Client_AddViewModel(&ent);
		return;
	}

	R_CSQC_BeginCull();
	if (!R_CSQC_EntityVisible(&ent)) {
		return;
	}

	CL_AddEntity(&ent);
}

/*
 void(entity ent) addentity = #302
 Does not run .predraw: modules call addentity(self) from inside predraw, which
 must not recurse.
*/
static void CSQC_Addentity(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm || !CSQC_Client_SceneActive()) {
		return;
	}
	CSQC_Add_One_Entity(CSQC_Ent_Of(vm, OFS_PARM0), false);
}

/* entity() spawn = #14 */
static void CSQC_Spawn(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	CSQC_Ret_Entity(vm, CSQC_Client_EntAlloc(vm));
}

/* void(entity e) remove = #15 */
static void CSQC_Remove(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	CSQC_Client_EntFree(vm, CSQC_Ent_Of(vm, OFS_PARM0));
}

/* void(entity e, vector org) setorigin = #2 */
static void CSQC_Setorigin(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float *f, *o;
	int e;
	if (!vm) {
		return;
	}
	e = CSQC_Ent_Of(vm, OFS_PARM0);
	f = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_ORIGIN));
	if (!f) {
		return;
	}
	o = &vm->globals[OFS_PARM0 + 3];
	f[0] = o[0];
	f[1] = o[1];
	f[2] = o[2];
	CSQC_Client_LinkBounds(vm, e);
}

/*
 Copy the model bbox into .mins/.maxs/.size; model == NULL zeroes them.
*/
static void CSQC_Model_Bbox(pr1vm_t *vm, int e, model_t *model)
{
	float *fmn, *fmx, *fsz;

	if (!vm || e <= 0) {
		return;
	}
	fmn = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_MINS));
	fmx = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_MAXS));
	fsz = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_SIZE));
	if (model) {
		if (fmn) {
			fmn[0] = model->mins[0];
			fmn[1] = model->mins[1];
			fmn[2] = model->mins[2];
		}
		if (fmx) {
			fmx[0] = model->maxs[0];
			fmx[1] = model->maxs[1];
			fmx[2] = model->maxs[2];
		}
		if (fsz) {
			fsz[0] = model->maxs[0] - model->mins[0];
			fsz[1] = model->maxs[1] - model->mins[1];
			fsz[2] = model->maxs[2] - model->mins[2];
		}
	}
	else {
		if (fmn) {
			fmn[0] = fmn[1] = fmn[2] = 0;
		}
		if (fmx) {
			fmx[0] = fmx[1] = fmx[2] = 0;
		}
		if (fsz) {
			fsz[0] = fsz[1] = fsz[2] = 0;
		}
	}
}

/* void(entity e, string m) setmodel = #3 */
static void CSQC_Setmodel(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *s;
	int e, ofs, idx;
	float *slot;
	model_t *model;
	if (!vm) {
		return;
	}
	e = CSQC_Ent_Of(vm, OFS_PARM0);
	s = CSQCVM_Str(OFS_PARM0 + 3);
	slot = CSQC_Ent_Slot(vm, e);
	if (!slot || !s) {
		return;
	}
	ofs = CSQC_Client_FieldOfs(vm, CSQC_FLD_MODEL);
	if (ofs >= 0) {
		PR1VM_ClientSetString(vm, (string_t *)&slot[ofs], s);
	}
	// .modelindex from the sign-split index space (arena edicts render by index)
	idx = CSQC_Client_ModelIndex(s);
	ofs = CSQC_Client_FieldOfs(vm, CSQC_FLD_MODELINDEX);
	if (ofs >= 0) {
		slot[ofs] = (float)idx;
	}
	model = idx ? CSQC_Client_ModelForIndex(idx) : NULL;
	CSQC_Model_Bbox(vm, e, model);
	ofs = CSQC_Client_FieldOfs(vm, CSQC_FLD_MODELFLAGS);
	if (ofs >= 0 && model) {
		slot[ofs] = (float)model->flags;
	}
	CSQC_Client_LinkBounds(vm, e);
}

/* void(entity e, float mdlindex) setmodelindex = #333 */
static void CSQC_Setmodelindex(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float *f, *slot;
	int e, idx, ofs;
	const char *name;
	model_t *model;
	if (!vm) {
		return;
	}
	e = CSQC_Ent_Of(vm, OFS_PARM0);
	idx = (int)vm->globals[OFS_PARM0 + 3];
	f = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_MODELINDEX));
	if (f) {
		f[0] = (float)idx;
	}
	// An index without a name (0 / unregistered) leaves .model and the bbox alone.
	// A named but unloaded model (failed or pending download) still sets .model and
	// clears mins/maxs but keeps .size.
	name = CSQC_Client_ModelNameForIndex(idx);
	if (!name) {
		return;
	}
	slot = CSQC_Ent_Slot(vm, e);
	ofs = CSQC_Client_FieldOfs(vm, CSQC_FLD_MODEL);
	if (ofs >= 0 && slot) {
		PR1VM_ClientSetString(vm, (string_t *)&slot[ofs], (char *)name);
	}
	model = CSQC_Client_ModelForIndex(idx);
	if (model) {
		CSQC_Model_Bbox(vm, e, model);
	}
	else {
		float *fmn = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_MINS));
		float *fmx = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_MAXS));
		if (fmn) {
			VectorClear(fmn);
		}
		if (fmx) {
			VectorClear(fmx);
		}
	}
	CSQC_Client_LinkBounds(vm, e);
}

/* void(entity e, vector min, vector max) setsize = #4 */
static void CSQC_Setsize(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float *fmin, *fmax, *fsz, *mn, *mx;
	int e;
	if (!vm) {
		return;
	}
	e = CSQC_Ent_Of(vm, OFS_PARM0);
	fmin = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_MINS));
	fmax = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_MAXS));
	if (!fmin || !fmax) {
		return;
	}
	mn = &vm->globals[OFS_PARM0 + 3];
	mx = &vm->globals[OFS_PARM0 + 6];
	fmin[0] = mn[0];
	fmin[1] = mn[1];
	fmin[2] = mn[2];
	fmax[0] = mx[0];
	fmax[1] = mx[1];
	fmax[2] = mx[2];
	fsz = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_SIZE));
	if (fsz) {
		fsz[0] = mx[0] - mn[0];
		fsz[1] = mx[1] - mn[1];
		fsz[2] = mx[2] - mn[2];
	}
	CSQC_Client_LinkBounds(vm, e);
}

/* entity(entity e) nextent = #47 - module arena edicts only */
static void CSQC_Nextent(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int e;
	if (!vm) {
		return;
	}
	e = CSQC_Ent_Of(vm, OFS_PARM0);
	if (e < 0) {
		e = 0;
	}
	for (e++; e < vm->num_edicts; e++) {
		if (CSQC_Client_EntUsed(e)) {
			CSQC_Ret_Entity(vm, e);
			return;
		}
	}
	CSQC_Ret_Entity(vm, 0);
}

/* entity(entity start, .string fld, string match) find = #18 (string fields) */
static void CSQC_Find(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int e, f;
	char *s, *t;
	float *slot;
	if (!vm) {
		return;
	}
	e = CSQC_Ent_Of(vm, OFS_PARM0);
	f = *(int *)&vm->globals[OFS_PARM0 + 3];
	s = CSQCVM_Str(OFS_PARM0 + 6);
	if (f < 0 || f >= vm->progs->entityfields) {
		CSQC_Client_Abort("PF_Find: bad field reference");
		return;
	}
	if (e < 0) {
		e = 0;
	}
	if (s) {
		for (e++; e < vm->num_edicts; e++) {
			if (!CSQC_Client_EntUsed(e)) {
				continue;
			}
			slot = CSQC_Ent_Slot(vm, e);
			if (!slot) {
				continue;
			}
			t = CSQC_Client_GetString(vm, *(int *)&slot[f]);
			if (t && !strcmp(t, s)) {
				CSQC_Ret_Entity(vm, e);
				return;
			}
		}
	}
	CSQC_Ret_Entity(vm, 0);
}

#define CSQC_MOVE_NOMONSTERS	1
#define CSQC_MOVE_MISSILE	2
#define CSQC_MOVE_HITMODEL	4
#define CSQC_MOVE_TRIGGERS	16
#define CSQC_MOVE_EVERYTHING	32
#define CSQC_MOVE_LAGGED	64

/* entity(vector org, float rad) findradius = #22 - arena; chain if the field exists */
static void CSQC_Findradius(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float *org, *o, *fld, rad, d;
	float *chslot;
	int e, chain_ofs, prev;
	if (!vm) {
		return;
	}
	org = &vm->globals[OFS_PARM0];
	rad = vm->globals[OFS_PARM0 + 3];
	chain_ofs = CSQC_Client_FieldOfs(vm, CSQC_FLD_CHAIN);
	prev = 0; // chain head; world(0) is the terminator
	for (e = CSQC_Client_EntSpawnBase(); e < vm->num_edicts; e++) {
		if (!CSQC_Client_EntUsed(e)) {
			continue;
		}
		o = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_ORIGIN));
		if (!o) {
			continue;
		}
		// non-solid is skipped unless FL_FINDABLE_NONSOLID is set.
		fld = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_SOLID));
		if (fld && (int)*fld == CSQC_SOLID_NOT) {
			fld = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_FLAGS));
			if (!fld || !((int)*fld & CSQC_FL_FINDABLE_NONSOLID)) {
				continue;
			}
		}
		d = (o[0] - org[0]) * (o[0] - org[0]) + (o[1] - org[1]) * (o[1] - org[1]) + (o[2] - org[2]) * (o[2] - org[2]);
		if (d > rad * rad) {
			continue;
		}
		// Link onto .chain; the last match is returned as the head.
		if (chain_ofs >= 0) {
			chslot = CSQC_Ent_Slot(vm, e);
			if (chslot) {
				*(int *)&chslot[chain_ofs] = prev * vm->edict_size;
			}
		}
		prev = e;
	}
	CSQC_Ret_Entity(vm, prev);
}

/*
 Entity search/copy (#400/#402/#403/#449/#450). Field references are float-word
 offsets (ddef_t.ofs). Chains store the previous match's entity value in the
 chainfield, terminated by world(0).
*/

/* void(entity from, entity to) copyentity = #400 */
static void CSQC_Copyentity(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int from, to;
	float *src, *dst;
	if (!vm) {
		return;
	}
	from = CSQC_Ent_Of(vm, OFS_PARM0);
	if (vm->argc <= 1) {
		to = CSQC_Client_EntAlloc(vm);
	}
	else {
		to = CSQC_Ent_Of(vm, OFS_PARM0 + 3);
	}
	// A free source or destination is fatal.
	if (from <= 0 || !CSQC_Client_EntUsed(from)) {
		CSQC_Client_Abort("PF_copyentity: source is free");
		return;
	}
	if (to <= 0 || !CSQC_Client_EntUsed(to)) {
		CSQC_Client_Abort("PF_copyentity: destination is free");
		return;
	}
	src = CSQC_Ent_Slot(vm, from);
	dst = CSQC_Ent_Slot(vm, to);
	if (!src || !dst) {
		return;
	}
	memcpy(dst, src, vm->edict_size);
	// No relink needed: culling is per frame. Returns dest for modules that use it.
	CSQC_Ret_Entity(vm, to);
}

/* entity(.string field, string match, .entity chainfield) findchain = #402 */
static void CSQC_Findchain(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int f, cf, e, prev;
	char *s, *t;
	float *slot;
	if (!vm) {
		return;
	}
	f = *(int *)&vm->globals[OFS_PARM0];
	s = CSQCVM_Str(OFS_PARM0 + 3);
	cf = (vm->argc > 2) ? *(int *)&vm->globals[OFS_PARM0 + 6] : CSQC_Client_FieldOfs(vm, CSQC_FLD_CHAIN);
	if (f < 0 || f >= vm->progs->entityfields || cf < 0 || cf >= vm->progs->entityfields) {
		CSQC_Client_Abort("PF_FindChain: bad field reference");
		return;
	}
	prev = 0;
	for (e = CSQC_Client_EntSpawnBase(); e < vm->num_edicts; e++) {
		if (!CSQC_Client_EntUsed(e)) {
			continue;
		}
		slot = CSQC_Ent_Slot(vm, e);
		if (!slot) {
			continue;
		}
		t = CSQC_Client_GetString(vm, *(int *)&slot[f]);
		if (!t) {
			continue;
		}
		if (strcmp(t, s ? s : "")) {
			continue;
		}
		*(int *)&slot[cf] = prev * vm->edict_size;
		prev = e;
	}
	CSQC_Ret_Entity(vm, prev);
}

/* entity(.float fld, float match, .entity chainfield) findchainfloat = #403 */
static void CSQC_Findchainfloat(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int f, cf, e, prev;
	float match, *slot;
	if (!vm) {
		return;
	}
	f = *(int *)&vm->globals[OFS_PARM0];
	match = vm->globals[OFS_PARM0 + 3];
	cf = (vm->argc > 2) ? *(int *)&vm->globals[OFS_PARM0 + 6] : CSQC_Client_FieldOfs(vm, CSQC_FLD_CHAIN);
	if (f < 0 || f >= vm->progs->entityfields || cf < 0 || cf >= vm->progs->entityfields) {
		CSQC_Client_Abort("PF_FindChain: bad field reference");
		return;
	}
	prev = 0;
	for (e = CSQC_Client_EntSpawnBase(); e < vm->num_edicts; e++) {
		if (!CSQC_Client_EntUsed(e)) {
			continue;
		}
		slot = CSQC_Ent_Slot(vm, e);
		if (!slot) {
			continue;
		}
		// float equality (not int bits, unlike findfloat #98).
		if (slot[f] != match) {
			continue;
		}
		*(int *)&slot[cf] = prev * vm->edict_size;
		prev = e;
	}
	CSQC_Ret_Entity(vm, prev);
}

/* entity(entity start, .float fld, float match) findflags = #449 */
static void CSQC_Findflags(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int e, f;
	float match, *slot;
	if (!vm) {
		return;
	}
	e = CSQC_Ent_Of(vm, OFS_PARM0);
	f = *(int *)&vm->globals[OFS_PARM0 + 3];
	match = vm->globals[OFS_PARM0 + 6];
	if (f < 0 || f >= vm->progs->entityfields) {
		CSQC_Client_Abort("PF_FindFlags: bad field reference");
		return;
	}
	if (e < 0) {
		e = 0;
	}
	for (e++; e < vm->num_edicts; e++) {
		if (!CSQC_Client_EntUsed(e)) {
			continue;
		}
		slot = CSQC_Ent_Slot(vm, e);
		if (!slot) {
			continue;
		}
		if ((int)slot[f] & (int)match) {
			CSQC_Ret_Entity(vm, e);
			return;
		}
	}
	CSQC_Ret_Entity(vm, 0);
}

/* entity(.float fld, float match, .entity chainfield) findchainflags = #450 */
static void CSQC_Findchainflags(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int f, cf, e, prev;
	float match, *slot;
	if (!vm) {
		return;
	}
	f = *(int *)&vm->globals[OFS_PARM0];
	match = vm->globals[OFS_PARM0 + 3];
	cf = (vm->argc > 2) ? *(int *)&vm->globals[OFS_PARM0 + 6] : CSQC_Client_FieldOfs(vm, CSQC_FLD_CHAIN);
	if (f < 0 || f >= vm->progs->entityfields || cf < 0 || cf >= vm->progs->entityfields) {
		CSQC_Client_Abort("PF_FindChain: bad field reference");
		return;
	}
	prev = 0;
	for (e = CSQC_Client_EntSpawnBase(); e < vm->num_edicts; e++) {
		if (!CSQC_Client_EntUsed(e)) {
			continue;
		}
		slot = CSQC_Ent_Slot(vm, e);
		if (!slot) {
			continue;
		}
		if (!((int)slot[f] & (int)match)) {
			continue;
		}
		*(int *)&slot[cf] = prev * vm->edict_size;
		prev = e;
	}
	CSQC_Ret_Entity(vm, prev);
}

/* void() changeyaw = #49 - no-op (no server physics) */
static void CSQC_Changeyaw(void)
{
}

/*
 void(entity e) makestatic = #69
 Copies the edict's render state into a static entity drawn with the world every
 frame, then removes the edict. Module statics have their own list so they never
 reach the server-static paths (CL_ParseStatic's Host_Error on overflow, demo
 recording of statics, corona/QMB entity_id references). The list is capped at
 the server-static budget: when full the edict is still removed and a warning is
 printed once per map. Cleared by CSQCVM_ClearStatics on module unload, i.e. on
 every map change and disconnect.
*/
#define CSQC_MAX_STATICS	MAX_STATIC_ENTITIES
static entity_t csqc_statics[CSQC_MAX_STATICS];
static int csqc_num_statics;
static qbool csqc_statics_full_warned;

static void CSQC_Makestatic(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int e;

	if (!vm) {
		return;
	}
	e = CSQC_Ent_Of(vm, OFS_PARM0);
	if (e > 0 && CSQC_Client_EntUsed(e)) {
		if (csqc_num_statics >= CSQC_MAX_STATICS) {
			if (!csqc_statics_full_warned) {
				Con_Printf("CSQC: makestatic: too many static entities (max %d), dropping\n", CSQC_MAX_STATICS);
			}
			csqc_statics_full_warned = true;
		}
		else if (CSQC_Edict_To_Entity(vm, e, &csqc_statics[csqc_num_statics])) {
			entity_t *ent = &csqc_statics[csqc_num_statics++];
			float *f = CSQC_Ent_Ofs(vm, e, CSQC_Client_FieldOfs(vm, CSQC_FLD_LERPFRAC));

			// A static has no lerp history (the player-colormap lerp read
			// cl_entities at conversion time and would freeze mid-blend).
			// A module .frame2/.lerpfrac blend is kept.
			if (!f || f[0] == 0) {
				ent->oldframe = ent->frame;
				ent->framelerp = -1;
			}
		}
	}
	// Removed even when nothing was copied.
	CSQC_Client_EntFree(vm, e);
}

/*
 Draw the module statics. Called from the world traversal right after the engine
 statics are collected from the efrags (R_CreateWorldTextureChains), so they are
 drawn in every mode exactly like the server statics (engine-owned scene, CSQC
 takeover via #304 renderscene, and the takeover fallback), independent of the
 addentities mask. Culled like arena edicts; r_drawflame as in R_StoreEfrags.
*/
void CSQCVM_LinkStatics(void)
{
	extern cvar_t r_drawflame;
	int i;

	for (i = 0; i < csqc_num_statics; i++) {
		entity_t *ent = &csqc_statics[i];

		if ((ent->model->modhint == MOD_FLAME || ent->model->modhint == MOD_FLAME2) && !r_drawflame.integer) {
			continue;
		}
		if (!R_CSQC_EntityVisible(ent)) {
			continue;
		}
		CL_AddEntity(ent);
	}
}

void CSQCVM_ClearStatics(void)
{
	csqc_num_statics = 0;
	csqc_statics_full_warned = false;
}

/*
 string(entity e, string key) infokey = #80
 world -> serverkey; otherwise the player key of slot .entnum - 1 ("" for non-players).
*/
static void CSQC_Infokey(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *key = CSQCVM_Str(OFS_PARM0 + 3);
	char buf[64];
	const char *v;
	int e;
	if (!vm) {
		return;
	}
	if (!key) {
		key = "";
	}
	e = CSQC_Ent_Of(vm, OFS_PARM0);
	if (e == 0) {
		v = CSQC_Serverkey_Value(key, buf, sizeof(buf));
	}
	else {
		v = CSQC_Playerkey_Value(CSQC_Client_EntityEntNum(vm, e) - 1, key, buf, sizeof(buf));
	}
	CSQCVM_SetRetStr((char *)(v ? v : ""));
}

/* float(entity e) checkbottom = #40 - always 0 */
static void CSQC_Checkbottom(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN] = 0;
}

/* void(entity e) eprint = #31 - print the entity's fields to the console */
static void CSQC_Eprint(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int e, i, ofs;
	float *slot;
	if (!vm) {
		return;
	}
	e = CSQC_Ent_Of(vm, OFS_PARM0);
	slot = CSQC_Ent_Slot(vm, e);
	if (!slot) {
		Con_Printf("eprint: bad entity %d\n", e);
		return;
	}
	Con_Printf("eprint entity %d\n", e);
	for (i = 0; i < vm->progs->numfielddefs; i++) {
		char *fn = CSQC_Client_GetString(vm, vm->fielddefs[i].s_name);
		ofs = vm->fielddefs[i].ofs;
		if (!fn) {
			continue;
		}
		switch (vm->fielddefs[i].type) {
			case 1: /* ev_string */
				Con_Printf("  .%s = \"%s\"\n", fn,
					CSQC_Client_GetString(vm, *(int *)&slot[ofs]) ? CSQC_Client_GetString(vm, *(int *)&slot[ofs]) : "");
				break;
			case 2: /* ev_float */
				Con_Printf("  .%s = %g\n", fn, slot[ofs]);
				break;
			case 3: /* ev_vector */
				Con_Printf("  .%s = '%g %g %g'\n", fn, slot[ofs], slot[ofs + 1], slot[ofs + 2]);
				break;
			case 4: /* ev_entity */
			{
				// raw int bits (slot*edict_size), not a float
				int v = *(int *)&slot[ofs];
				Con_Printf("  .%s = ent %d\n", fn, (vm->edict_size > 0) ? v / vm->edict_size : v);
			} break;
		}
	}
}

/* void() coredump = #28 - module header + arena usage */
static void CSQC_Coredump(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	Con_Printf("coredump (CSQC): funcs %d globals %d fields %d pool_used %d\n",
		vm->progs->numfunctions, vm->progs->numglobals, vm->progs->numfielddefs,
		CSQC_Client_EntUsedCount());
}

/*
 World traces. traceline/tracebox also test the arena entities (CSQC_Trace_Ents).
 World and SOLID_BSP entities are traced on the hull chosen by box size; other
 entities are swept AABBs. Limitations: HITMODEL uses the bbox, brush-entity angles
 are ignored, TRIGGERS does not test brush triggers, no LAGGED, no plane for AABB
 hits.
*/

static trace_t CSQC_Trace_Fallback(vec3_t end)
{
	trace_t tr;
	memset(&tr, 0, sizeof(tr));
	tr.fraction = 1;
	VectorCopy(end, tr.endpos);
	return tr;
}

/*
 Hull for a box of the given size: width < 3 -> point hull,
 <= 32 -> player hull (HL crouch hull 3 when it exists and the box is short enough),
 wider -> shambler hull. Only maxs - mins matters for the choice; the trace is offset
 by clip_mins - mins, so the box's min corner rides on the hull's.
*/
static hull_t *CSQC_Choose_Hull(cmodel_t *cm, vec3_t mins, vec3_t maxs, vec3_t offset)
{
	hull_t *hull;
	float w = maxs[0] - mins[0];

	if (w < 3) {
		hull = &cm->hulls[0];
	}
	else if (w <= 32.1f) {
		if (maxs[2] - mins[2] < 54.1f && cm->hulls[3].clip_maxs[2] > 0) {
			hull = &cm->hulls[3];
		}
		else {
			hull = &cm->hulls[1];
		}
	}
	else {
		hull = &cm->hulls[2];
	}
	VectorSubtract(hull->clip_mins, mins, offset);
	return hull;
}

// Clip model of an inline ("*N") model in the sign-split index space, else NULL.
static cmodel_t *CSQC_Clipmodel_For_Index(int idx)
{
	const char *name;
	int num;

	if (!cl.clipmodels[1] || idx == 1) {
		return NULL;
	}
	if (idx > 1 && idx < MAX_MODELS) {
		return cl.clipmodels[idx];
	}
	name = CSQC_Client_ModelNameForIndex(idx);
	if (!name || name[0] != '*') {
		return NULL;
	}
	num = atoi(name + 1);
	if (num < 1 || num >= CM_NumInlineModels()) {
		return NULL;
	}
	return CM_InlineModel((char *)name);
}

// Box trace against a BSP clip model placed at origin. mins/maxs NULL = point.
static trace_t CSQC_Hull_Trace(cmodel_t *cm, vec3_t origin, vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end)
{
	static vec3_t zero = { 0, 0, 0 };
	hull_t *hull;
	trace_t tr;
	vec3_t offset, sl, el;

	hull = CSQC_Choose_Hull(cm, mins ? mins : zero, maxs ? maxs : zero, offset);
	VectorAdd(offset, origin, offset);
	VectorSubtract(start, offset, sl);
	VectorSubtract(end, offset, el);
	tr = CM_HullTrace(hull, sl, el);
	if (tr.fraction == 1 && !tr.allsolid) {
		VectorCopy(end, tr.endpos);
	}
	else {
		VectorAdd(tr.endpos, offset, tr.endpos);
	}
	return tr;
}

static trace_t CSQC_World_Trace(vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end)
{
	static vec3_t zero = { 0, 0, 0 };

	if (!cl.clipmodels[1]) {
		return CSQC_Trace_Fallback(end);
	}
	return CSQC_Hull_Trace(cl.clipmodels[1], zero, start, mins, maxs, end);
}

/*
 Q1 CONTENTS_* (-1..-7) -> FTECONTENTS bits, indexed by -1-q1.
*/
static const unsigned int s_q1_to_fte_contents[7] = {
	0x00000000u, // CONTENTS_EMPTY (-1)
	0x00000001u, // CONTENTS_SOLID (-2)
	0x00000020u, // CONTENTS_WATER (-3)
	0x00000010u, // CONTENTS_SLIME (-4)
	0x00000008u, // CONTENTS_LAVA  (-5)
	0x80000000u, // CONTENTS_SKY   (-6)
	0x00000001u  // STRIPPED       (-7)
};

static void CSQC_Store_Trace(pr1vm_t *vm, trace_t *tr)
{
	int o;
	// Global offsets are resolved once at load.
	if ((o = CSQC_Client_TraceGlobal(vm, CSQC_TRACEG_FRACTION)) >= 0) {
		vm->globals[o] = tr->fraction;
	}
	if ((o = CSQC_Client_TraceGlobal(vm, CSQC_TRACEG_ALLSOLID)) >= 0) {
		vm->globals[o] = tr->allsolid;
	}
	if ((o = CSQC_Client_TraceGlobal(vm, CSQC_TRACEG_STARTSOLID)) >= 0) {
		vm->globals[o] = tr->startsolid;
	}
	if ((o = CSQC_Client_TraceGlobal(vm, CSQC_TRACEG_INOPEN)) >= 0) {
		vm->globals[o] = tr->inopen;
	}
	if ((o = CSQC_Client_TraceGlobal(vm, CSQC_TRACEG_INWATER)) >= 0) {
		vm->globals[o] = tr->inwater;
	}
	if ((o = CSQC_Client_TraceGlobal(vm, CSQC_TRACEG_PLANE_DIST)) >= 0) {
		vm->globals[o] = tr->plane.dist;
	}
	if ((o = CSQC_Client_TraceGlobal(vm, CSQC_TRACEG_ENDPOS)) >= 0) {
		vm->globals[o] = tr->endpos[0];
		vm->globals[o + 1] = tr->endpos[1];
		vm->globals[o + 2] = tr->endpos[2];
	}
	if ((o = CSQC_Client_TraceGlobal(vm, CSQC_TRACEG_PLANE_NORMAL)) >= 0) {
		vm->globals[o] = tr->plane.normal[0];
		vm->globals[o + 1] = tr->plane.normal[1];
		vm->globals[o + 2] = tr->plane.normal[2];
	}
	if ((o = CSQC_Client_TraceGlobal(vm, CSQC_TRACEG_ENT)) >= 0) {
		*(int *)&vm->globals[o] = (tr->e.entnum > 0) ? tr->e.entnum * vm->edict_size : 0;
	}
	// trace_networkentity: the server entity number of the hit entity (0 for
	// world and module-spawned entities), not an arena slot.
	if ((o = CSQC_Client_TraceGlobal(vm, CSQC_TRACEG_NETWORKENTITY)) >= 0) {
		vm->globals[o] = (float)CSQC_Client_EntityEntNum(vm, tr->e.entnum);
	}
	// trace_endcontents (FTECONTENTS bits). World: the leaf contents at endpos.
	// A networked brush entity: its Q1 contents from .skin, or SOLID for a plain
	// SOLID_BSP. Module-spawned entities have no contents (0).
	if ((o = CSQC_Client_TraceGlobal(vm, CSQC_TRACEG_ENDCONTENTS)) >= 0) {
		unsigned int c = 0u;
		if (tr->e.entnum > 0 && vm->game_edicts && vm->edict_size > 0) {
			int ssqc = CSQC_Client_EntityEntNum(vm, tr->e.entnum);
			if (ssqc > 0) {
				int fskin = CSQC_Client_FieldOfs(vm, CSQC_FLD_SKIN);
				int fsol = CSQC_Client_FieldOfs(vm, CSQC_FLD_SOLID);
				float *base = (float *)((byte *)vm->game_edicts + (size_t)tr->e.entnum * vm->edict_size);
				int skin = (fskin >= 0) ? (int)base[fskin] : 0;
				int sol = (fsol >= 0) ? (int)base[fsol] : 0;
				if (skin <= -1 && skin >= -6) {
					c = s_q1_to_fte_contents[-1 - skin];
				}
				else if (sol == CSQC_SOLID_BSP) {
					c = 0x00000001u; // FTECONTENTS_SOLID
				}
			}
		}
		else if (cl.clipmodels[1]) {
			hull_t *h = &cl.clipmodels[1]->hulls[0];
			int q1 = CM_HullPointContents(h, h->firstclipnode, tr->endpos);
			if (q1 >= -6 && q1 <= -1) {
				c = s_q1_to_fte_contents[-1 - q1];
			}
		}
		vm->globals[o] = (float)c;
	}
}

/* Segment vs AABB (slab); returns t in [0,1], false if no intersection. */
static qbool CSQC_Ray_Aabb(vec3_t start, vec3_t dir, vec3_t bmin, vec3_t bmax, float *tout)
{
	float tmin = 0, tmax = 1;
	int i;
	for (i = 0; i < 3; i++) {
		float d = dir[i];
		float t1, t2, tmp;
		if (d > -1e-8 && d < 1e-8) {
			if (start[i] < bmin[i] || start[i] > bmax[i]) {
				return false;
			}
			continue;
		}
		t1 = (bmin[i] - start[i]) / d;
		t2 = (bmax[i] - start[i]) / d;
		if (t1 > t2) {
			tmp = t1;
			t1 = t2;
			t2 = tmp;
		}
		if (t1 > tmin) {
			tmin = t1;
		}
		if (t2 < tmax) {
			tmax = t2;
		}
		if (tmin > tmax) {
			return false;
		}
	}
	*tout = tmin;
	return (tmin >= 0 && tmin <= 1);
}

/*
 Trace the arena entities after the world trace and take any closer hit. Brush
 entities use their submodel hull; others are AABBs (no movetype semantics).
 moveflags is the MOVE_* mask; forent, its .owner and entities owned by forent
 are not hit. boxmin/boxmax != NULL (tracebox) expands each entity AABB by the box.
*/

static void CSQC_Trace_Ents(pr1vm_t *vm, vec3_t start, vec3_t end,
	int moveflags, int forent, vec3_t boxmin, vec3_t boxmax, trace_t *tr)
{
	vec3_t dir, bmin, bmax;
	float t;
	int e, i;
	int ofs_o, ofs_mn, ofs_mx, ofs_sol, ofs_fl, ofs_own, ofs_mi;
	qbool nomon, everything, triggers, missile;
	cmodel_t *cm;

	if (!vm || !vm->game_edicts || tr->fraction <= 0 || vm->edict_size <= 0) {
		return;
	}
	nomon = !!(moveflags & CSQC_MOVE_NOMONSTERS);
	everything = !!(moveflags & CSQC_MOVE_EVERYTHING);
	triggers = !!(moveflags & CSQC_MOVE_TRIGGERS);
	missile = !!(moveflags & CSQC_MOVE_MISSILE);
	if (forent < 0 || forent >= vm->num_edicts) {
		forent = 0; // world - no forent checks
	}

	ofs_o = CSQC_Client_FieldOfs(vm, CSQC_FLD_ORIGIN);
	ofs_mn = CSQC_Client_FieldOfs(vm, CSQC_FLD_MINS);
	ofs_mx = CSQC_Client_FieldOfs(vm, CSQC_FLD_MAXS);
	ofs_sol = CSQC_Client_FieldOfs(vm, CSQC_FLD_SOLID);
	ofs_fl = CSQC_Client_FieldOfs(vm, CSQC_FLD_FLAGS);
	ofs_own = CSQC_Client_FieldOfs(vm, CSQC_FLD_OWNER);
	ofs_mi = CSQC_Client_FieldOfs(vm, CSQC_FLD_MODELINDEX);
	if (ofs_o < 0 || ofs_mn < 0 || ofs_mx < 0) {
		return; // module has no geometry fields - entity layer unavailable
	}

	for (i = 0; i < 3; i++) {
		dir[i] = end[i] - start[i];
	}

	for (e = 1; e < vm->num_edicts; e++) {
		float *base, *org, *mn, *mx;
		int solf, flf;
		float inflate = 0;

		if (!CSQC_Client_EntUsed(e)) {
			continue;
		}
		base = (float *)((byte *)vm->game_edicts + (size_t)e * vm->edict_size);
		org = base + ofs_o;
		mn = base + ofs_mn;
		mx = base + ofs_mx;

		if (e == forent) {
			continue;
		}
		if (ofs_own >= 0) {
			// .owner is raw int bits (slot*edict_size), not a float.
			int own;
			own = *(int *)&base[ofs_own];
			if (own != 0 && own / vm->edict_size == forent) {
				continue; // ent whose owner == forent
			}
		}
		if (forent > 0 && ofs_own >= 0) {
			float *fbase = (float *)((byte *)vm->game_edicts + (size_t)forent * vm->edict_size);
			int fo = *(int *)&fbase[ofs_own];
			if (fo != 0 && fo / vm->edict_size == e) {
				continue; // forent.owner == ent
			}
		}

		solf = (ofs_sol >= 0) ? (int)base[ofs_sol] : CSQC_SOLID_NOT;
		flf = (ofs_fl >= 0) ? (int)base[ofs_fl] : 0;

		// SOLID_NOT is never hit; a trigger only with MOVE_TRIGGERS/MOVE_EVERYTHING
		// and FL_FINDABLE_NONSOLID.
		if (solf == CSQC_SOLID_NOT) {
			continue;
		}
		if (solf == CSQC_SOLID_TRIGGER) {
			if (!(flf & CSQC_FL_FINDABLE_NONSOLID)) {
				continue;
			}
			if (!everything && !triggers) {
				continue;
			}
		}
		// MOVE_NOMONSTERS keeps SOLID_BSP.
		if (nomon && solf != CSQC_SOLID_BSP) {
			continue;
		}
		// MOVE_MISSILE: monsters are inflated by 15 units.
		if (missile && (flf & CSQC_FL_MONSTER)) {
			inflate = 15;
		}

		// Brush entities clip against their submodel's hull (angles ignored).
		if (solf == CSQC_SOLID_BSP && ofs_mi >= 0 && (cm = CSQC_Clipmodel_For_Index((int)base[ofs_mi])) != NULL) {
			trace_t bt = CSQC_Hull_Trace(cm, org, start, boxmin, boxmax, end);

			if (bt.fraction >= tr->fraction && !bt.startsolid) {
				continue;
			}
			tr->fraction = bt.fraction;
			VectorCopy(bt.endpos, tr->endpos);
			tr->plane = bt.plane;
			tr->allsolid = bt.allsolid;
			tr->startsolid = bt.startsolid;
			tr->e.entnum = e;
			continue;
		}

		// Minkowski sum of the entity box and the trace box.
		for (i = 0; i < 3; i++) {
			bmin[i] = org[i] + mn[i] - inflate - (boxmax ? boxmax[i] : 0);
			bmax[i] = org[i] + mx[i] + inflate - (boxmin ? boxmin[i] : 0);
		}
		if (!CSQC_Ray_Aabb(start, dir, bmin, bmax, &t)) {
			continue;
		}
		if (t > tr->fraction) {
			continue;
		}
		tr->fraction = t;
		for (i = 0; i < 3; i++) {
			tr->endpos[i] = start[i] + dir[i] * t;
		}
		tr->e.entnum = e;
		tr->allsolid = false;
		tr->startsolid = false;
	}
}

/* void(vector v1, vector v2, float flags, entity forent) traceline = #16 */
static void CSQC_Traceline(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	trace_t tr;
	int moveflags, forent;
	if (!vm) {
		return;
	}
	moveflags = (int)vm->globals[OFS_PARM0 + 6];
	forent = CSQC_Ent_Of(vm, OFS_PARM0 + 9);
	tr = CSQC_World_Trace(&vm->globals[OFS_PARM0], NULL, NULL, &vm->globals[OFS_PARM0 + 3]);
	CSQC_Trace_Ents(vm, &vm->globals[OFS_PARM0], &vm->globals[OFS_PARM0 + 3], moveflags, forent, NULL, NULL, &tr);
	CSQC_Store_Trace(vm, &tr);
}

/* void(vector v1, vector mins, vector maxs, vector v2, float flags, entity forent) tracebox = #90 */
static void CSQC_Tracebox(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	trace_t tr;
	int moveflags, forent;
	if (!vm) {
		return;
	}
	moveflags = (int)vm->globals[OFS_PARM0 + 12];
	forent = CSQC_Ent_Of(vm, OFS_PARM0 + 15);
	tr = CSQC_World_Trace(&vm->globals[OFS_PARM0],
		&vm->globals[OFS_PARM0 + 3], &vm->globals[OFS_PARM0 + 6],
		&vm->globals[OFS_PARM0 + 9]);
	CSQC_Trace_Ents(vm, &vm->globals[OFS_PARM0], &vm->globals[OFS_PARM0 + 9],
		moveflags, forent, &vm->globals[OFS_PARM0 + 3], &vm->globals[OFS_PARM0 + 6], &tr);
	CSQC_Store_Trace(vm, &tr);
}

/* float(vector org) pointcontents = #41 */
static void CSQC_Pointcontents(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	hull_t *hull;
	if (!vm) {
		return;
	}
	if (!cl.clipmodels[1]) {
		vm->globals[OFS_RETURN] = CONTENTS_EMPTY;
		return;
	}
	hull = &cl.clipmodels[1]->hulls[0];
	vm->globals[OFS_RETURN] = CM_HullPointContents(hull, hull->firstclipnode, &vm->globals[OFS_PARM0]);
}
/*
 Monster movement (#32 walkmove, #34 droptofloor) on the arena edict in `self`,
 ported from SV_movestep / SV_CheckBottom. Moves clip against the world and the
 arena entities (MOVE_NORMAL, the mover itself excluded) with the mover's box.
*/
#define CSQC_STEPSIZE	18

typedef struct
{
	pr1vm_t *vm;
	int e;
	float *org, *mins, *maxs, *flags;
	float *ground; // .groundentity, optional
	float *enemy;  // .enemy, optional
} csqc_mover_t;

static qbool CSQC_Mover_Get(pr1vm_t *vm, csqc_mover_t *m)
{
	int ofs = CSQC_Client_SelfGlobal();

	memset(m, 0, sizeof(*m));
	if (ofs < 0) {
		return false;
	}
	m->vm = vm;
	m->e = CSQC_Client_EntNum(vm, *(int *)&vm->globals[ofs]);
	if (m->e <= 0) {
		return false;
	}
	m->org = CSQC_Ent_Ofs(vm, m->e, CSQC_Client_FieldOfs(vm, CSQC_FLD_ORIGIN));
	m->mins = CSQC_Ent_Ofs(vm, m->e, CSQC_Client_FieldOfs(vm, CSQC_FLD_MINS));
	m->maxs = CSQC_Ent_Ofs(vm, m->e, CSQC_Client_FieldOfs(vm, CSQC_FLD_MAXS));
	m->flags = CSQC_Ent_Ofs(vm, m->e, CSQC_Client_FieldOfs(vm, CSQC_FLD_FLAGS));
	m->ground = CSQC_Ent_Ofs(vm, m->e, CSQC_Client_FieldOfs(vm, CSQC_FLD_GROUNDENTITY));
	m->enemy = CSQC_Ent_Ofs(vm, m->e, CSQC_Client_FieldOfs(vm, CSQC_FLD_ENEMY));
	return m->org && m->mins && m->maxs && m->flags;
}

static trace_t CSQC_Mover_Trace(csqc_mover_t *m, vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, int moveflags)
{
	trace_t tr = CSQC_World_Trace(start, mins, maxs, end);

	CSQC_Trace_Ents(m->vm, start, end, moveflags, m->e, mins, maxs, &tr);
	return tr;
}

static int CSQC_World_Contents(vec3_t p)
{
	hull_t *hull;

	if (!cl.clipmodels[1]) {
		return CONTENTS_EMPTY;
	}
	hull = &cl.clipmodels[1]->hulls[0];
	return CM_HullPointContents(hull, hull->firstclipnode, p);
}

static void CSQC_Mover_Set_Ground(csqc_mover_t *m, trace_t *tr)
{
	if (m->ground) {
		*(int *)m->ground = (tr->e.entnum > 0) ? tr->e.entnum * m->vm->edict_size : 0;
	}
}

static qbool CSQC_Check_Bottom(csqc_mover_t *m)
{
	vec3_t mins, maxs, start, stop;
	trace_t tr;
	int x, y;
	float mid, bottom;

	VectorAdd(m->org, m->mins, mins);
	VectorAdd(m->org, m->maxs, maxs);

	// All four corners over solid world: done.
	start[2] = mins[2] - 1;
	for (x = 0; x <= 1; x++) {
		for (y = 0; y <= 1; y++) {
			start[0] = x ? maxs[0] : mins[0];
			start[1] = y ? maxs[1] : mins[1];
			if (CSQC_World_Contents(start) != CONTENTS_SOLID) {
				goto realcheck;
			}
		}
	}
	return true;

realcheck:
	// The midpoint must have ground within 2 steps, each corner within a step of it.
	start[2] = mins[2];
	start[0] = stop[0] = (mins[0] + maxs[0]) * 0.5f;
	start[1] = stop[1] = (mins[1] + maxs[1]) * 0.5f;
	stop[2] = start[2] - 2 * CSQC_STEPSIZE;
	tr = CSQC_Mover_Trace(m, start, NULL, NULL, stop, CSQC_MOVE_NOMONSTERS);
	if (tr.fraction == 1) {
		return false;
	}
	mid = bottom = tr.endpos[2];

	for (x = 0; x <= 1; x++) {
		for (y = 0; y <= 1; y++) {
			start[0] = stop[0] = x ? maxs[0] : mins[0];
			start[1] = stop[1] = y ? maxs[1] : mins[1];
			tr = CSQC_Mover_Trace(m, start, NULL, NULL, stop, CSQC_MOVE_NOMONSTERS);
			if (tr.fraction != 1 && tr.endpos[2] > bottom) {
				bottom = tr.endpos[2];
			}
			if (tr.fraction == 1 || mid - tr.endpos[2] > CSQC_STEPSIZE) {
				return false;
			}
		}
	}
	return true;
}

static qbool CSQC_Movestep(csqc_mover_t *m, vec3_t move, trace_t *tr)
{
	vec3_t oldorg, neworg, end;
	int flags = (int)*m->flags;
	int i;

	VectorCopy(m->org, oldorg);
	VectorAdd(m->org, move, neworg);

	// Flyers and swimmers do not step: one try with vertical motion toward the
	// enemy, then one without.
	if (flags & (CSQC_FL_SWIM | CSQC_FL_FLY)) {
		int enemy = m->enemy ? CSQC_Client_EntNum(m->vm, *(int *)m->enemy) : 0;
		float *eorg = enemy > 0 ? CSQC_Ent_Ofs(m->vm, enemy, CSQC_Client_FieldOfs(m->vm, CSQC_FLD_ORIGIN)) : NULL;

		for (i = 0; i < 2; i++) {
			VectorAdd(m->org, move, neworg);
			if (i == 0 && eorg) {
				float dz = m->org[2] - eorg[2];
				if (dz > 40) {
					neworg[2] -= 8;
				}
				if (dz < 30) {
					neworg[2] += 8;
				}
			}
			*tr = CSQC_Mover_Trace(m, m->org, m->mins, m->maxs, neworg, 0);
			if (tr->fraction == 1) {
				if ((flags & CSQC_FL_SWIM) && CSQC_World_Contents(tr->endpos) == CONTENTS_EMPTY) {
					return false; // swimmer left the water
				}
				VectorCopy(tr->endpos, m->org);
				return true;
			}
			if (!eorg) {
				break;
			}
		}
		return false;
	}

	// Push down from a step above the wished position.
	neworg[2] += CSQC_STEPSIZE;
	VectorCopy(neworg, end);
	end[2] -= CSQC_STEPSIZE * 2;

	*tr = CSQC_Mover_Trace(m, neworg, m->mins, m->maxs, end, 0);
	if (tr->allsolid) {
		return false;
	}
	if (tr->startsolid) {
		neworg[2] -= CSQC_STEPSIZE;
		*tr = CSQC_Mover_Trace(m, neworg, m->mins, m->maxs, end, 0);
		if (tr->allsolid || tr->startsolid) {
			return false;
		}
	}
	if (tr->fraction == 1) {
		// Ground pulled out from under a partially grounded mover: fall.
		if (flags & CSQC_FL_PARTIALGROUND) {
			VectorAdd(m->org, move, m->org);
			*m->flags = (float)(flags & ~CSQC_FL_ONGROUND);
			return true;
		}
		return false; // walked off an edge
	}

	VectorCopy(tr->endpos, m->org);
	if (!CSQC_Check_Bottom(m)) {
		if (flags & CSQC_FL_PARTIALGROUND) {
			return true; // keep correcting with the floor mostly gone
		}
		VectorCopy(oldorg, m->org);
		return false;
	}

	if (flags & CSQC_FL_PARTIALGROUND) {
		*m->flags = (float)(flags & ~CSQC_FL_PARTIALGROUND);
	}
	CSQC_Mover_Set_Ground(m, tr);
	return true;
}

/* float(float yaw, float dist, optional float settrace) walkmove = #32 */
static void CSQC_Walkmove(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	csqc_mover_t m;
	vec3_t move;
	trace_t tr;
	float yaw;
	qbool ok;

	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN] = 0;
	if (!CSQC_Mover_Get(vm, &m)) {
		return;
	}
	if (!((int)*m.flags & (CSQC_FL_ONGROUND | CSQC_FL_FLY | CSQC_FL_SWIM))) {
		return;
	}

	yaw = vm->globals[OFS_PARM0] * (M_PI / 180.0);
	move[0] = cos(yaw) * vm->globals[OFS_PARM1];
	move[1] = sin(yaw) * vm->globals[OFS_PARM1];
	move[2] = 0;

	memset(&tr, 0, sizeof(tr));
	tr.fraction = 1;
	ok = CSQC_Movestep(&m, move, &tr);
	if (ok) {
		CSQC_Client_LinkBounds(vm, m.e);
	}
	if (vm->argc >= 3 && vm->globals[OFS_PARM2]) {
		CSQC_Store_Trace(vm, &tr);
	}
	vm->globals[OFS_RETURN] = ok;
}

/* float() droptofloor = #34 */
static void CSQC_Droptofloor(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	csqc_mover_t m;
	vec3_t end;
	trace_t tr;

	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN] = 0;
	if (!CSQC_Mover_Get(vm, &m)) {
		return;
	}
	VectorCopy(m.org, end);
	end[2] -= 512;
	tr = CSQC_Mover_Trace(&m, m.org, m.mins, m.maxs, end, 0);
	if (tr.fraction == 1 || tr.allsolid) {
		return;
	}
	VectorCopy(tr.endpos, m.org);
	CSQC_Client_LinkBounds(vm, m.e);
	*m.flags = (float)((int)*m.flags | CSQC_FL_ONGROUND);
	CSQC_Mover_Set_Ground(&m, &tr);
	vm->globals[OFS_RETURN] = 1;
}

/* void(float step) movetogoal = #67 - no-op (the module has no goalentity field) */
static void CSQC_Movetogoal(void)
{
}

// tokenizebyseparator: split by any of up to 7 separators (empty tokens are kept),
// tokens are the raw substrings.
static void CSQC_Tok_Sep_Fill(const char *s, const char *sep[], int nsep)
{
	int i, len, tokstart, n, si;
	int seplen[7];

	s_tokn = 0;
	if (!s || !*s) {
		return;
	}
	len = (int)strlen(s);
	for (si = 0; si < nsep && si < 7; si++) {
		// A NULL (invalid string) or empty separator is ignored; an empty one would
		// match at every position and never advance.
		seplen[si] = (sep[si] && sep[si][0]) ? (int)strlen(sep[si]) : 0;
	}
	i = 0;
	tokstart = 0;
	n = 0;
	for (;;) {
		int found = -1;
		if (i >= len) {
			found = -2;   // end of string
		}
		else {
			for (si = 0; si < nsep && si < 7; si++) {
				if (seplen[si] > 0 && !strncmp(s + i, sep[si], seplen[si])) {
					found = si;
					break;
				}
			}
		}
		if (found >= 0) {
			if (n < CSQC_TOK_MAX) {
				s_tok_start[n] = tokstart;
				s_tok_end[n] = i;
				n++;
			}
			i += seplen[found];
			tokstart = i;
			if (n >= CSQC_TOK_MAX) {
				break;
			}
		}
		else if (found == -2) {
			if (n < CSQC_TOK_MAX) {
				s_tok_start[n] = tokstart;
				s_tok_end[n] = len;
				n++;
			}
			break;
		}
		else {
			i++;
		}
	}
	s_tokn = n;
	for (i = 0; i < s_tokn; i++) {
		int tlen = s_tok_end[i] - s_tok_start[i];

		if (tlen < 0) {
			tlen = 0;
		}
		if (tlen > CSQC_TOK_SRC - 1) {
			tlen = CSQC_TOK_SRC - 1;
		}
		memcpy(s_tok_text[i], s + s_tok_start[i], tlen);
		s_tok_text[i][tlen] = 0;
	}
}

/* string(float uselocaltime, string format, ...) strftime = #478 */
static void CSQC_Strftime(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	const char *p;
	char buf[2048];
	int i, o = 0;
	time_t t;
	struct tm *tm;
	if (!vm) {
		return;
	}
	t = time(NULL);
	tm = (vm->globals[OFS_PARM0] != 0) ? localtime(&t) : gmtime(&t);
	for (i = 1; i < vm->argc && o < (int)sizeof(buf) - 1; i++) {
		p = CSQC_Client_GetString(vm, *(int *)&vm->globals[OFS_PARM0 + i * 3]);
		if (!p) {
			continue;
		}
		// %R/%F are missing from msvc's strftime
		if (!strcmp(p, "%R")) {
			p = "%H:%M";
		}
		else if (!strcmp(p, "%F")) {
			p = "%Y-%m-%d";
		}
		o += snprintf(buf + o, sizeof(buf) - o, "%s", p);
	}
	if (!o) {
		CSQCVM_SetRetStr("");
		return;
	}
	{
		char out[512];
		if (strftime(out, sizeof(out), buf, tm)) {
			CSQCVM_SetRetStr(out);
		}
		else {
			CSQCVM_SetRetStr(buf); // invalid format - return as is
		}
	}
}

/* float(string str) tokenize_console = #514 */
static void CSQC_Tokenize_Console(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN] = CSQC_Tokenize_Source(CSQCVM_Str(OFS_PARM0), false);
}

/* float(string s, string sep1, ...) tokenizebyseparator = #479 */
static void CSQC_Tokenizebyseparator(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *s;
	const char *sep[7];
	int nsep = 0, i;
	if (!vm) {
		return;
	}
	s = CSQCVM_Str(OFS_PARM0);
	for (i = 1; i < vm->argc && nsep < 7; i++) {
		sep[nsep++] = CSQCVM_Str(OFS_PARM0 + i * 3);
	}
	strlcpy(s_tok_src, s ? s : "", sizeof(s_tok_src));
	CSQC_Tok_Sep_Fill(s_tok_src, sep, nsep);
	vm->globals[OFS_RETURN] = s_tokn;
}

/* float(float idx) argv_start_index = #515 / argv_end_index = #516 */
static void CSQC_Argv_Start_Index(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int idx;
	if (!vm) {
		return;
	}
	idx = (int)vm->globals[OFS_PARM0];
	if (idx < 0) {
		idx += s_tokn;
	}
	if ((unsigned int)idx >= (unsigned int)s_tokn) {
		vm->globals[OFS_RETURN] = -1;
	}
	else {
		vm->globals[OFS_RETURN] = s_tok_start[idx];
	}
}
static void CSQC_Argv_End_Index(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int idx;
	if (!vm) {
		return;
	}
	idx = (int)vm->globals[OFS_PARM0];
	if (idx < 0) {
		idx += s_tokn;
	}
	if ((unsigned int)idx >= (unsigned int)s_tokn) {
		vm->globals[OFS_RETURN] = -1;
	}
	else {
		vm->globals[OFS_RETURN] = s_tok_end[idx];
	}
}

/*
 Input/keyboard/menu. Key numbers in and out are QC key codes. There are no bindmaps,
 modifiers or video mode enumeration; those builtins are no-ops or approximations.
*/

/* string(float keynum) getkeybind = #342 - bound command or "" */
static void CSQC_Getkeybind(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int keynum;
	char *b;
	if (!vm) {
		return;
	}
	keynum = CSQC_Client_QCToKeynum((int)vm->globals[OFS_PARM0]);
	if (keynum < 0 || keynum >= UNKNOWN + 256) {
		b = NULL;
	}
	else {
		b = keybindings[keynum];
	}
	// Users commonly keep rcon passwords in binds; never hand such a bind to the
	// untrusted module. The whole string is redacted.
	if (b && CSQC_BindSensitive(b)) {
		b = NULL;
	}
	CSQCVM_SetRetStr(b ? b : "");
}

/* void(float keynum, string binding, optional float bindmap) setkeybind = #630 */

static void CSQC_Setkeybind(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int keynum;
	char *binding;
	if (!vm) {
		return;
	}
	keynum = CSQC_Client_QCToKeynum((int)vm->globals[OFS_PARM0]);
	binding = CSQCVM_Str(OFS_PARM1);
	if (keynum >= 0 && keynum < UNKNOWN + 256) {
		// module binds are flagged so they are not saved.
		Key_SetBindingModule(keynum, binding ? binding : "");
	}
}

/* #520 keynumtostring_omgwtf / #609 keynumtostring_menu - same as #340 */
static void CSQC_Keynumtostring_Menu(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	CSQCVM_SetRetStr(Key_KeynumToString(CSQC_Client_QCToKeynum((int)vm->globals[OFS_PARM0])));
}

/* float(string key) stringtokeynum_menu = #614 - same as #341 */
static void CSQC_Stringtokeynum_Menu(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *name = CSQCVM_Str(OFS_PARM0);
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN] = (name && name[0]) ? CSQC_Client_KeynumToQC(Key_StringToKeynum(name)) : -1;
}

/*
 string(string command, optional float bindmap) findkeysforcommand = #521
 string(string command, optional float bindmap) findkeysforcommand_dp = #610
 QC key codes as ` 'code' 'code'...`, at most 16, padded with '-1' to at least two
 entries (modules test argv(0) == -1 for unbound).
*/
static void CSQC_Findkeysforcommand(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *cmd = CSQCVM_Str(OFS_PARM0);
	char buf[512];
	int i, n = 0, o = 0;
	if (!vm) {
		return;
	}
	buf[0] = 0;
	if (cmd && cmd[0]) {
		for (i = 0; i < UNKNOWN + 256 && n < 16; i++) {
			if (keybindings[i] && !strcmp(keybindings[i], cmd)) {
				o += snprintf(buf + o, sizeof(buf) - o, " '%d'", CSQC_Client_KeynumToQC(i));
				n++;
			}
		}
	}
	for (; n < 2; n++) {
		o += snprintf(buf + o, sizeof(buf) - o, " '-1'");
	}
	CSQCVM_SetRetStr(buf);
}

/* void(float trg) setmousetarget = #603 - no-op (cursor via #343) */
static void CSQC_Setmousetarget(void)
{
}

/* float() getmousetarget = #604 - 2 if the CSQC cursor is active, otherwise 1 */
static void CSQC_Getmousetarget(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN] = CSQC_Client_CSQCCursor() ? 2 : 1;
}

/* vector(float vidmode, optional float forfullscreen) getresolution = #608 -
   returns the current resolution (mode list is not enumerated) */
static void CSQC_Getresolution(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN + 0] = vid.width;
	vm->globals[OFS_RETURN + 1] = vid.height;
	vm->globals[OFS_RETURN + 2] = 0;
}

/* vector() getbindmaps = #631 - no bindmaps: (0,0,0) */
static void CSQC_Getbindmaps(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN + 0] = 0;
	vm->globals[OFS_RETURN + 1] = 0;
	vm->globals[OFS_RETURN + 2] = 0;
}

/* float(vector bindmaps) setbindmaps = #632 - no-op, returns 1 */
static void CSQC_Setbindmaps(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN] = 1;
}

/*
 void(vector origin, string sample, float volume, float attenuation,
      optional float pitchpct) pointsound = #483
 pitchpct is ignored: the mixer has no per-channel playback rate.
*/
static void CSQC_Pointsound(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float *org;
	char *sample;
	sfx_t *sfx;
	float pitchpct = 0;
	if (!vm) {
		return;
	}
	org = &vm->globals[OFS_PARM0];
	sample = CSQC_Client_GetString(vm, *(int *)&vm->globals[OFS_PARM0 + 3]);
	if (vm->argc >= 5) {
		pitchpct = vm->globals[OFS_PARM0 + 12] * 0.01f;
	}
	(void)pitchpct;
	if (!sample || !sample[0]) {
		return;
	}
	sfx = S_PrecacheSound(sample);
	if (sfx) {
		S_StartSound(0, 0, sfx, org, vm->globals[OFS_PARM0 + 6], vm->globals[OFS_PARM0 + 9]);
	}
}

/*
 void(vector origin, vector forward, vector right, vector up) setlistener = #351
 Audio listener used by S_Update while the module is active, instead of the
 engine's view.
*/
static void CSQC_Setlistener(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	CSQC_Client_SetListener(&vm->globals[OFS_PARM0 + 0], &vm->globals[OFS_PARM0 + 3],
		&vm->globals[OFS_PARM0 + 6], &vm->globals[OFS_PARM0 + 9]);
}
/*
 float(string modelname, float(float isnew) updatecallback, float flags) deltalisten = #371
 Registers a callback by modelindex, called on entity updates (the player_state
 bridge in CSQC_Client_DeltaPlayers). modelname="*" - all models; func 0 - removal;
 an invalid func index is a no-op that keeps the existing listener.
*/
static void CSQC_Deltalisten(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *model;
	int func;
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN] = 0;
	model = CSQC_Client_GetString(vm, *(int *)&vm->globals[OFS_PARM0]);
	func = *(int *)&vm->globals[OFS_PARM1];
	if (func < 0 || func >= vm->progs->numfunctions) {
		Con_Printf("PF_DeltaListen: Bad function index\n");
		return;
	}
	CSQC_Client_DeltaListen(model, func, (int)vm->globals[OFS_PARM2]);
}

/* #533 getsoundtime (-1) / #534 soundlength (0): no channel timings or sample length */
static void CSQC_Getsoundtime(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN] = -1;
}
static void CSQC_Soundlength(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN] = 0;
}

/*
 Entity reflection (#496-500) over vm->fielddefs[] (ofs in float words).
*/
static ddef_t *CSQC_Fielddef(pr1vm_t *vm, unsigned int fidx)
{
	if (!vm || !vm->fielddefs || !vm->progs) {
		return NULL;
	}
	if (fidx >= (unsigned int)vm->progs->numfielddefs) {
		return NULL;
	}
	return &vm->fielddefs[fidx];
}

/* float() numentityfields = #496 */
static void CSQC_Numentityfields(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm || !vm->progs) {
		return;
	}
	vm->globals[OFS_RETURN] = vm->progs->numfielddefs;
}

/* string(float fieldnum) entityfieldname = #497 */
static void CSQC_Entityfieldname(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	ddef_t *f;
	char *s;
	if (!vm) {
		return;
	}
	f = CSQC_Fielddef(vm, (unsigned int)vm->globals[OFS_PARM0]);
	if (!f) {
		CSQCVM_SetRetStr("");
		return;
	}
	s = CSQC_Client_GetString(vm, f->s_name);
	CSQCVM_SetRetStr(s ? s : "");
}

/* float(float fieldnum) entityfieldtype = #498 (full ddef_t.type) */
static void CSQC_Entityfieldtype(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	ddef_t *f;
	if (!vm) {
		return;
	}
	f = CSQC_Fielddef(vm, (unsigned int)vm->globals[OFS_PARM0]);
	vm->globals[OFS_RETURN] = f ? (float)f->type : 0;
}

/* string(float fieldnum, entity ent) getentityfieldstring = #499 */
static void CSQC_Getentityfieldstring(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	ddef_t *f;
	float *slot;
	int slotidx, type, ofs;
	char buf[512];
	if (!vm) {
		return;
	}
	f = CSQC_Fielddef(vm, (unsigned int)vm->globals[OFS_PARM0]);
	slotidx = CSQC_Ent_Of(vm, OFS_PARM1);
	if (!f || slotidx < 0) {
		CSQCVM_SetRetStr("");
		return;
	}
	slot = CSQC_Ent_Slot(vm, slotidx);
	if (!slot) {
		CSQCVM_SetRetStr("");
		return;
	}
	type = f->type & 0xff;
	ofs = f->ofs;
	switch (type) {
		case ev_string:
		{
			char *s = CSQC_Client_GetString(vm, *(int *)&slot[ofs]);
			CSQCVM_SetRetStr(s ? s : "");
			return;
		}
		case ev_vector:
			snprintf(buf, sizeof(buf), "%g %g %g", slot[ofs], slot[ofs + 1], slot[ofs + 2]);
			break;
		case ev_entity:
		{
			int v = *(int *)&slot[ofs];
			snprintf(buf, sizeof(buf), "entity %d", (vm->edict_size > 0) ? v / vm->edict_size : v);
			break;
		}
		case ev_float:
		default:
			CSQC_Q_Ftoa(buf, sizeof(buf), slot[ofs]);
			break;
	}
	CSQCVM_SetRetStr(buf);
}

/* float(float fieldnum, entity ent, string s) putentityfieldstring = #500 */
static void CSQC_Putentityfieldstring(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	ddef_t *f;
	float *slot;
	int slotidx, type, ofs;
	char *s = CSQCVM_Str(OFS_PARM2);
	if (!vm) {
		return;
	}
	f = CSQC_Fielddef(vm, (unsigned int)vm->globals[OFS_PARM0]);
	slotidx = CSQC_Ent_Of(vm, OFS_PARM1);
	vm->globals[OFS_RETURN] = 0;
	if (!f || slotidx < 0) {
		return;
	}
	slot = CSQC_Ent_Slot(vm, slotidx);
	if (!slot) {
		return;
	}
	type = f->type & 0xff;
	ofs = f->ofs;
	switch (type) {
		case ev_string:
			// An ordinary temp string: the GC keeps it alive while the field (or
			// any other global/field) references it.
			PR1VM_ClientSetString(vm, (string_t *)&slot[ofs], s);
			break;
		case ev_vector:
		{
			float v[3] = { 0, 0, 0 };
			int n = 0;
			if (s) {
				n = sscanf(s, "%f %f %f", &v[0], &v[1], &v[2]);
			}
			if (n > 0) {
				VectorCopy(v, &slot[ofs]);
			}
			else {
				return;
			}
			break;
		}
		case ev_float:
		default:
			slot[ofs] = (s && s[0]) ? (float)atof(s) : 0;
			break;
	}
	vm->globals[OFS_RETURN] = 1;
}

/*
 BSP surface queries. All no-ops: there is no brush-model geometry interface.
 Registered so modules do not hit "Bad builtin".
*/
static void CSQC_Bsp_Nop_Vec(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN + 0] = 0;
	vm->globals[OFS_RETURN + 1] = 0;
	vm->globals[OFS_RETURN + 2] = 0;
}

// float(string name, float mode) fopen = #110: the client has no file I/O, and -1 is the failure value.
static void CSQC_Fopen(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (vm) {
		vm->globals[OFS_RETURN] = -1;
	}
}

/*
 Introspection/console. No-ops: #391-#394 (there is a single console) and #605
 callfunction (re-entrant execution from a builtin is unsupported).
*/

/* float(string name) checkcommand = #294 - command 1, alias 2, cvar 3, else 0 */
static void CSQC_Checkcommand(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *name = CSQCVM_Str(OFS_PARM0);
	if (!vm) {
		return;
	}
	if (name && name[0] && Cmd_Exists(name)) {
		vm->globals[OFS_RETURN] = 1;
	}
	else if (name && name[0] && Cmd_FindAlias(name)) {
		vm->globals[OFS_RETURN] = 2;
	}
	else if (name && name[0] && Cvar_Find(name)) {
		vm->globals[OFS_RETURN] = 3;
	}
	else {
		vm->globals[OFS_RETURN] = 0;
	}
}

/* string(string s) argescape = #295 - wraps in quotes with escaping */
static void CSQC_Argescape(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	const char *s = CSQCVM_Str(OFS_PARM0);
	char buf[4096];
	int i, n, need = 0;
	if (!vm) {
		return;
	}
	s = s ? s : "";
	n = strlen(s);
	for (i = 0; i < n; i++) {
		if (s[i] == ' ' || s[i] == '\t' || s[i] == '"' || s[i] == '\\' || s[i] == ';') {
			need = 1;
			break;
		}
	}
	if (!need) {
		CSQCVM_SetRetStr((char *)s);
		return;
	}
	{
		char *d = buf;
		*d++ = '"';
		for (i = 0; i < n && d < buf + sizeof(buf) - 3; i++) {
			if (s[i] == '"' || s[i] == '\\') {
				*d++ = '\\';
			}
			*d++ = s[i];
		}
		*d++ = '"';
		*d = 0;
	}
	CSQCVM_SetRetStr(buf);
}

/* float(string name) isfunction = #607 - the function exists in the module */
static void CSQC_Isfunction(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	char *name = CSQCVM_Str(OFS_PARM0);
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN] = name && name[0] && PR1VM_FindFunction(vm, name);
}

/*
 void(vector anglechange, optional float seat) CL_RotateMoves = #638
 Rotates the angles of unacknowledged usercmds. A seat other than 0 returns 0;
 otherwise the return value is left unset.
*/
static void CSQC_Cl_Rotatemoves(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int seat;

	if (!vm) {
		return;
	}
	seat = (vm->argc > 1) ? (int)vm->globals[OFS_PARM1] : 0;
	if (seat != 0) {
		vm->globals[OFS_RETURN] = 0;
		return;
	}
	CSQC_Client_RotateMoves(&vm->globals[OFS_PARM0], seat);
}

/*
 Lights/decals/skins. All no-ops or approximations: there are no decal or skin
 file subsystems, readback pics or per-field dlight access. Registered so modules
 do not hit "Bad builtin".
*/
static void CSQC_Light_Nop_Ret0(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN] = 0;
}

/*
 entity(entity start, .float fld, float match) findfloat = #98
 Next used entity after start whose field holds the same 32-bit value (a bit
 comparison, so -0.0 != 0.0 and NaN matches itself); none - world (0).
*/
static void CSQC_Findfloat(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int e, f;
	float match, *slot;
	if (!vm) {
		return;
	}
	e = CSQC_Ent_Of(vm, OFS_PARM0);
	f = *(int *)&vm->globals[OFS_PARM0 + 3];
	match = vm->globals[OFS_PARM0 + 6];
	if (f < 0 || f >= vm->progs->entityfields) {
		CSQC_Client_Abort("PF_FindFloat: bad field reference");
		return;
	}
	if (e < 0) {
		e = 0;
	}
	for (e++; e < vm->num_edicts; e++) {
		if (!CSQC_Client_EntUsed(e)) {
			continue;
		}
		slot = CSQC_Ent_Slot(vm, e);
		if (!slot) {
			continue;
		}
		if (*(int *)&slot[f] == *(int *)&match) {
			CSQC_Ret_Entity(vm, e);
			return;
		}
	}
	CSQC_Ret_Entity(vm, 0);
}

/* vector(vector org) getlight = #92 - approximation: no static light sample -> 0 */
static void CSQC_Getlight_Approx(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	vm->globals[OFS_RETURN + 0] = 0;
	vm->globals[OFS_RETURN + 1] = 0;
	vm->globals[OFS_RETURN + 2] = 0;
}

/* no-op: #64 tracetoss, #240 checkpvs, #278 terrain_edit, #279 touchtriggers */
static void CSQC_Vmrest_Nop(void)
{
}

/*
 __variant(float entnum, float fieldnum) getentity = #504.
 Interpolated state of a networked entity by server number. Always writes 3
 return words (float fields use the first). Fields with no source return their
 default.
*/
static void CSQC_Getentity(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float out[3];
	if (!vm) {
		return;
	}
	CSQC_Client_GetEntity((int)vm->globals[OFS_PARM0], (int)vm->globals[OFS_PARM1], out);
	vm->globals[OFS_RETURN + 0] = out[0];
	vm->globals[OFS_RETURN + 1] = out[1];
	vm->globals[OFS_RETURN + 2] = out[2];
}

/* string(entity ent) etos = #65 - "entity <slot>" (arena slot, not server entnum) */
static void CSQC_Etos(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int slot;
	char buf[32];
	if (!vm) {
		return;
	}
	slot = CSQC_Ent_Of(vm, OFS_PARM0);
	snprintf(buf, sizeof(buf), "entity %d", slot > 0 ? slot : 0);
	CSQCVM_SetRetStr(buf);
}

/* void(string s, ...) print = #339 */
static void CSQC_Print(void)
{
	char *s = CSQCVM_VarString(0);
	if (s && s[0]) {
		Con_Printf("%s", s);
	}
}

/* void(string s, ...) cprint = #338 - centerprint */
static void CSQC_Cprint(void)
{
	char *s = CSQCVM_VarString(0);
	char buf[1024];
	int i, lines = 1;

	// SCR_CenterPrint does not bound its line count (oversized input overflows
	// scr_centerstring_lines[1024][41]); cap module input to 1023 bytes / 1000 lines.
	for (i = 0; s && s[i] && i < (int)sizeof(buf) - 1; i++) {
		if (s[i] == '\n' && ++lines > 1000) {
			break;
		}
		buf[i] = s[i];
	}
	buf[i] = 0;
	SCR_CenterPrint(buf);
}

/* float() isserver = #350 - 1 if the built-in server is running. Never returns 0.5
   (single-player vs. listen server is not distinguished). */
static void CSQC_Isserver(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
#ifndef CLIENTONLY
	vm->globals[OFS_RETURN] = sv.state != ss_dead;
#else
	vm->globals[OFS_RETURN] = 0; // this build has no server
#endif
}

/* float(entity ent) wasfreed = #353 */
static void CSQC_Wasfreed(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int slot;
	if (!vm) {
		return;
	}
	slot = CSQC_Ent_Of(vm, OFS_PARM0);
	vm->globals[OFS_RETURN] = slot > 0 && !CSQC_Client_EntUsed(slot);
}

/* float(entity ent) num_for_edict = #512 - arena slot index (inverse of #459) */
static void CSQC_Num_For_Edict(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	int slot;
	if (!vm) {
		return;
	}
	slot = CSQC_Ent_Of(vm, OFS_PARM0);
	vm->globals[OFS_RETURN] = slot > 0 ? slot : 0;
}

/*
 #63 changepitch (DP_QC_CHANGEPITCH).
 Moves self.angles[0] toward self.idealpitch by at most self.pitch_speed (wrapping
 at +/-180), then anglemods. The entity argument is ignored; the target is `self`.
*/
static void CSQC_Changepitch(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	float *ang, *idealp, *speedp;
	int selfofs, entnum, f_ang, f_ideal, f_speed;
	float current, ideal, speed, move;

	if (!vm) {
		return;
	}
	selfofs = CSQC_Client_SelfGlobal();
	f_ang = CSQC_Client_FieldOfs(vm, CSQC_FLD_ANGLES);
	f_ideal = CSQC_Client_FieldOfs(vm, CSQC_FLD_IDEALPITCH);
	f_speed = CSQC_Client_FieldOfs(vm, CSQC_FLD_PITCH_SPEED);
	if (selfofs < 0 || f_ang < 0 || f_ideal < 0 || f_speed < 0) {
		return;
	}
	entnum = CSQC_Client_EntNum(vm, *(int *)&vm->globals[selfofs]);
	ang = CSQC_Ent_Ofs(vm, entnum, f_ang);
	idealp = CSQC_Ent_Ofs(vm, entnum, f_ideal);
	speedp = CSQC_Ent_Ofs(vm, entnum, f_speed);
	if (!ang || !idealp || !speedp) {
		return;
	}

	current = CSQC_Anglemod_Value(ang[0]);
	ideal = idealp[0];
	speed = speedp[0];
	if (current == ideal) {
		return;
	}
	move = ideal - current;
	if (ideal > current) {
		if (move >= 180) {
			move = move - 360;
		}
	}
	else {
		if (move <= -180) {
			move = move + 360;
		}
	}
	if (move > 0) {
		if (move > speed) {
			move = speed;
		}
	}
	else {
		if (move < -speed) {
			move = -speed;
		}
	}
	ang[0] = CSQC_Anglemod_Value(current + move);
}

/*
 #639 digest_hex: hash the concatenation of args from parm1 into a lowercase-hex
 string. Supported names (exact uppercase match): "SHA1", "MD4", "CRC16" (bytes
 little-endian). Any other name returns "". bin2hex is not used: it is uppercase.
*/
/* MD4 context layout of md4.c, which has no header (it is shared with mvdsv).
   Keep in sync with md4.c; UINT4 there is unsigned int. */
typedef struct {
	unsigned int state[4];
	unsigned int count[2];
	unsigned char buffer[64];
} csqc_md4_ctx_t;

extern void MD4Init(csqc_md4_ctx_t *);
extern void MD4Update(csqc_md4_ctx_t *, unsigned char *, unsigned int);
extern void MD4Final(unsigned char[16], csqc_md4_ctx_t *);

static void CSQC_Digest_To_Hex(char *out, const unsigned char *digest, int len)
{
	static const char hex[] = "0123456789abcdef";
	int i;
	for (i = 0; i < len; i++) {
		out[i * 2 + 0] = hex[digest[i] >> 4];
		out[i * 2 + 1] = hex[digest[i] & 0xf];
	}
	out[i * 2] = 0;
}

static void CSQC_Digest_Hex(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	unsigned char digest[DIGEST_SIZE]; // SHA1 (20 bytes) is the largest
	char *name, *data, out[DIGEST_SIZE * 2 + 1];
	int len = 0;

	if (!vm) {
		return;
	}
	name = CSQCVM_Str(OFS_PARM0);
	data = CSQCVM_VarString(1);
	if (!name) {
		CSQCVM_SetRetStr("");
		return;
	}
	if (!strcmp(name, "SHA1")) {
		SHA1_CTX ctx;
		SHA1Init(&ctx);
		SHA1Update(&ctx, (unsigned char *)data, strlen(data));
		SHA1Final(digest, &ctx);
		len = DIGEST_SIZE;
	}
	else if (!strcmp(name, "MD4")) {
		csqc_md4_ctx_t ctx;
		MD4Init(&ctx);
		MD4Update(&ctx, (unsigned char *)data, (unsigned int)strlen(data));
		MD4Final(digest, &ctx);
		len = 16;
	}
	else if (!strcmp(name, "CRC16")) {
		unsigned short crc;
		int i;
		CRC_Init(&crc);
		for (i = 0; i < (int)strlen(data); i++) {
			CRC_ProcessByte(&crc, (unsigned char)data[i]);
		}
		crc = CRC_Value(crc);
		digest[0] = crc & 0xff;  // little-endian
		digest[1] = (crc >> 8) & 0xff;
		len = 2;
	}
	if (len) {
		CSQC_Digest_To_Hex(out, digest, len);
		CSQCVM_SetRetStr(out);
	}
	else {
		CSQCVM_SetRetStr("");
	}
}

/* #355 getentitytoken - not supported: returns "". */
static void CSQC_Nop_Str(void)
{
	pr1vm_t *vm = CSQCVM_Active();
	if (!vm) {
		return;
	}
	CSQCVM_SetRetStr("");
}

/*
=================
CSQC_Traceon / CSQC_Traceoff (#29/#30)

No-ops. The server PF_traceon/PF_traceoff set vm->trace, and the shared executor's
statement printer (PR_PrintStatement -> ED_GlobalAtOfs) walks the *server* progs /
pr_globaldefs, which are NULL on a client without a local server. Tracing is
therefore disabled for the client VM; a no-op is permitted.
 =================
*/
static void CSQC_Traceon(void)
{
}

static void CSQC_Traceoff(void)
{
}

void CSQCVM_RegisterBuiltins(pr1vm_t *vm)
{
	PR1VM_RegisterBuiltin(vm, 1, (builtin_t)CSQC_Makevectors); // #1 void() makevectors (QUAKE)

	// Pure float/vector server PF_* are reused (see the externs above). #7/#13/#51
	// have client versions: their CSQC return values and optional args differ.
	PR1VM_RegisterBuiltin(vm, 7, (builtin_t)CSQC_Random); // #7 float() random (QUAKE)
	PR1VM_RegisterBuiltin(vm, 9, (builtin_t)PF_normalize); // #9 vector(vector in) normalize (QUAKE)
	PR1VM_RegisterBuiltin(vm, 12, (builtin_t)PF_vlen); // #12 float(vector v) vlen (QUAKE)
	PR1VM_RegisterBuiltin(vm, 13, (builtin_t)CSQC_Vectoyaw); // #13 float(vector v) vectoyaw (QUAKE)
	PR1VM_RegisterBuiltin(vm, 29, (builtin_t)CSQC_Traceon); // #29 void() traceon (QUAKE)
	PR1VM_RegisterBuiltin(vm, 30, (builtin_t)CSQC_Traceoff); // #30 void() traceoff (QUAKE)
	PR1VM_RegisterBuiltin(vm, 36, (builtin_t)PF_rint); // #36 float(float f) rint (QUAKE)
	PR1VM_RegisterBuiltin(vm, 37, (builtin_t)PF_floor); // #37 float(float f) floor (QUAKE)
	PR1VM_RegisterBuiltin(vm, 38, (builtin_t)PF_ceil); // #38 float(float f) ceil (QUAKE)
	PR1VM_RegisterBuiltin(vm, 43, (builtin_t)PF_fabs); // #43 float(float f) fabs (QUAKE)
	PR1VM_RegisterBuiltin(vm, 51, (builtin_t)CSQC_Vectoangles); // #51 vector(vector v) vectoangles (QUAKE)
	PR1VM_RegisterBuiltin(vm, 60, (builtin_t)PF_sin); // #60 float(float angle) sin (DP_QC_SINCOSSQRTPOW)
	PR1VM_RegisterBuiltin(vm, 61, (builtin_t)PF_cos); // #61 float(float angle) cos (DP_QC_SINCOSSQRTPOW)
	PR1VM_RegisterBuiltin(vm, 62, (builtin_t)PF_sqrt); // #62 float(float value) sqrt (DP_QC_SINCOSSQRTPOW)
	PR1VM_RegisterBuiltin(vm, 94, (builtin_t)PF_min); // #94 float(float a, floats) min (DP_QC_MINMAXBOUND)
	PR1VM_RegisterBuiltin(vm, 95, (builtin_t)PF_max); // #95 float(float a, floats) max (DP_QC_MINMAXBOUND)
	PR1VM_RegisterBuiltin(vm, 96, (builtin_t)PF_bound); // #96 float(float minimum, float val, float maximum) bound (DP_QC_MINMAXBOUND)
	// #97 pow / #91 randomvec: the server bodies are static, so client copies.
	PR1VM_RegisterBuiltin(vm, 97, (builtin_t)CSQC_Pow); // #97 float(float base, float exp) pow (DP_QC_SINCOSSQRTPOW)
	PR1VM_RegisterBuiltin(vm, 91, (builtin_t)CSQC_Randomvec); // #91 vector() randomvec (DP_QC_RANDOMVEC)

	PR1VM_RegisterBuiltin(vm, 10, (builtin_t)CSQC_Error); // #10 void(string errortext) error (QUAKE)
	PR1VM_RegisterBuiltin(vm, 11, (builtin_t)CSQC_Objerror); // #11 void(string errortext) objerror (QUAKE)
	PR1VM_RegisterBuiltin(vm, 46, (builtin_t)CSQC_Localcmd); // #46 void(string str) localcmd (QUAKE)
	PR1VM_RegisterBuiltin(vm, 72, (builtin_t)CSQC_Cvar_Set); // #72 void(string cvarname, string valuetoset) cvar_set (QUAKE)
	PR1VM_RegisterBuiltin(vm, 93, (builtin_t)CSQC_Registercvar); // #93 void(string cvarname, string defaultvalue) registercvar (DP_QC_REGISTERCVAR)
	PR1VM_RegisterBuiltin(vm, 99, (builtin_t)CSQC_Checkextension); // #99 float(string extname) checkextension (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 231, (builtin_t)CSQC_Calltimeofday); // #231 void() calltimeofday

	PR1VM_RegisterBuiltin(vm, 27, (builtin_t)CSQC_Vtos); // #27 string(vector f) vtos (QUAKE)
	PR1VM_RegisterBuiltin(vm, 81, (builtin_t)CSQC_Stof); // #81 float(string s) stof (FRIK_FILE or QW_ENGINE)
	PR1VM_RegisterBuiltin(vm, 114, (builtin_t)CSQC_Strlen); // #114 float(string str) strlen (FRIK_FILE)
	PR1VM_RegisterBuiltin(vm, 116, (builtin_t)CSQC_Substring); // #116 string(string str, float start, float length) substring (FRIK_FILE)
	PR1VM_RegisterBuiltin(vm, 117, (builtin_t)CSQC_Stov); // #117 vector(string str) stov (FRIK_FILE)
	PR1VM_RegisterBuiltin(vm, 118, (builtin_t)CSQC_Strzone); // #118 string(string str) dupstring (FRIK_FILE)
	PR1VM_RegisterBuiltin(vm, 119, (builtin_t)CSQC_Strunzone); // #119 void(string str) freestring (FRIK_FILE)
	PR1VM_RegisterBuiltin(vm, 448, (builtin_t)CSQC_Cvar_String); // #448 string(float n) cvar_string (DP_QC_CVAR_STRING)

	PR1VM_RegisterBuiltin(vm, 6, (builtin_t)CSQC_Breakpoint); // #6 void() debugbreak (QUAKE)
	PR1VM_RegisterBuiltin(vm, 8, (builtin_t)CSQC_Sound); // #8 void(entity e, float chan, string samp, float vol, float atten) sound (QUAKE)
	PR1VM_RegisterBuiltin(vm, 19, (builtin_t)CSQC_Precache_Sound); // #19 void(string str) precache_sound (QUAKE)
	PR1VM_RegisterBuiltin(vm, 20, (builtin_t)CSQC_Precache_Model); // #20 void(string str) precache_model (QUAKE)
	PR1VM_RegisterBuiltin(vm, 35, (builtin_t)CSQC_Lightstyle); // #35 void(float lightstyle, string stylestring) lightstyle (QUAKE)
	PR1VM_RegisterBuiltin(vm, 48, (builtin_t)CSQC_Particle); // #48 void(vector org, vector dir, float colour, float count) particle (QUAKE)
	PR1VM_RegisterBuiltin(vm, 68, (builtin_t)CSQC_Precache_File); // #68 void(string s) precache_file (QUAKE)
	PR1VM_RegisterBuiltin(vm, 74, (builtin_t)CSQC_Ambientsound); // #74 void (vector pos, string samp, float vol, float atten) ambientsound (QUAKE)
	PR1VM_RegisterBuiltin(vm, 75, (builtin_t)CSQC_Precache_Model); // #75 void(string str) precache_model2 (QUAKE)
	PR1VM_RegisterBuiltin(vm, 76, (builtin_t)CSQC_Precache_Sound); // #76 void(string str) precache_sound2 (QUAKE)
	PR1VM_RegisterBuiltin(vm, 77, (builtin_t)CSQC_Precache_File); // #77 void(string str) precache_file2 (QUAKE)
	PR1VM_RegisterBuiltin(vm, 531, (builtin_t)CSQC_Setpause);

	PR1VM_RegisterBuiltin(vm, 2, (builtin_t)CSQC_Setorigin); // #2 void(entity e, vector org) setorigin (QUAKE)
	PR1VM_RegisterBuiltin(vm, 3, (builtin_t)CSQC_Setmodel); // #3 void(entity e, string modl) setmodel (QUAKE)
	PR1VM_RegisterBuiltin(vm, 4, (builtin_t)CSQC_Setsize); // #4 void(entity e, vector mins, vector maxs) setsize (QUAKE)
	PR1VM_RegisterBuiltin(vm, 14, (builtin_t)CSQC_Spawn); // #14 entity() spawn (QUAKE)
	PR1VM_RegisterBuiltin(vm, 15, (builtin_t)CSQC_Remove); // #15 void(entity e) remove (QUAKE)
	PR1VM_RegisterBuiltin(vm, 18, (builtin_t)CSQC_Find); // #18 entity(entity start, .string fld, string match) findstring (QUAKE)
	PR1VM_RegisterBuiltin(vm, 22, (builtin_t)CSQC_Findradius); // #22 entity(vector org, float rad) findradius (QUAKE)
	PR1VM_RegisterBuiltin(vm, 28, (builtin_t)CSQC_Coredump); // #28 void(void) coredump (QUAKE)
	PR1VM_RegisterBuiltin(vm, 31, (builtin_t)CSQC_Eprint); // #31 void(entity e) eprint (QUAKE)
	PR1VM_RegisterBuiltin(vm, 40, (builtin_t)CSQC_Checkbottom); // #40 float(entity e) checkbottom (QUAKE)
	PR1VM_RegisterBuiltin(vm, 47, (builtin_t)CSQC_Nextent); // #47 entity(entity e) nextent (QUAKE)
	PR1VM_RegisterBuiltin(vm, 49, (builtin_t)CSQC_Changeyaw); // #49 void() changeyaw (QUAKE)
	PR1VM_RegisterBuiltin(vm, 69, (builtin_t)CSQC_Makestatic); // #69 void(entity e) makestatic (QUAKE)
	PR1VM_RegisterBuiltin(vm, 80, (builtin_t)CSQC_Infokey); // #80 string(entity e, string keyname) infokey (QW_ENGINE)

	PR1VM_RegisterBuiltin(vm, 16, (builtin_t)CSQC_Traceline); // #16 void(vector v1, vector v2, float nomonst, entity forent) traceline (QUAKE)
	PR1VM_RegisterBuiltin(vm, 90, (builtin_t)CSQC_Tracebox); // #90 void(vector start, vector mins, vector maxs, vector end, float nomonsters, entity ent) tracebox
	PR1VM_RegisterBuiltin(vm, 41, (builtin_t)CSQC_Pointcontents); // #41 float(vector org) pointcontents (QUAKE)
	PR1VM_RegisterBuiltin(vm, 32, (builtin_t)CSQC_Walkmove); // #32 float(float yaw, float dist) walkmove (QUAKE)
	PR1VM_RegisterBuiltin(vm, 34, (builtin_t)CSQC_Droptofloor); // #34 float() droptofloor
	PR1VM_RegisterBuiltin(vm, 67, (builtin_t)CSQC_Movetogoal); // #67 void(float step) movetogoal (QUAKE)

	PR1VM_RegisterBuiltin(vm, 25, (builtin_t)CSQC_Dprint); // #25 void(string s, ...) dprint (QUAKE)
	PR1VM_RegisterBuiltin(vm, 26, (builtin_t)CSQC_Ftos); // #26 string(float f) ftos (QUAKE)
	PR1VM_RegisterBuiltin(vm, 45, (builtin_t)CSQC_Cvar); // #45 float(string cvarname) cvar (QUAKE)

	PR1VM_RegisterBuiltin(vm, 340, (builtin_t)CSQC_Keynumtostring); // #340 string(float keynum) keynumtostring (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 341, (builtin_t)CSQC_Stringtokeynum); // #341 float(string keyname) stringtokeynum (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 349, (builtin_t)CSQC_Isdemo); // #349 float() isdemo (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 354, (builtin_t)CSQC_Serverkey); // #354 string(string key) serverkey;
	PR1VM_RegisterBuiltin(vm, 343, (builtin_t)CSQC_Setcursormode); // #343 void(float usecursor, optional string cursorimage, optional vector hotspot, optional float scale) setcursormode
	PR1VM_RegisterBuiltin(vm, 344, (builtin_t)CSQC_Getmousepos); // #344 vector() getmousepos
	PR1VM_RegisterBuiltin(vm, 348, (builtin_t)CSQC_Getplayerkeyvalue); // #348 string(float playernum, string keyname) getplayerkeyvalue (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 346, (builtin_t)CSQC_Setsensitivityscaler); // #346 void(float sens) setsensitivityscaler (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 345, (builtin_t)CSQC_Getinputstate); // #345 float(float framenum) getinputstate (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 347, (builtin_t)CSQC_Runstandardplayerphysics); // #347 void() runstandardplayerphysics (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 459, (builtin_t)CSQC_Edict_Num); // #459 entity(float entnum) edict_num
	PR1VM_RegisterBuiltin(vm, 460, (builtin_t)CSQC_Buf_Create); // #460 float() buf_create
	PR1VM_RegisterBuiltin(vm, 461, (builtin_t)CSQC_Buf_Del); // #461 void(float bufhandle) buf_del
	PR1VM_RegisterBuiltin(vm, 462, (builtin_t)CSQC_Buf_Getsize); // #462 float(float bufhandle) buf_getsize
	PR1VM_RegisterBuiltin(vm, 463, (builtin_t)CSQC_Buf_Copy); // #463 void(float bufhandle_from, float bufhandle_to) buf_copy
	PR1VM_RegisterBuiltin(vm, 464, (builtin_t)CSQC_Buf_Sort); // #464 void(float bufhandle, float sortpower, float backward) buf_sort
	PR1VM_RegisterBuiltin(vm, 465, (builtin_t)CSQC_Buf_Implode); // #465 string(float bufhandle, string glue) buf_implode
	PR1VM_RegisterBuiltin(vm, 466, (builtin_t)CSQC_Bufstr_Get); // #466 string(float bufhandle, float string_index) bufstr_get
	PR1VM_RegisterBuiltin(vm, 467, (builtin_t)CSQC_Bufstr_Set); // #467 void(float bufhandle, float string_index, string str) bufstr_set
	PR1VM_RegisterBuiltin(vm, 468, (builtin_t)CSQC_Bufstr_Add); // #468 float(float bufhandle, string str, float order) bufstr_add
	PR1VM_RegisterBuiltin(vm, 469, (builtin_t)CSQC_Bufstr_Free); // #469 void(float bufhandle, float string_index) bufstr_free
	PR1VM_RegisterBuiltin(vm, 177, (builtin_t)CSQC_Localsound); // #177 void(string soundname, optional float channel, optional float volume) localsound
	PR1VM_RegisterBuiltin(vm, 305, (builtin_t)CSQC_Dynamiclight_Add); // #305 float(vector org, float radius, vector lightcolours) adddynamiclight (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 335, (builtin_t)CSQC_Particleeffectnum); // #335 float(string effectname) particleeffectnum (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 336, (builtin_t)CSQC_Trailparticles); // #336 void(float effectnum, entity ent, vector start, vector end) trailparticles (EXT_CSQC),
	PR1VM_RegisterBuiltin(vm, 337, (builtin_t)CSQC_Pointparticles); // #337 void(float effectnum, vector origin [, vector dir, float count]) pointparticles (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 405, (builtin_t)CSQC_Te_Blood); // #405 void(vector org, vector velocity, float howmany) te_blood (DP_TE_BLOOD)
	PR1VM_RegisterBuiltin(vm, 406, (builtin_t)CSQC_Te_Bloodshower); // #406 void(vector mincorner, vector maxcorner, float explosionspeed, float howmany) te_bloodshower (DP_TE_BLOODSHOWER)
	PR1VM_RegisterBuiltin(vm, 407, (builtin_t)CSQC_Te_Explosionrgb); // #407 void(vector org, vector color) te_explosionrgb (DP_TE_EXPLOSIONRGB)
	PR1VM_RegisterBuiltin(vm, 408, (builtin_t)CSQC_Te_Particlecube); // #408 void(vector mincorner, vector maxcorner, vector vel, float howmany, float color, float gravityflag, float randomveljitter) te_particlecube (DP_TE_PARTICLECUBE)
	PR1VM_RegisterBuiltin(vm, 409, (builtin_t)CSQC_Te_Particlerain); // #409 void(vector mincorner, vector maxcorner, vector vel, float howmany, float color) te_particlerain (DP_TE_PARTICLERAIN)
	PR1VM_RegisterBuiltin(vm, 410, (builtin_t)CSQC_Te_Particlesnow); // #410 void(vector mincorner, vector maxcorner, vector vel, float howmany, float color) te_particlesnow (DP_TE_PARTICLESNOW)
	PR1VM_RegisterBuiltin(vm, 411, (builtin_t)CSQC_Te_Spark); // #411 void(vector org, vector vel, float howmany) te_spark (DP_TE_SPARK)
	PR1VM_RegisterBuiltin(vm, 412, (builtin_t)CSQC_Te_Gunshotquad); // #412 void(vector org) te_gunshotquad (DP_TE_QUADEFFECTS1)
	PR1VM_RegisterBuiltin(vm, 413, (builtin_t)CSQC_Te_Spikequad); // #413 void(vector org) te_spikequad (DP_TE_QUADEFFECTS1)
	PR1VM_RegisterBuiltin(vm, 414, (builtin_t)CSQC_Te_Superspikequad); // #414 void(vector org) te_superspikequad (DP_TE_QUADEFFECTS1)
	PR1VM_RegisterBuiltin(vm, 415, (builtin_t)CSQC_Te_Explosionquad); // #415 void(vector org) te_explosionquad (DP_TE_QUADEFFECTS1)
	PR1VM_RegisterBuiltin(vm, 416, (builtin_t)CSQC_Te_Smallflash); // #416 void(vector org) te_smallflash (DP_TE_SMALLFLASH)
	PR1VM_RegisterBuiltin(vm, 417, (builtin_t)CSQC_Te_Customflash); // #417 void(vector org, float radius, float lifetime, vector color) te_customflash (DP_TE_CUSTOMFLASH)
	PR1VM_RegisterBuiltin(vm, 418, (builtin_t)CSQC_Te_Gunshot); // #418 void(vector org) te_gunshot (DP_TE_STANDARDEFFECTBUILTINS)
	PR1VM_RegisterBuiltin(vm, 419, (builtin_t)CSQC_Te_Spike); // #419 void(vector org) te_spike (DP_TE_STANDARDEFFECTBUILTINS)
	PR1VM_RegisterBuiltin(vm, 420, (builtin_t)CSQC_Te_Superspike); // #420 void(vector org) te_superspike (DP_TE_STANDARDEFFECTBUILTINS)
	PR1VM_RegisterBuiltin(vm, 421, (builtin_t)CSQC_Te_Explosion); // #421 void(vector org) te_explosion (DP_TE_STANDARDEFFECTBUILTINS)
	PR1VM_RegisterBuiltin(vm, 422, (builtin_t)CSQC_Te_Tarexplosion); // #422 void(vector org) te_tarexplosion (DP_TE_STANDARDEFFECTBUILTINS)
	PR1VM_RegisterBuiltin(vm, 423, (builtin_t)CSQC_Te_Wizspike); // #423 void(vector org) te_wizspike (DP_TE_STANDARDEFFECTBUILTINS)
	PR1VM_RegisterBuiltin(vm, 424, (builtin_t)CSQC_Te_Knightspike); // #424 void(vector org) te_knightspike (DP_TE_STANDARDEFFECTBUILTINS)
	PR1VM_RegisterBuiltin(vm, 425, (builtin_t)CSQC_Te_Lavasplash); // #425 void(vector org) te_lavasplash  (DP_TE_STANDARDEFFECTBUILTINS)
	PR1VM_RegisterBuiltin(vm, 427, (builtin_t)CSQC_Te_Explosion2); // #427 void(vector org, float color, float colorlength) te_explosion2 (DP_TE_STANDARDEFFECTBUILTINS)
	PR1VM_RegisterBuiltin(vm, 428, (builtin_t)CSQC_Te_Lightning1); // #428 void(entity own, vector start, vector end) te_lightning1 (DP_TE_STANDARDEFFECTBUILTINS)
	PR1VM_RegisterBuiltin(vm, 429, (builtin_t)CSQC_Te_Lightning2); // #429 void(entity own, vector start, vector end) te_lightning2 (DP_TE_STANDARDEFFECTBUILTINS)
	PR1VM_RegisterBuiltin(vm, 430, (builtin_t)CSQC_Te_Lightning3); // #430 void(entity own, vector start, vector end) te_lightning3 (DP_TE_STANDARDEFFECTBUILTINS)
	PR1VM_RegisterBuiltin(vm, 431, (builtin_t)CSQC_Te_Beam); // #431 void(entity own, vector start, vector end) te_beam (DP_TE_STANDARDEFFECTBUILTINS)
	PR1VM_RegisterBuiltin(vm, 115, (builtin_t)CSQC_Strcat); // #115 string(string str1, string str2, ...) strcat (FRIK_FILE)
	PR1VM_RegisterBuiltin(vm, 221, (builtin_t)CSQC_Strstrofs); // #221 float(string s1, string sub) strstrofs (FTE_STRINGS)
	PR1VM_RegisterBuiltin(vm, 352, (builtin_t)CSQC_Registercommand); // #352 void(string cmdname) registercommand (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 441, (builtin_t)CSQC_Tokenize); // #441 float(string s) tokenize (KRIMZON_SV_PARSECLIENTCOMMAND)
	PR1VM_RegisterBuiltin(vm, 442, (builtin_t)CSQC_Argv); // #442 string(float n) argv (KRIMZON_SV_PARSECLIENTCOMMAND)

	PR1VM_RegisterBuiltin(vm, 471, (builtin_t)CSQC_Asin); // #471 float(float s) asin
	PR1VM_RegisterBuiltin(vm, 472, (builtin_t)CSQC_Acos); // #472 float(float c) acos
	PR1VM_RegisterBuiltin(vm, 473, (builtin_t)CSQC_Atan); // #473 float(float t) atan
	PR1VM_RegisterBuiltin(vm, 474, (builtin_t)CSQC_Atan2); // #474 float(float c, float s) atan2
	PR1VM_RegisterBuiltin(vm, 475, (builtin_t)CSQC_Tan); // #475 float(float a) tan
	PR1VM_RegisterBuiltin(vm, 532, (builtin_t)CSQC_Log); // #532 float(float value, optional float base) log
	PR1VM_RegisterBuiltin(vm, 102, (builtin_t)CSQC_Anglemod); // #102 float(float value) anglemod
	PR1VM_RegisterBuiltin(vm, 245, (builtin_t)CSQC_Mod); // #245 float(float a, float b) mod
	PR1VM_RegisterBuiltin(vm, 218, (builtin_t)CSQC_Bitshift); // #218 bitshift (EXT_DIMENSION_PLANES)
	PR1VM_RegisterBuiltin(vm, 494, (builtin_t)CSQC_Crc16); // #494 float(float caseinsensitive, string s, ...) crc16
	PR1VM_RegisterBuiltin(vm, 519, (builtin_t)CSQC_Gettimef); // #519 float(optional float timetype) gettime

	PR1VM_RegisterBuiltin(vm, 259, (builtin_t)CSQC_Stoi); // #259 int(string) stoi
	PR1VM_RegisterBuiltin(vm, 260, (builtin_t)CSQC_Itos); // #260 string(int) itos
	PR1VM_RegisterBuiltin(vm, 261, (builtin_t)CSQC_Stoh); // #261 int(string) stoh
	PR1VM_RegisterBuiltin(vm, 262, (builtin_t)CSQC_Htos); // #262 string(int) htos

	PR1VM_RegisterBuiltin(vm, 482, (builtin_t)CSQC_Cvar_Defstring); // #482 string(string s) cvar_defstring
	PR1VM_RegisterBuiltin(vm, 495, (builtin_t)CSQC_Cvar_Type); // #495 float(string name) cvar_type
	PR1VM_RegisterBuiltin(vm, 518, (builtin_t)CSQC_Cvar_Description); // #518 string(string cvarname) cvar_description

	PR1VM_RegisterBuiltin(vm, 222, (builtin_t)CSQC_Str2chr); // #222 float(string str, float index) str2chr (FTE_STRINGS)
	PR1VM_RegisterBuiltin(vm, 223, (builtin_t)CSQC_Chr2str); // #223 string(float chr, ...) chr2str (FTE_STRINGS)
	PR1VM_RegisterBuiltin(vm, 225, (builtin_t)CSQC_Strpad); // #225 string strpad(float pad, string str1, ...) strpad (FTE_STRINGS)
	PR1VM_RegisterBuiltin(vm, 226, (builtin_t)CSQC_Infoadd); // #226 string(string old, string key, string value) infoadd
	PR1VM_RegisterBuiltin(vm, 227, (builtin_t)CSQC_Infoget); // #227 string(string info, string key) infoget
	PR1VM_RegisterBuiltin(vm, 228, (builtin_t)CSQC_Strncmp); // #228 float(string s1, string s2) strcmp (FTE_STRINGS)
	PR1VM_RegisterBuiltin(vm, 229, (builtin_t)CSQC_Strncasecmp); // #229 float(string s1, string s2) strcasecmp (FTE_STRINGS)
	PR1VM_RegisterBuiltin(vm, 230, (builtin_t)CSQC_Strncasecmp); // #230 float(string s1, string s2, float len) strncasecmp (FTE_STRINGS)
	PR1VM_RegisterBuiltin(vm, 480, (builtin_t)CSQC_Strtolower); // #480 string(string s) strtolower
	PR1VM_RegisterBuiltin(vm, 481, (builtin_t)CSQC_Strtoupper); // #481 string(string s) strtoupper
	PR1VM_RegisterBuiltin(vm, 484, (builtin_t)CSQC_Strreplace); // #484 string(string search, string replace, string subject) strreplace
	PR1VM_RegisterBuiltin(vm, 485, (builtin_t)CSQC_Strireplace); // #485 string(string search, string replace, string subject) strireplace

	PR1VM_RegisterBuiltin(vm, 65, (builtin_t)CSQC_Etos); // #65 string(entity ent) etos (DP_QC_ETOS)
	PR1VM_RegisterBuiltin(vm, 338, (builtin_t)CSQC_Cprint); // #338 void(string s) cprint (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 339, (builtin_t)CSQC_Print); // #339 void(string s) print (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 350, (builtin_t)CSQC_Isserver); // #350 float() isserver (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 353, (builtin_t)CSQC_Wasfreed); // #353 float(entity ent) wasfreed (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 512, (builtin_t)CSQC_Num_For_Edict); // #512 float(entity ent) num_for_edict
	PR1VM_RegisterBuiltin(vm, 63, (builtin_t)CSQC_Changepitch); // #63 void(entity ent) changepitch (DP_QC_CHANGEPITCH)
	PR1VM_RegisterBuiltin(vm, 355, (builtin_t)CSQC_Nop_Str); // #355 string() getentitytoken;

	PR1VM_RegisterBuiltin(vm, 478, (builtin_t)CSQC_Strftime); // #478 string(float uselocaltime, string format, ...) strftime
	PR1VM_RegisterBuiltin(vm, 514, (builtin_t)CSQC_Tokenize_Console); // #514 float(string str) tokenize_console
	PR1VM_RegisterBuiltin(vm, 479, (builtin_t)CSQC_Tokenizebyseparator); // #479 float(string s, string separator1, ...) tokenizebyseparator
	PR1VM_RegisterBuiltin(vm, 515, (builtin_t)CSQC_Argv_Start_Index); // #515 float(float idx) argv_start_index
	PR1VM_RegisterBuiltin(vm, 516, (builtin_t)CSQC_Argv_End_Index); // #516 float(float idx) argv_end_index

	PR1VM_RegisterBuiltin(vm, 342, (builtin_t)CSQC_Getkeybind); // #342 string(float keynum) getkeybind (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 520, (builtin_t)CSQC_Keynumtostring_Menu); // #520 string(float keynum) keynumtostring_omgwtf
	PR1VM_RegisterBuiltin(vm, 521, (builtin_t)CSQC_Findkeysforcommand); // #521 string(string command, optional float bindmap) findkeysforcommand
	PR1VM_RegisterBuiltin(vm, 603, (builtin_t)CSQC_Setmousetarget); // #603 void(float trg) setmousetarget
	PR1VM_RegisterBuiltin(vm, 604, (builtin_t)CSQC_Getmousetarget); // #604 float() getmousetarget
	PR1VM_RegisterBuiltin(vm, 608, (builtin_t)CSQC_Getresolution); // #608 vector(float vidmode, optional float forfullscreen) getresolution
	PR1VM_RegisterBuiltin(vm, 609, (builtin_t)CSQC_Keynumtostring_Menu); // #609 string(float keynum) keynumtostring_menu
	PR1VM_RegisterBuiltin(vm, 610, (builtin_t)CSQC_Findkeysforcommand); // #610 string(string command, optional float bindmap) findkeysforcommand_dp
	PR1VM_RegisterBuiltin(vm, 614, (builtin_t)CSQC_Stringtokeynum_Menu); // #614 float(string key) stringtokeynum_menu
	PR1VM_RegisterBuiltin(vm, 630, (builtin_t)CSQC_Setkeybind); // #630 void(float keynum, string binding, optional float bindmap) setkeybind
	PR1VM_RegisterBuiltin(vm, 631, (builtin_t)CSQC_Getbindmaps); // #631 vector() getbindmaps
	PR1VM_RegisterBuiltin(vm, 632, (builtin_t)CSQC_Setbindmaps); // #632 float(vector bindmaps) setbindmaps

	PR1VM_RegisterBuiltin(vm, 98, (builtin_t)CSQC_Findfloat); // #98 entity(entity start, .float fld, float match) findfloat (DP_QC_FINDFLOAT)
	PR1VM_RegisterBuiltin(vm, 92, (builtin_t)CSQC_Getlight_Approx); // #92 vector(vector org) getlight (DP_QC_GETLIGHT)
	PR1VM_RegisterBuiltin(vm, 64, (builtin_t)CSQC_Vmrest_Nop); // #64 void(entity ent, entity ignore) tracetoss (DP_QC_TRACETOSS)
	PR1VM_RegisterBuiltin(vm, 240, (builtin_t)CSQC_Vmrest_Nop); // #240 float(vector viewpos, entity entity) checkpvs
	PR1VM_RegisterBuiltin(vm, 278, (builtin_t)CSQC_Vmrest_Nop); // #278 void(float action, vector pos, float radius, float quant) terrain_edit
	PR1VM_RegisterBuiltin(vm, 279, (builtin_t)CSQC_Vmrest_Nop); // #279 void() touchtriggers
	PR1VM_RegisterBuiltin(vm, 504, (builtin_t)CSQC_Getentity); // #504 __variant(float entnum, float fieldnum) getentity

	PR1VM_RegisterBuiltin(vm, 483, (builtin_t)CSQC_Pointsound); // #483 void(vector origin, string sample, float volume, float attenuation) pointsound
	PR1VM_RegisterBuiltin(vm, 351, (builtin_t)CSQC_Setlistener); // #351 void(vector origin, vector forward, vector right, vector up) SetListener (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 371, (builtin_t)CSQC_Deltalisten); // #371 float(string modelname, float flags) deltalisten  (EXT_CSQC_1)
	PR1VM_RegisterBuiltin(vm, 533, (builtin_t)CSQC_Getsoundtime); // #533 float(entity e, float channel) getsoundtime
	PR1VM_RegisterBuiltin(vm, 534, (builtin_t)CSQC_Soundlength); // #534 float(string sample) soundlength

	PR1VM_RegisterBuiltin(vm, 372, (builtin_t)CSQC_Light_Nop_Ret0); // #372 __variant(float lno, float fld) dynamiclight_get
	PR1VM_RegisterBuiltin(vm, 373, (builtin_t)CSQC_Light_Nop_Ret0); // #373 void(float lno, float fld, __variant value) dynamiclight_set
	PR1VM_RegisterBuiltin(vm, 375, (builtin_t)CSQC_Light_Nop_Ret0); // #375 void(string shadername, vector origin, vector up, vector side, vector rgb, float alpha) adddecal
	PR1VM_RegisterBuiltin(vm, 376, (builtin_t)CSQC_Light_Nop_Ret0); // #376 void(entity e, string skinfilename, optional string skindata) setcustomskin
	PR1VM_RegisterBuiltin(vm, 377, (builtin_t)CSQC_Light_Nop_Ret0);
	PR1VM_RegisterBuiltin(vm, 378, (builtin_t)CSQC_Light_Nop_Ret0);
	PR1VM_RegisterBuiltin(vm, 379, (builtin_t)CSQC_Light_Nop_Ret0);
	PR1VM_RegisterBuiltin(vm, 501, (builtin_t)CSQC_Light_Nop_Ret0); // #501 void(float to, string s, float sz) WritePicture

	PR1VM_RegisterBuiltin(vm, 294, (builtin_t)CSQC_Checkcommand); // #294 float(string name) checkcommand
	PR1VM_RegisterBuiltin(vm, 295, (builtin_t)CSQC_Argescape); // #295 string(string s) argescape
	PR1VM_RegisterBuiltin(vm, 607, (builtin_t)CSQC_Isfunction); // #607 float(string s) isfunction
	PR1VM_RegisterBuiltin(vm, 391, (builtin_t)CSQC_Nop_Str); // #391 string(string conname, string field, optional string newvalue) con_getset
	PR1VM_RegisterBuiltin(vm, 392, (builtin_t)CSQC_Vmrest_Nop); // #392 void(string conname, string messagefmt, ...) con_printf
	PR1VM_RegisterBuiltin(vm, 393, (builtin_t)CSQC_Vmrest_Nop); // #393 void(string conname, vector pos, vector size, float fontsize) con_draw
	PR1VM_RegisterBuiltin(vm, 394, (builtin_t)CSQC_Light_Nop_Ret0); // #394 float(string conname, float inevtype, float parama, float paramb, float paramc) con_input
	PR1VM_RegisterBuiltin(vm, 605, (builtin_t)CSQC_Vmrest_Nop); // #605 void(.../*, string funcname*/) callfunction

	PR1VM_RegisterBuiltin(vm, 434, (builtin_t)CSQC_Light_Nop_Ret0); // #434 float(entity e, float s) getsurfacenumpoints (DP_QC_GETSURFACE)
	PR1VM_RegisterBuiltin(vm, 435, (builtin_t)CSQC_Bsp_Nop_Vec); // #435 vector(entity e, float s, float n) getsurfacepoint (DP_QC_GETSURFACE)
	PR1VM_RegisterBuiltin(vm, 436, (builtin_t)CSQC_Bsp_Nop_Vec); // #436 vector(entity e, float s) getsurfacenormal (DP_QC_GETSURFACE)
	PR1VM_RegisterBuiltin(vm, 437, (builtin_t)CSQC_Nop_Str); // #437 string(entity e, float s) getsurfacetexture (DP_QC_GETSURFACE)
	PR1VM_RegisterBuiltin(vm, 438, (builtin_t)CSQC_Light_Nop_Ret0); // #438 float(entity e, vector p) getsurfacenearpoint (DP_QC_GETSURFACE)
	PR1VM_RegisterBuiltin(vm, 439, (builtin_t)CSQC_Bsp_Nop_Vec); // #439 vector(entity e, float s, vector p) getsurfaceclippedpoint (DP_QC_GETSURFACE)
	PR1VM_RegisterBuiltin(vm, 486, (builtin_t)CSQC_Bsp_Nop_Vec); // #486 vector(entity e, float s, float n, float a) getsurfacepointattribute
	PR1VM_RegisterBuiltin(vm, 628, (builtin_t)CSQC_Light_Nop_Ret0); // #628 float(entity e, float s) getsurfacenumtriangles
	PR1VM_RegisterBuiltin(vm, 629, (builtin_t)CSQC_Bsp_Nop_Vec); // #629 vector(entity e, float s, float n) getsurfacetriangle

	PR1VM_RegisterBuiltin(vm, 496, (builtin_t)CSQC_Numentityfields); // #496 float() numentityfields
	PR1VM_RegisterBuiltin(vm, 497, (builtin_t)CSQC_Entityfieldname); // #497 string(float fieldnum) entityfieldname
	PR1VM_RegisterBuiltin(vm, 498, (builtin_t)CSQC_Entityfieldtype); // #498 float(float fieldnum) entityfieldtype
	PR1VM_RegisterBuiltin(vm, 499, (builtin_t)CSQC_Getentityfieldstring); // #499 string(float fieldnum, entity ent) getentityfieldstring
	PR1VM_RegisterBuiltin(vm, 500, (builtin_t)CSQC_Putentityfieldstring); // #500 float(float fieldnum, entity ent, string s) putentityfieldstring

	PR1VM_RegisterBuiltin(vm, 300, (builtin_t)CSQC_Clearscene); // #300 void() clearscene (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 301, (builtin_t)CSQC_Addentities); // #301 void(float mask) addentities (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 303, (builtin_t)CSQC_Setproperty); // #303 float(float property, ...) setproperty (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 304, (builtin_t)CSQC_Renderscene); // #304 void() renderscene (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 309, (builtin_t)CSQC_Getproperty); // #309 vector/float(float property) getproperty (EXT_CSQC_1)
	PR1VM_RegisterBuiltin(vm, 326, (builtin_t)CSQC_Drawstring); // #326 float(vector position, string text, vector size, vector rgb, float alpha, float drawflag) drawstring
	PR1VM_RegisterBuiltin(vm, 315, (builtin_t)CSQC_Drawline); // #315 void(float width, vector pos1, vector pos2) drawline (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 317, (builtin_t)CSQC_Precache_Pic); // #317 string(string name, float trywad) precache_pic (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 320, (builtin_t)CSQC_Drawcharacter); // #320 float(vector position, float character, vector scale, vector rgb, float alpha [, float flag]) drawcharacter (EXT_CSQC, [EXT_CSQC_???])
	PR1VM_RegisterBuiltin(vm, 322, (builtin_t)CSQC_Drawpic); // #322 float(vector position, string pic, vector size, vector rgb, float alpha [, float flag]) drawpic (EXT_CSQC, [EXT_CSQC_???])
	PR1VM_RegisterBuiltin(vm, 323, (builtin_t)CSQC_Drawfill); // #323 float(vector position, vector size, vector rgb, float alpha [, float flag]) drawfill (EXT_CSQC, [EXT_CSQC_???])
	PR1VM_RegisterBuiltin(vm, 327, (builtin_t)CSQC_Stringwidth); // #327 float(string text, float usecolours, optional vector fontsize) stringwidth
	PR1VM_RegisterBuiltin(vm, 328, (builtin_t)CSQC_Drawsubpic); // #328 void(vector pos, vector sz, string pic, vector srcpos, vector srcsz, vector rgb, float alpha, optional float drawflag) drawsubpic
	PR1VM_RegisterBuiltin(vm, 316, (builtin_t)CSQC_Iscachedpic); // #316 float(string name) iscachedpic (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 318, (builtin_t)CSQC_Drawgetimagesize); // #318 vector(string picname) draw_getimagesize (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 319, (builtin_t)CSQC_Freepic); // #319 void(string name) freepic (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 321, (builtin_t)CSQC_Drawrawstring); // #321 float(vector position, string text, vector scale, vector rgb, float alpha [, float flag]) drawstring (EXT_CSQC, [EXT_CSQC_???])
	PR1VM_RegisterBuiltin(vm, 324, (builtin_t)CSQC_Drawsetcliparea); // #324 void(float x, float y, float width, float height) drawsetcliparea (EXT_CSQC_???)
	PR1VM_RegisterBuiltin(vm, 325, (builtin_t)CSQC_Drawresetcliparea); // #325 void(void) drawresetcliparea (EXT_CSQC_???)
	PR1VM_RegisterBuiltin(vm, 329, (builtin_t)CSQC_Drawrotpic_Dp); // #329 void(vector pivot, string picname, vector size, vector mins, float angle, vector rgb, float alpha, optional float drawflag) drawrotpic_dp
	PR1VM_RegisterBuiltin(vm, 330, (builtin_t)CSQC_Getstati); // #330 int(float stnum) getstati (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 331, (builtin_t)CSQC_Getstatf); // #331 float(float stnum) getstatf (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 332, (builtin_t)CSQC_Getstats); // #332 string(float firststnum) getstats (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 359, (builtin_t)CSQC_Sendevent); // #359 void(string evname, string evargs, ...) (EXT_CSQC_1)
	PR1VM_RegisterBuiltin(vm, 627, (builtin_t)CSQC_Sprintf); // #627 string(string fmt, ...) sprintf

	PR1VM_RegisterBuiltin(vm, 360, (builtin_t)CSQC_Readbyte); // #360 float() readbyte (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 361, (builtin_t)CSQC_Readchar); // #361 float() readchar (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 362, (builtin_t)CSQC_Readshort); // #362 float() readshort (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 363, (builtin_t)CSQC_Readlong); // #363 float() readlong (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 364, (builtin_t)CSQC_Readcoord); // #364 float() readcoord (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 365, (builtin_t)CSQC_Readangle); // #365 float() readangle (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 366, (builtin_t)CSQC_Readstring); // #366 string() readstring (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 367, (builtin_t)CSQC_Readfloat); // #367 float() readfloat (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 368, (builtin_t)CSQC_Readentitynum); // #368 float() readentitynum (EXT_CSQC)

	// VOID stubs - type-correct no-op.
	PR1VM_RegisterBuiltin(vm, 111, (builtin_t)CSQC_Vmrest_Nop); // #111 void(float fnum) fclose (FRIK_FILE)
	PR1VM_RegisterBuiltin(vm, 113, (builtin_t)CSQC_Vmrest_Nop); // #113 void(float fnum, string str) fputs (FRIK_FILE)
	PR1VM_RegisterBuiltin(vm, 204, (builtin_t)CSQC_Vmrest_Nop); // #204 void(float prnum, __variant newval, string varname) externset - no-op in classic v6: variant write to another program (no v6 types)
	PR1VM_RegisterBuiltin(vm, 207, (builtin_t)CSQC_Vmrest_Nop); // #207 void(entity portal, float state) openportal
	PR1VM_RegisterBuiltin(vm, 210, (builtin_t)CSQC_Vmrest_Nop); // #210 void() fork
	PR1VM_RegisterBuiltin(vm, 211, (builtin_t)CSQC_Vmrest_Nop); // #211 void() abort (FTE_MULTITHREADED)
	PR1VM_RegisterBuiltin(vm, 212, (builtin_t)CSQC_Vmrest_Nop); // #212 void() sleep
	PR1VM_RegisterBuiltin(vm, 215, (builtin_t)CSQC_Vmrest_Nop); // #215 215 (FTE_PEXT_HEXEN2)
	PR1VM_RegisterBuiltin(vm, 216, (builtin_t)CSQC_Vmrest_Nop); // #216 216 (FTE_PEXT_HEXEN2)
	PR1VM_RegisterBuiltin(vm, 217, (builtin_t)CSQC_Vmrest_Nop); // #217 217 (FTE_PEXT_HEXEN2)
	PR1VM_RegisterBuiltin(vm, 219, (builtin_t)CSQC_Te_Lightningblood); // #219 te_lightningblood void(vector org) (FTE_TE_STANDARDEFFECTBUILTINS)
	PR1VM_RegisterBuiltin(vm, 234, (builtin_t)CSQC_Light_Nop_Ret0); // #234 float(entity ent) isbackbuffered
	PR1VM_RegisterBuiltin(vm, 235, (builtin_t)CSQC_Vmrest_Nop); // #235 void(vector angle) rotatevectorsbyangle
	PR1VM_RegisterBuiltin(vm, 236, (builtin_t)CSQC_Vmrest_Nop); // #236 void(vector fwd, vector right, vector up) rotatevectorsbyvectors
	PR1VM_RegisterBuiltin(vm, 239, (builtin_t)CSQC_Te_Bloodqw); // #239 void te_bloodqw(vector org[, float count]) (FTE_TE_STANDARDEFFECTBUILTINS)
	PR1VM_RegisterBuiltin(vm, 271, (builtin_t)CSQC_Vmrest_Nop); // #271 void(float skel, float bonenum, vector org) skel_set_bone
	PR1VM_RegisterBuiltin(vm, 272, (builtin_t)CSQC_Vmrest_Nop); // #272 void(float skel, float bonenum, vector org) skel_mul_bone
	PR1VM_RegisterBuiltin(vm, 273, (builtin_t)CSQC_Vmrest_Nop); // #273 void(float skel, float startbone, float endbone, vector org) skel_mul_bone
	PR1VM_RegisterBuiltin(vm, 274, (builtin_t)CSQC_Vmrest_Nop); // #274 void(float skeldst, float skelsrc, float startbone, float entbone) skel_copybones
	PR1VM_RegisterBuiltin(vm, 275, (builtin_t)CSQC_Vmrest_Nop); // #275 void(float skel) skel_delete
	PR1VM_RegisterBuiltin(vm, 283, (builtin_t)CSQC_Vmrest_Nop); // #283 void(entity ent, float bonenum, vector org, optional vector angorfwd, optional vector right, optional vector up) skel_set_bone_world
	PR1VM_RegisterBuiltin(vm, 288, (builtin_t)CSQC_Vmrest_Nop); // #288 void(hashtable table) hash_destroytab
	PR1VM_RegisterBuiltin(vm, 289, (builtin_t)CSQC_Vmrest_Nop); // #289 void(hashtable table, string name, __variant value, optional float typeandflags) hash_add
	PR1VM_RegisterBuiltin(vm, 293, (builtin_t)CSQC_Vmrest_Nop); // #293 void() hash_getcb
	PR1VM_RegisterBuiltin(vm, 302, (builtin_t)CSQC_Addentity); // #302 void(entity ent) addentity (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 306, (builtin_t)CSQC_Vmrest_Nop); // #306 void(string texturename) R_BeginPolygon (EXT_CSQC_???)
	PR1VM_RegisterBuiltin(vm, 307, (builtin_t)CSQC_Vmrest_Nop); // #307 void(vector org, vector texcoords, vector rgb, float alpha) R_PolygonVertex (EXT_CSQC_???)
	PR1VM_RegisterBuiltin(vm, 308, (builtin_t)CSQC_Vmrest_Nop); // #308 void() R_EndPolygon (EXT_CSQC_???)
	PR1VM_RegisterBuiltin(vm, 333, (builtin_t)CSQC_Setmodelindex); // #333 void(entity e, float mdlindex) setmodelindex (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 385, (builtin_t)CSQC_Vmrest_Nop); // #385 void(__variant *ptr) memfree - no-op in classic v6: typed pointer (no v6 pointer model)
	PR1VM_RegisterBuiltin(vm, 386, (builtin_t)CSQC_Vmrest_Nop); // #386 void(__variant *dst, __variant *src, int size) memcpy - no-op in classic v6: typed pointer (no v6 pointer model)
	PR1VM_RegisterBuiltin(vm, 387, (builtin_t)CSQC_Vmrest_Nop); // #387 void(__variant *dst, int val, int size) memfill8 - no-op in classic v6: typed pointer (no v6 pointer model)
	PR1VM_RegisterBuiltin(vm, 389, (builtin_t)CSQC_Vmrest_Nop); // #389 void(__variant *dst, float ofs, __variant val) memsetval - no-op in classic v6: typed pointer + runtime value type
	PR1VM_RegisterBuiltin(vm, 400, (builtin_t)CSQC_Copyentity); // #400 void(entity from, entity to) copyentity (DP_QC_COPYENTITY)
	PR1VM_RegisterBuiltin(vm, 404, (builtin_t)CSQC_Effect); // #404 void(vector org, string modelname, float startframe, float endframe, float framerate) effect (DP_SV_EFFECT)
	PR1VM_RegisterBuiltin(vm, 426, (builtin_t)CSQC_Te_Teleport); // #426 void(vector org) te_teleport (DP_TE_STANDARDEFFECTBUILTINS)
	PR1VM_RegisterBuiltin(vm, 432, (builtin_t)CSQC_Vectorvectors); // #432 void(vector dir) vectorvectors (DP_QC_VECTORVECTORS)
	PR1VM_RegisterBuiltin(vm, 433, (builtin_t)CSQC_Te_Plasmaburn); // #433 void(vector org) te_plasmaburn (DP_TE_PLASMABURN)
	PR1VM_RegisterBuiltin(vm, 443, (builtin_t)CSQC_Vmrest_Nop); // #443 void(entity e, entity tagentity, string tagname) setattachment (DP_GFX_QUAKE3MODELTAGS)
	PR1VM_RegisterBuiltin(vm, 445, (builtin_t)CSQC_Vmrest_Nop); // #445 void	search_end(float handle) (DP_QC_FS_SEARCH)
	PR1VM_RegisterBuiltin(vm, 457, (builtin_t)CSQC_Te_Flamejet); // #457 void(vector org, vector vel, float howmany) te_flamejet
	PR1VM_RegisterBuiltin(vm, 488, (builtin_t)CSQC_Vmrest_Nop); // #488 void(string name)
	PR1VM_RegisterBuiltin(vm, 489, (builtin_t)CSQC_Vmrest_Nop); // #489 void(string name, string URI)
	PR1VM_RegisterBuiltin(vm, 491, (builtin_t)CSQC_Vmrest_Nop); // #491 void(string name, float x, float y)
	PR1VM_RegisterBuiltin(vm, 492, (builtin_t)CSQC_Vmrest_Nop); // #492 void(string name, float w, float h)
	PR1VM_RegisterBuiltin(vm, 502, (builtin_t)CSQC_Boxparticles); // #502 void(float effectindex, entity own, vector org_from, vector org_to, vector dir_from, vector dir_to, float countmultiplier, optional float flags) boxparticles
	PR1VM_RegisterBuiltin(vm, 517, (builtin_t)CSQC_Vmrest_Nop); // #517 void(strbuf strbuf) buf_cvarlist
	PR1VM_RegisterBuiltin(vm, 529, (builtin_t)CSQC_Vmrest_Nop); // #529 void(string s) loadfromdata
	PR1VM_RegisterBuiltin(vm, 530, (builtin_t)CSQC_Vmrest_Nop); // #530 void(string s) loadfromfile
	PR1VM_RegisterBuiltin(vm, 540, (builtin_t)CSQC_Vmrest_Nop); // #540 void(entity e, float physics_enabled) physics_enable
	PR1VM_RegisterBuiltin(vm, 541, (builtin_t)CSQC_Vmrest_Nop); // #541 void(entity e, vector force, vector relative_ofs) physics_addforce
	PR1VM_RegisterBuiltin(vm, 542, (builtin_t)CSQC_Vmrest_Nop); // #542 void(entity e, vector torque) physics_addtorque
	PR1VM_RegisterBuiltin(vm, 606, (builtin_t)CSQC_Vmrest_Nop); // #606 void(filestream fh, entity e) writetofile
	PR1VM_RegisterBuiltin(vm, 613, (builtin_t)CSQC_Vmrest_Nop); // #613 void(entity e, string s) parseentitydata
	PR1VM_RegisterBuiltin(vm, 615, (builtin_t)CSQC_Vmrest_Nop); // #615 void() resethostcachemasks
	PR1VM_RegisterBuiltin(vm, 616, (builtin_t)CSQC_Vmrest_Nop); // #616 void(float mask, float fld, string str, float op) sethostcachemaskstring
	PR1VM_RegisterBuiltin(vm, 617, (builtin_t)CSQC_Vmrest_Nop); // #617 void(float mask, float fld, float num, float op) sethostcachemasknumber
	PR1VM_RegisterBuiltin(vm, 618, (builtin_t)CSQC_Vmrest_Nop); // #618 void() resorthostcache
	PR1VM_RegisterBuiltin(vm, 619, (builtin_t)CSQC_Vmrest_Nop); // #619 void(float fld, float descending) sethostcachesort
	PR1VM_RegisterBuiltin(vm, 620, (builtin_t)CSQC_Vmrest_Nop); // #620 void() refreshhostcache
	PR1VM_RegisterBuiltin(vm, 623, (builtin_t)CSQC_Vmrest_Nop); // #623 void(string key) addwantedhostcachekey
	PR1VM_RegisterBuiltin(vm, 650, (builtin_t)CSQC_Vmrest_Nop); // #650 void() fcopy
	PR1VM_RegisterBuiltin(vm, 651, (builtin_t)CSQC_Vmrest_Nop); // #651 void() frename
	PR1VM_RegisterBuiltin(vm, 652, (builtin_t)CSQC_Vmrest_Nop); // #652 void() fremove
	PR1VM_RegisterBuiltin(vm, 654, (builtin_t)CSQC_Vmrest_Nop); // #654 void() rmtree
	PR1VM_RegisterBuiltin(vm, 741, (builtin_t)CSQC_Vmrest_Nop); // #741 void() controller_rumble
	PR1VM_RegisterBuiltin(vm, 742, (builtin_t)CSQC_Vmrest_Nop); // #742 void() controller_rumbletriggers
	// FLOAT0 stubs - type-correct no-op.
	PR1VM_RegisterBuiltin(vm, 110, (builtin_t)CSQC_Fopen); // #110 float(string strname, float accessmode) fopen (FRIK_FILE)
	PR1VM_RegisterBuiltin(vm, 200, (builtin_t)CSQC_Getmodelindex); // #200 float(string modelname, optional float queryonly) getmodelindex
	PR1VM_RegisterBuiltin(vm, 201, (builtin_t)CSQC_Light_Nop_Ret0); // #201 __variant(float prnum, string funcname, ...) externcall - no-op in classic v6: variant exchange between VMs (no v6 types)
	PR1VM_RegisterBuiltin(vm, 202, (builtin_t)CSQC_Light_Nop_Ret0); // #202 float(string progsname) addprogs
	PR1VM_RegisterBuiltin(vm, 203, (builtin_t)CSQC_Light_Nop_Ret0); // #203 __variant(float prnum, string varname) externvalue - no-op in classic v6: variant read of another program (no v6 types)
	PR1VM_RegisterBuiltin(vm, 205, (builtin_t)CSQC_Light_Nop_Ret0); // #205 float() externrefcall
	PR1VM_RegisterBuiltin(vm, 206, (builtin_t)CSQC_Instr); // #206 string(string input, string token) instr
	PR1VM_RegisterBuiltin(vm, 237, (builtin_t)CSQC_Light_Nop_Ret0); // #237 float(float mdlindex, string skinname) skinforname
	PR1VM_RegisterBuiltin(vm, 238, (builtin_t)CSQC_Light_Nop_Ret0); // #238 float(string shadername, optional string defaultshader, ...) shaderforname
	PR1VM_RegisterBuiltin(vm, 242, (builtin_t)CSQC_Light_Nop_Ret0); // #242 void(string dest, string content) sendpacket
	PR1VM_RegisterBuiltin(vm, 263, (builtin_t)CSQC_Light_Nop_Ret0); // #263 float(float modlindex) skel_create
	PR1VM_RegisterBuiltin(vm, 264, (builtin_t)CSQC_Light_Nop_Ret0); // #264 float(float skel, entity ent, float modelindex, float retainfrac, float firstbone, float lastbone, optional float addition) skel_build
	PR1VM_RegisterBuiltin(vm, 265, (builtin_t)CSQC_Light_Nop_Ret0); // #265 float(float skel) skel_get_numbones
	PR1VM_RegisterBuiltin(vm, 267, (builtin_t)CSQC_Light_Nop_Ret0); // #267 float(float skel, float bonenum) skel_get_boneparent
	PR1VM_RegisterBuiltin(vm, 268, (builtin_t)CSQC_Light_Nop_Ret0); // #268 float(float skel, string tagname) skel_get_boneidx
	PR1VM_RegisterBuiltin(vm, 276, (builtin_t)CSQC_Light_Nop_Ret0); // #276 float(float modidx, string framename) frameforname
	PR1VM_RegisterBuiltin(vm, 277, (builtin_t)CSQC_Light_Nop_Ret0); // #277 float(float modidx, float framenum) frameduration
	PR1VM_RegisterBuiltin(vm, 281, (builtin_t)CSQC_Light_Nop_Ret0); // #281 (FTE_QC_RAGDOLL)
	PR1VM_RegisterBuiltin(vm, 282, (builtin_t)CSQC_Light_Nop_Ret0); // #282 (FTE_QC_RAGDOLL) skel_mmap - no-op in classic v6: native pointer to skeleton data (no v6 pointer)
	PR1VM_RegisterBuiltin(vm, 286, (builtin_t)CSQC_Light_Nop_Ret0); // #286 float(float resourcetype, float tryload, string resourcename) resourcestatus
	PR1VM_RegisterBuiltin(vm, 287, (builtin_t)CSQC_Light_Nop_Ret0); // #287 hashtable(float tabsize, optional float defaulttype) hash_createtab
	PR1VM_RegisterBuiltin(vm, 290, (builtin_t)CSQC_Light_Nop_Ret0); // #290 __variant(hashtable table, string name, optional __variant deflt, optional float requiretype, optional float index) hash_get
	PR1VM_RegisterBuiltin(vm, 291, (builtin_t)CSQC_Light_Nop_Ret0); // #291 __variant(hashtable table, string name) hash_delete
	PR1VM_RegisterBuiltin(vm, 356, (builtin_t)CSQC_Light_Nop_Ret0); // #356 float(string s) findfont
	PR1VM_RegisterBuiltin(vm, 357, (builtin_t)CSQC_Light_Nop_Ret0); // #357 float(string fontname, string fontmaps, string sizes, float slot, optional float fix_scale, optional float fix_voffset) loadfont
	PR1VM_RegisterBuiltin(vm, 384, (builtin_t)CSQC_Light_Nop_Ret0); // #384 __variant*(int size) memalloc - no-op in classic v6: typed pointer (no v6 pointer model)
	PR1VM_RegisterBuiltin(vm, 388, (builtin_t)CSQC_Light_Nop_Ret0); // #388 __variant(__variant *dst, float ofs) memgetval - no-op in classic v6: typed pointer + runtime value type
	PR1VM_RegisterBuiltin(vm, 390, (builtin_t)CSQC_Light_Nop_Ret0); // #390 __variant*(__variant *base, float ofs) memptradd - no-op in classic v6: native pointer arithmetic (no v6 pointer)
	PR1VM_RegisterBuiltin(vm, 402, (builtin_t)CSQC_Findchain); // #402 entity(.string field, string match, .entity chainfield) findchain (DP_QC_FINDCHAIN)
	PR1VM_RegisterBuiltin(vm, 403, (builtin_t)CSQC_Findchainfloat); // #403 entity(.float fld, float match, .entity chainfield) findchainfloat (DP_QC_FINDCHAINFLOAT)
	PR1VM_RegisterBuiltin(vm, 444, (builtin_t)CSQC_Light_Nop_Ret0); // #444 float	search_begin(string pattern, float caseinsensitive, float quiet) (DP_QC_FS_SEARCH)
	PR1VM_RegisterBuiltin(vm, 446, (builtin_t)CSQC_Light_Nop_Ret0); // #446 float	search_getsize(float handle) (DP_QC_FS_SEARCH)
	PR1VM_RegisterBuiltin(vm, 449, (builtin_t)CSQC_Findflags); // #449 entity(entity start, .float fld, float match) findflags (DP_QC_FINDFLAGS)
	PR1VM_RegisterBuiltin(vm, 450, (builtin_t)CSQC_Findchainflags); // #450 entity(.float fld, float match, .entity chainfield) findchainflags (DP_QC_FINDCHAINFLAGS)
	PR1VM_RegisterBuiltin(vm, 451, (builtin_t)CSQC_Light_Nop_Ret0); // #451 float(entity ent, string tagname) gettagindex (DP_MD3_TAGSINFO)
	PR1VM_RegisterBuiltin(vm, 476, (builtin_t)CSQC_Strlennocol); // #476 float(string s) strlennocol
	PR1VM_RegisterBuiltin(vm, 487, (builtin_t)CSQC_Light_Nop_Ret0); // #487 float(string name)
	PR1VM_RegisterBuiltin(vm, 490, (builtin_t)CSQC_Light_Nop_Ret0); // #490 float(string name, float key, float eventtype)
	PR1VM_RegisterBuiltin(vm, 513, (builtin_t)CSQC_Light_Nop_Ret0); // #513 float(string uril, float id) uri_get
	PR1VM_RegisterBuiltin(vm, 535, (builtin_t)CSQC_Light_Nop_Ret0); // #535 float(string filename, strbuf bufhandle) buf_loadfile
	PR1VM_RegisterBuiltin(vm, 536, (builtin_t)CSQC_Light_Nop_Ret0); // #536 float(filestream filehandle, strbuf bufhandle, optional float startpos, optional float numstrings) buf_writefile
	PR1VM_RegisterBuiltin(vm, 537, (builtin_t)CSQC_Light_Nop_Ret0); // #537 float() bufstr_find
	PR1VM_RegisterBuiltin(vm, 611, (builtin_t)CSQC_Light_Nop_Ret0); // #611 float(float type) gethostcachevalue
	PR1VM_RegisterBuiltin(vm, 621, (builtin_t)CSQC_Light_Nop_Ret0); // #621 float(float fld, float hostnr) gethostcachenumber
	PR1VM_RegisterBuiltin(vm, 622, (builtin_t)CSQC_Light_Nop_Ret0); // #622 float(string key) gethostcacheindexforkey
	PR1VM_RegisterBuiltin(vm, 638, (builtin_t)CSQC_Cl_Rotatemoves); // #638 void(vector anglechange, optional float seat) CL_RotateMoves (FTE)
	PR1VM_RegisterBuiltin(vm, 640, (builtin_t)CSQC_Light_Nop_Ret0); // #640 float() V_CalcRefdef
	PR1VM_RegisterBuiltin(vm, 653, (builtin_t)CSQC_Light_Nop_Ret0); // #653 float() fexists
	PR1VM_RegisterBuiltin(vm, 740, (builtin_t)CSQC_Light_Nop_Ret0); // #740 float() controller_query
	// STRING stubs - type-correct no-op.
	PR1VM_RegisterBuiltin(vm, 112, (builtin_t)CSQC_Nop_Str); // #112 string(float fnum) fgets (FRIK_FILE)
	PR1VM_RegisterBuiltin(vm, 224, (builtin_t)CSQC_Strconv); // #224 string(float ccase, float redalpha, float redchars, string str, ...) strconv (FTE_STRINGS)
	PR1VM_RegisterBuiltin(vm, 266, (builtin_t)CSQC_Nop_Str); // #266 string(float skel, float bonenum) skel_get_bonename
	PR1VM_RegisterBuiltin(vm, 284, (builtin_t)CSQC_Nop_Str); // #284 string(float modidx, float framenum) frametoname
	PR1VM_RegisterBuiltin(vm, 285, (builtin_t)CSQC_Nop_Str); // #285 string(float modidx, float skin) skintoname
	PR1VM_RegisterBuiltin(vm, 292, (builtin_t)CSQC_Nop_Str); // #292 string(hashtable table, float idx) hash_getkey
	PR1VM_RegisterBuiltin(vm, 334, (builtin_t)CSQC_Modelnameforindex); // #334 string(float mdlindex) modelnameforindex (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 374, (builtin_t)CSQC_Particleeffectquery); // #374 string(float efnum, float body) particleeffectquery
	PR1VM_RegisterBuiltin(vm, 447, (builtin_t)CSQC_Nop_Str); // #447 string	search_getfilename(float handle, float num) (DP_QC_FS_SEARCH)
	PR1VM_RegisterBuiltin(vm, 477, (builtin_t)CSQC_Strdecolorize); // #477 string(string s) strdecolorize
	PR1VM_RegisterBuiltin(vm, 503, (builtin_t)CSQC_Nop_Str); // #503 string(string filename) whichpack
	PR1VM_RegisterBuiltin(vm, 510, (builtin_t)CSQC_Nop_Str); // #510 string(string in) uri_escape
	PR1VM_RegisterBuiltin(vm, 511, (builtin_t)CSQC_Nop_Str); // #511 string(string in) uri_unescape
	PR1VM_RegisterBuiltin(vm, 612, (builtin_t)CSQC_Nop_Str); // #612 string(float type, float hostnr) gethostcachestring
	PR1VM_RegisterBuiltin(vm, 624, (builtin_t)CSQC_Nop_Str); // #624 string() getextresponse
	PR1VM_RegisterBuiltin(vm, 625, (builtin_t)CSQC_Nop_Str); // #625 string(string dnsname, optional float defport) netaddress_resolve
	PR1VM_RegisterBuiltin(vm, 626, (builtin_t)CSQC_Nop_Str); // #626 string() getgamedirinfo
	PR1VM_RegisterBuiltin(vm, 639, (builtin_t)CSQC_Digest_Hex); // #639 string(string digest, string data, ...) digest_hex
	// VECTOR stubs - type-correct no-op.
	PR1VM_RegisterBuiltin(vm, 244, (builtin_t)CSQC_Bsp_Nop_Vec); // #244 vector(entity ent, float tagnum) rotatevectorsbytag
	PR1VM_RegisterBuiltin(vm, 269, (builtin_t)CSQC_Bsp_Nop_Vec); // #269 vector(float skel, float bonenum) skel_get_bonerel
	PR1VM_RegisterBuiltin(vm, 270, (builtin_t)CSQC_Bsp_Nop_Vec); // #270 vector(float skel, float bonenum) skel_get_boneabs
	PR1VM_RegisterBuiltin(vm, 310, (builtin_t)CSQC_Unproject); // #310 vector (vector v) unproject (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 311, (builtin_t)CSQC_Project); // #311 vector (vector v) project (EXT_CSQC)
	PR1VM_RegisterBuiltin(vm, 452, (builtin_t)CSQC_Bsp_Nop_Vec); // #452 vector(entity ent, float tagindex) gettaginfo (DP_MD3_TAGSINFO)
	PR1VM_RegisterBuiltin(vm, 493, (builtin_t)CSQC_Bsp_Nop_Vec); // #493 vector(string name)

	// Name-mapped `#0` builtins (engine slots; the module declares `= #0`).
	PR1VM_RegisterBuiltin(vm, CSQC_BUILTIN_CHECKBUILTIN, (builtin_t)CSQC_Checkbuiltin); // #0 checkbuiltin
	PR1VM_RegisterBuiltin(vm, CSQC_BUILTIN_NAMED_UNKNOWN, (builtin_t)CSQC_Named_Unknown); // #0 unknown name: call aborts
	PR1VM_RegisterBuiltin(vm, CSQC_BUILTIN_ARGC, (builtin_t)CSQC_Argc); // #0 float() argc
	PR1VM_RegisterBuiltin(vm, CSQC_BUILTIN_GETPLAYERKEYFLOAT, (builtin_t)CSQC_Getplayerkeyfloat); // #0 getplayerkeyfloat
	PR1VM_RegisterBuiltin(vm, CSQC_BUILTIN_SERVERKEYFLOAT, (builtin_t)CSQC_Serverkeyfloat); // #0 serverkeyfloat
	PR1VM_MarkBuiltinStub(vm, CSQC_BUILTIN_NAMED_UNKNOWN); // checkbuiltin reports unresolved names as 0

	// Mark no-op stubs so checkbuiltin/checkextension do not advertise them.
	// Shared stubs are detected by identity to the four no-op bodies; the
	// dedicated list covers single-use no-ops with their own wrapper.
	{
		static const int dedicated[] = { 6, 35, 40, 49, 67, 92, 110, 319, 329, 531, 533, 534, 603, 631, 632 };
		int i, j;
		for (i = 0; i < vm->numbuiltins; i++) {
			builtin_t fn = vm->builtins[i];
			if (!fn) {
				continue;
			}
			if (fn == (builtin_t)CSQC_Vmrest_Nop || fn == (builtin_t)CSQC_Nop_Str ||
				fn == (builtin_t)CSQC_Light_Nop_Ret0 || fn == (builtin_t)CSQC_Bsp_Nop_Vec) {
				PR1VM_MarkBuiltinStub(vm, i);
			}
			else {
				for (j = 0; j < (int)(sizeof(dedicated) / sizeof(dedicated[0])); j++) {
					if (i == dedicated[j]) {
						PR1VM_MarkBuiltinStub(vm, i);
						break;
					}
				}
			}
		}
	}
}
