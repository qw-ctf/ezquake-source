/*
Copyright (C) 2024 ezQuake team

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.
*/

// csqc_builtins.c -- Builtin functions for the client-side QuakeC VM

#include "quakedef.h"
#include "progs.h"
#include "csqc_progs.h"
#include "csqc_progs_internal.h"

// Uses G_FLOAT, G_INT, G_STRING, pr_argc etc. via pr_current_ctx compatibility macros.

// Local temp string buffer (pr_string_temp in pr_cmds.c is server-only)
#define MAX_PR_STRING_SIZE 2048
static char csqc_string_temp[MAX_PR_STRING_SIZE];
#define pr_string_temp csqc_string_temp

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static char *CSQC_VarString(int first)
{
	int i;
	static char out[2048];

	out[0] = 0;
	for (i = first; i < pr_argc; i++) {
		strlcat(out, G_STRING(OFS_PARM0 + i * 3), sizeof(out));
	}
	return out;
}

static tokenizecontext_t csqc_tokencontext;

// ---------------------------------------------------------------------------
// Builtin implementations (FTE-compatible numbering)
// ---------------------------------------------------------------------------
static void CSQC_random(void) {
	G_FLOAT(OFS_RETURN) = (rand() & 0x7fff) / ((float)0x7fff);
}

static void CSQC_dprint(void) {
	Com_DPrintf("%s", CSQC_VarString(0));
}

static void CSQC_ftos(void) {
	float v = G_FLOAT(OFS_PARM0);
	if (v == (int)v)
		snprintf(pr_string_temp, MAX_PR_STRING_SIZE, "%d", (int)v);
	else
		snprintf(pr_string_temp, MAX_PR_STRING_SIZE, "%5.1f", v);
	PR_SetTmpString(&G_INT(OFS_RETURN), pr_string_temp);
}

static void CSQC_rint(void) {
	float f = G_FLOAT(OFS_PARM0);
	G_FLOAT(OFS_RETURN) = (f > 0) ? (int)(f + 0.5) : (int)(f - 0.5);
}

static void CSQC_floor(void) {
	G_FLOAT(OFS_RETURN) = floor(G_FLOAT(OFS_PARM0));
}

static void CSQC_ceil(void) {
	G_FLOAT(OFS_RETURN) = ceil(G_FLOAT(OFS_PARM0));
}

static void CSQC_fabs(void) {
	G_FLOAT(OFS_RETURN) = fabs(G_FLOAT(OFS_PARM0));
}

static void CSQC_cvar(void) {
	G_FLOAT(OFS_RETURN) = Cvar_Value(G_STRING(OFS_PARM0));
}

static void CSQC_localcmd(void) {
	Cbuf_AddText(G_STRING(OFS_PARM0));
}

static void CSQC_cvar_set(void) {
	char *var_name = G_STRING(OFS_PARM0);
	char *val = G_STRING(OFS_PARM1);
	cvar_t *var = Cvar_Find(var_name);
	if (!var) {
		Com_Printf("CSQC cvar_set: variable %s not found\n", var_name);
		return;
	}
	Cvar_Set(var, val);
}

static void CSQC_stof(void) {
	G_FLOAT(OFS_RETURN) = Q_atof(G_STRING(OFS_PARM0));
}

static void CSQC_tokenize(void) {
	Cmd_TokenizeStringEx(&csqc_tokencontext, G_STRING(OFS_PARM0));
	G_FLOAT(OFS_RETURN) = Cmd_ArgcEx(&csqc_tokencontext);
}

static void CSQC_argc(void) {
	G_FLOAT(OFS_RETURN) = (float)Cmd_ArgcEx(&csqc_tokencontext);
}

static void CSQC_argv(void) {
	int num = (int)G_FLOAT(OFS_PARM0);
	if (num < 0 || num >= Cmd_ArgcEx(&csqc_tokencontext)) {
		PR_SetTmpString(&G_INT(OFS_RETURN), "");
	}
	else {
		snprintf(pr_string_temp, MAX_PR_STRING_SIZE, "%s", Cmd_ArgvEx(&csqc_tokencontext, num));
		PR_SetTmpString(&G_INT(OFS_RETURN), pr_string_temp);
	}
}

static void CSQC_checkextension(void) {
	G_FLOAT(OFS_RETURN) = 0;
}

static void CSQC_cvar_string(void) {
	cvar_t *var = Cvar_Find(G_STRING(OFS_PARM0));
	if (!var) {
		G_INT(OFS_RETURN) = 0;
		return;
	}
	strlcpy(pr_string_temp, var->string, MAX_PR_STRING_SIZE);
	PR_SetTmpString(&G_INT(OFS_RETURN), pr_string_temp);
}

static void CSQC_strlen(void) {
	G_FLOAT(OFS_RETURN) = (float)strlen(G_STRING(OFS_PARM0));
}

static void CSQC_strcat(void) {
	strlcpy(pr_string_temp, CSQC_VarString(0), MAX_PR_STRING_SIZE);
	PR_SetTmpString(&G_INT(OFS_RETURN), pr_string_temp);
}

static void CSQC_substr(void) {
	const char *s = G_STRING(OFS_PARM0);
	int start = (int)G_FLOAT(OFS_PARM1);
	int len = (int)G_FLOAT(OFS_PARM2);
	int l = (int)strlen(s);

	if (start >= l || !len || !*s) {
		PR_SetTmpString(&G_INT(OFS_RETURN), "");
		return;
	}
	if (start < 0) start = 0;
	if (len > l - start) len = l - start;
	if (len >= MAX_PR_STRING_SIZE) len = MAX_PR_STRING_SIZE - 1;

	memcpy(pr_string_temp, s + start, len);
	pr_string_temp[len] = '\0';
	PR_SetTmpString(&G_INT(OFS_RETURN), pr_string_temp);
}

static void CSQC_print(void) {
	Com_Printf("%s", CSQC_VarString(0));
}

static void CSQC_Fixme(void) {
	PR_RunError("unimplemented builtin");
}

// ---------------------------------------------------------------------------
// Builtin table
// ---------------------------------------------------------------------------
#define CSQC_BUILTIN_TABLE_SIZE 512

void CSQC_InitBuiltins(pr_context_t *ctx)
{
	int i;

	ctx->numbuiltins = CSQC_BUILTIN_TABLE_SIZE;
	ctx->builtins = (pr_builtin_t *)Q_calloc(ctx->numbuiltins, sizeof(pr_builtin_t));

	for (i = 0; i < ctx->numbuiltins; i++) {
		ctx->builtins[i] = CSQC_Fixme;
	}

	ctx->builtins[7]   = CSQC_random;
	ctx->builtins[25]  = CSQC_dprint;
	ctx->builtins[26]  = CSQC_ftos;
	ctx->builtins[36]  = CSQC_rint;
	ctx->builtins[37]  = CSQC_floor;
	ctx->builtins[38]  = CSQC_ceil;
	ctx->builtins[43]  = CSQC_fabs;
	ctx->builtins[45]  = CSQC_cvar;
	ctx->builtins[46]  = CSQC_localcmd;
	ctx->builtins[72]  = CSQC_cvar_set;
	ctx->builtins[81]  = CSQC_stof;
	ctx->builtins[84]  = CSQC_tokenize;
	ctx->builtins[85]  = CSQC_argc;
	ctx->builtins[86]  = CSQC_argv;
	ctx->builtins[99]  = CSQC_checkextension;
	ctx->builtins[103] = CSQC_cvar_string;
	ctx->builtins[114] = CSQC_strlen;
	ctx->builtins[115] = CSQC_strcat;
	ctx->builtins[116] = CSQC_substr;
	ctx->builtins[339] = CSQC_print;

	memset(&csqc_tokencontext, 0, sizeof(csqc_tokencontext));
}

void CSQC_ShutdownBuiltins(pr_context_t *ctx)
{
	Q_free(ctx->builtins);
	ctx->builtins = NULL;
	ctx->numbuiltins = 0;
}
