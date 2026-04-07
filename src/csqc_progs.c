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

// csqc_progs.c -- Client-side QuakeC VM using the shared PR1 interpreter

#include "quakedef.h"
#include "progs.h"
#include "csqc_progs.h"
#include "csqc_progs_internal.h"
#include <setjmp.h>

// ---------------------------------------------------------------------------
// CSQC context instance
// ---------------------------------------------------------------------------
static pr_context_t csqc_pr_context;

// Entry points
static func_t csqc_func_init;
static func_t csqc_func_shutdown;
static func_t csqc_func_consolecmd;

static qbool csqc_loaded;
static byte *csqc_progdata;

// Error recovery
static jmp_buf csqc_abort;

// Cvar
static cvar_t csqc_enabled = { "csqc_enabled", "1" };

// ---------------------------------------------------------------------------
// Error handler for CSQC context (longjmp instead of Sys_Error)
// ---------------------------------------------------------------------------
static void CSQC_ErrorHandler(pr_context_t *ctx, const char *msg)
{
	ctx->depth = 0;
	ctx->localstack_used = 0;
	longjmp(csqc_abort, 1);
}

// ---------------------------------------------------------------------------
// Context switch helpers
// ---------------------------------------------------------------------------
static pr_context_t *CSQC_SwitchContext(void)
{
	pr_context_t *old = pr_current_ctx;
	pr_current_ctx = &csqc_pr_context;
	return old;
}

static void CSQC_RestoreContext(pr_context_t *old)
{
	pr_current_ctx = old;
}

// ---------------------------------------------------------------------------
// Find function by name (must be called with CSQC context active)
// ---------------------------------------------------------------------------
static func_t CSQC_FindFunction(const char *name)
{
	int i;
	dfunction_t *f;

	for (i = 1; i < progs->numfunctions; i++) {
		f = &pr_functions[i];
		if (!strcmp(PR1_GetString(f->s_name), name)) {
			return i;
		}
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Loader
// ---------------------------------------------------------------------------
qbool CSQC_Load(void)
{
	int i;
	int filesize;
	pr_context_t *old_ctx;

	if (csqc_loaded) {
		CSQC_Unload();
	}

	if (!csqc_enabled.value) {
		return false;
	}

	csqc_progdata = FS_LoadHeapFile("csaddon.dat", &filesize);
	if (!csqc_progdata) {
		return false;
	}

	if (filesize < (int)sizeof(dprograms_t)) {
		Com_Printf("CSQC: csaddon.dat is too small\n");
		Q_free(csqc_progdata);
		csqc_progdata = NULL;
		return false;
	}

	// Initialize context and switch to it so the compatibility macros work
	memset(&csqc_pr_context, 0, sizeof(csqc_pr_context));
	csqc_pr_context.error_handler = CSQC_ErrorHandler;
	old_ctx = CSQC_SwitchContext();

	progs = (dprograms_t *)csqc_progdata;

	// Byte-swap header
	for (i = 0; i < (int)sizeof(dprograms_t) / 4; i++) {
		((int *)progs)[i] = LittleLong(((int *)progs)[i]);
	}

	if (progs->version != PROG_VERSION && progs->version != 7) {
		Com_Printf("CSQC: csaddon.dat has wrong version (%d, expected %d or 7)\n",
			progs->version, PROG_VERSION);
		Q_free(csqc_progdata);
		csqc_progdata = NULL;
		CSQC_RestoreContext(old_ctx);
		return false;
	}

	// Bounds-check
	if (progs->ofs_statements + progs->numstatements * (int)sizeof(dstatement_t) > filesize ||
	    progs->ofs_functions + progs->numfunctions * (int)sizeof(dfunction_t) > filesize ||
	    progs->ofs_globaldefs + progs->numglobaldefs * (int)sizeof(ddef_t) > filesize ||
	    progs->ofs_fielddefs + progs->numfielddefs * (int)sizeof(ddef_t) > filesize ||
	    progs->ofs_strings + progs->numstrings > filesize ||
	    progs->ofs_globals + progs->numglobals * 4 > filesize) {
		Com_Printf("CSQC: csaddon.dat has corrupt header\n");
		Q_free(csqc_progdata);
		csqc_progdata = NULL;
		CSQC_RestoreContext(old_ctx);
		return false;
	}

	// Set up pointers (using compatibility macros → pr_current_ctx → csqc_pr_context)
	pr_functions  = (dfunction_t *)((byte *)progs + progs->ofs_functions);
	pr_strings    = (char *)progs + progs->ofs_strings;
	pr_globaldefs = (ddef_t *)((byte *)progs + progs->ofs_globaldefs);
	pr_fielddefs  = (ddef_t *)((byte *)progs + progs->ofs_fielddefs);
	pr_statements = (dstatement_t *)((byte *)progs + progs->ofs_statements);
	pr_globals    = (float *)((byte *)progs + progs->ofs_globals);

	// Byte-swap lumps
	for (i = 0; i < progs->numstatements; i++) {
		pr_statements[i].op = LittleShort(pr_statements[i].op);
		pr_statements[i].a  = LittleShort(pr_statements[i].a);
		pr_statements[i].b  = LittleShort(pr_statements[i].b);
		pr_statements[i].c  = LittleShort(pr_statements[i].c);
	}

	for (i = 0; i < progs->numfunctions; i++) {
		pr_functions[i].first_statement = LittleLong(pr_functions[i].first_statement);
		pr_functions[i].parm_start = LittleLong(pr_functions[i].parm_start);
		pr_functions[i].s_name = LittleLong(pr_functions[i].s_name);
		pr_functions[i].s_file = LittleLong(pr_functions[i].s_file);
		pr_functions[i].numparms = LittleLong(pr_functions[i].numparms);
		pr_functions[i].locals = LittleLong(pr_functions[i].locals);
	}

	for (i = 0; i < progs->numglobaldefs; i++) {
		pr_globaldefs[i].type = LittleShort(pr_globaldefs[i].type);
		pr_globaldefs[i].ofs  = LittleShort(pr_globaldefs[i].ofs);
		pr_globaldefs[i].s_name = LittleLong(pr_globaldefs[i].s_name);
	}

	for (i = 0; i < progs->numfielddefs; i++) {
		pr_fielddefs[i].type = LittleShort(pr_fielddefs[i].type);
		pr_fielddefs[i].ofs  = LittleShort(pr_fielddefs[i].ofs);
		pr_fielddefs[i].s_name = LittleLong(pr_fielddefs[i].s_name);
	}

	for (i = 0; i < progs->numglobals; i++) {
		((int *)pr_globals)[i] = LittleLong(((int *)pr_globals)[i]);
	}

	// Init CSQC builtins into this context
	CSQC_InitBuiltins(&csqc_pr_context);

	// Find entry points
	csqc_func_init       = CSQC_FindFunction("CSQC_Init");
	csqc_func_shutdown   = CSQC_FindFunction("CSQC_Shutdown");
	csqc_func_consolecmd = CSQC_FindFunction("CSQC_ConsoleCommand");

	csqc_loaded = true;
	Com_DPrintf("CSQC: csaddon.dat loaded (%d bytes)\n", filesize);

	// Call CSQC_Init
	if (csqc_func_init) {
		if (setjmp(csqc_abort)) {
			Com_Printf("CSQC: error during CSQC_Init, unloading\n");
			CSQC_RestoreContext(old_ctx);
			CSQC_Unload();
			return false;
		}
		G_FLOAT(OFS_PARM0) = 1.0f;
		PR_SetTmpString(&G_INT(OFS_PARM1), "ezquake");
		G_FLOAT(OFS_PARM2) = 1.0f;
		PR_ExecuteProgram(csqc_func_init);
	}
	else {
		Com_DPrintf("CSQC: CSQC_Init not found\n");
	}

	CSQC_RestoreContext(old_ctx);
	return true;
}

void CSQC_Unload(void)
{
	pr_context_t *old_ctx;

	if (!csqc_loaded) {
		return;
	}

	if (csqc_func_shutdown) {
		old_ctx = CSQC_SwitchContext();
		if (setjmp(csqc_abort) == 0) {
			PR_ExecuteProgram(csqc_func_shutdown);
		}
		CSQC_RestoreContext(old_ctx);
	}

	CSQC_ShutdownBuiltins(&csqc_pr_context);

	Q_free(csqc_progdata);
	csqc_progdata = NULL;
	memset(&csqc_pr_context, 0, sizeof(csqc_pr_context));
	csqc_func_init = 0;
	csqc_func_shutdown = 0;
	csqc_func_consolecmd = 0;
	csqc_loaded = false;

	Com_DPrintf("CSQC: unloaded\n");
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
qbool CSQC_IsLoaded(void)
{
	return csqc_loaded;
}

qbool CSQC_ConsoleCommand(const char *cmd_text)
{
	pr_context_t *old_ctx;
	qbool handled;

	if (!csqc_loaded || !csqc_func_consolecmd) {
		return false;
	}

	old_ctx = CSQC_SwitchContext();

	if (setjmp(csqc_abort)) {
		Com_Printf("CSQC: error in CSQC_ConsoleCommand, unloading\n");
		CSQC_RestoreContext(old_ctx);
		CSQC_Unload();
		return false;
	}

	PR_SetTmpString(&G_INT(OFS_PARM0), cmd_text);
	PR_ExecuteProgram(csqc_func_consolecmd);
	handled = (qbool)(G_FLOAT(OFS_RETURN) != 0);

	CSQC_RestoreContext(old_ctx);
	return handled;
}

// ---------------------------------------------------------------------------
// Console commands
// ---------------------------------------------------------------------------
static void CSQC_Load_f(void)
{
	if (csqc_loaded) {
		Com_Printf("CSQC: already loaded, use csqc_restart to reload\n");
		return;
	}
	if (CSQC_Load()) {
		Com_Printf("CSQC: csaddon.dat loaded\n");
	}
	else {
		Com_Printf("CSQC: failed to load csaddon.dat\n");
	}
}

static void CSQC_Unload_f(void)
{
	if (!csqc_loaded) {
		Com_Printf("CSQC: not loaded\n");
		return;
	}
	CSQC_Unload();
	Com_Printf("CSQC: unloaded\n");
}

static void CSQC_Restart_f(void)
{
	CSQC_Unload();
	if (CSQC_Load()) {
		Com_Printf("CSQC: csaddon.dat reloaded\n");
	}
	else {
		Com_Printf("CSQC: failed to load csaddon.dat\n");
	}
}

void CSQC_RegisterCvars(void)
{
	Cvar_Register(&csqc_enabled);
	Cmd_AddCommand("csqc_load", CSQC_Load_f);
	Cmd_AddCommand("csqc_unload", CSQC_Unload_f);
	Cmd_AddCommand("csqc_restart", CSQC_Restart_f);
}
