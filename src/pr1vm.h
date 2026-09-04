/*
pr1vm.h -- PR1 interpreter instance

Execution state (call/local-variable stacks, xfunction/xstatement) is
per-instance, in pr1vm_t. The "module" globals (progs/pr_functions/...) are
shared between PR1 and PR2 and read by sv_*.c, so they remain globals; the
server instance mirrors them through PR1VM_BindServer().

PR1 runs under the CLIENTONLY gate (server build). The client instance API
is added together with its loader/wiring.
*/

#ifndef PR1VM_H
#define PR1VM_H

#include "progs.h"

#define PR1VM_MAX_STACK	32
#define PR1VM_LOCALSTACK	2048

typedef struct pr1vm_s pr1vm_t;

typedef struct
{
	int s;   // statement (return address)
	dfunction_t *f;
} pr1vm_stack_t;

struct pr1vm_s {
	// Mirrors of the "module" globals (the server uses shared symbols via
	// BindServer; a client instance gets its own buffers).
	dprograms_t *progs;
	dfunction_t *functions;
	ddef_t *fielddefs;
	ddef_t *globaldefs;
	dstatement_t *statements;
	char *strings;
	globalvars_t *global_struct;
	float *globals;
	int edict_size;  // bytes per entvars block

	// Edict model (server: sv.edicts / sv.game_edicts). state mirrors sv.state.
	edict_t *edicts;
	int num_edicts;
	int max_edicts;
	int state;
	void *game_edicts; // entvars base for STOREP_*/EDICT_TO_PROG

	// Execution state.
	pr1vm_stack_t stack[PR1VM_MAX_STACK];
	int depth;
	int localstack[PR1VM_LOCALSTACK];
	int localstack_used;
	dfunction_t *xfunction;
	int xstatement;

	// Builtin table, indexed by -first_statement.
	int numbuiltins;
	builtin_t *builtins;
	// Current call: arg count + trace flag.
	int argc;
	qbool trace;

	// Client (per-instance) dynamic strings. Not used by the server instance:
	// it delegates to the global tables (PR1_GetString/...), since PR2 and sv_*
	// read them.
	char *strtbl[MAX_PRSTR];
	char *newstrtbl[MAX_PRSTR];
	int numstr;

	// Host interface: callbacks receive a ready string.
	void (*host_error)(pr1vm_t *vm, const char *msg);
	void (*host_print)(pr1vm_t *vm, const char *msg);
	void *host_udata;
};

// Instance PR1 is currently executing inside; NULL outside a call.
pr1vm_t *PR1VM_Active(void);

// Server instance (sv_pr1vm) -- default target of the PR_* wrappers.
pr1vm_t *PR1VM_Server(void);

// Zero the instance and (for the server) fill the mirrors from the shared globals and sv.*.
void PR1VM_Reset(pr1vm_t *vm);
// Fill the server mirrors from shared globals and sv.*. Exec state is not touched:
// BindServer may run on a nested (recursive) PR_ExecuteProgram.
void PR1VM_BindServer(pr1vm_t *vm);
// Detach the instance from the module: restore the classic context if still
// attached, free the builtin tables and zero everything except host_error,
// host_print and host_udata.
void PR1VM_UnLoad(pr1vm_t *vm);

// Byte-swap header and lumps and fill the instance mirrors. Does no
// version/CRC validation; that is the caller's job.
void PR1VM_LoadData(pr1vm_t *vm, dprograms_t *hdr);
// Server: instance mirrors -> shared "module" globals (read by PR2/sv_*.c).
void PR1VM_CommitServer(pr1vm_t *vm);

// Client v7-secondary16 loader ("empty" extended). Returns false and
// prints the reason via Con_Printf (no SV_Error).
qbool PR1VM_LoadClientV7(pr1vm_t *vm, const byte *data, int filesize);

// Resolve by name on the instance (ED_Find* use the shared server globals).
dfunction_t *PR1VM_FindFunction(pr1vm_t *vm, const char *name);
int PR1VM_FindGlobal(pr1vm_t *vm, const char *name);
char *PR1VM_GetString(pr1vm_t *vm, int num);
void PR1VM_SetString(pr1vm_t *vm, string_t *address, char *s);

// Debug: csqc_smoke console command (registered in PR2_Init).
void PR1VM_CSQCSmoke_f(void);
// Debug: provoke PR_RunError on the server instance (pr1vm_test_error).
void PR1VM_TestError_f(void);

int PR1VM_EnterFunction(pr1vm_t *vm, dfunction_t *f);
int PR1VM_LeaveFunction(pr1vm_t *vm);
void PR1VM_ExecuteProgram(pr1vm_t *vm, func_t fnum);

#endif /* PR1VM_H */
