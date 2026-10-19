/*
pr1vm.h -- PR1 interpreter instance

Execution state (call/local-variable stacks, xfunction/xstatement) is
per-instance, in pr1vm_t. The "module" globals (progs/pr_functions/...) are
shared between PR1 and PR2 and read by sv_*.c, so they remain globals; the
server instance mirrors them through PR1VM_BindServer().

The interpreter is always built. The server binding (PR1VM_BindServer and the
PR_* wrappers) is built only with the server. The same API also serves the
client CSQC instance, whose loader and wiring live in the client CSQC module.
*/

#ifndef PR1VM_H
#define PR1VM_H

#include "progs.h"
#include <setjmp.h>

#define PR1VM_MAX_STACK	32
#define PR1VM_LOCALSTACK	2048

// Compile-time gate for the PR1VM/CSQC debug and test console commands
// (csqc_smoke, csqc_progscheck, pr1vm_test_error, ...) and the bound-predicate
// unit tests. Off by default; uncomment to enable.
//#define CSQC_DEBUG

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
	// Client CSQC: world (edict 0) is writable only while the
	// CSQC_Init/CSQC_WorldLoaded callbacks run. Always false for the server
	// instance, which gates world writes on state instead.
	qbool world_writable;
	void *game_edicts; // entvars base for STOREP_*/EDICT_TO_PROG

	// Module dialect field-offset map: NULL = identity (QW progs, CSQC
	// csprogs), otherwise a remap table (NQ progs). Set on (re)load.
	const int *fieldofs_patch;

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
	// Parallel to builtins: a set byte marks the builtin at that number as a
	// no-op placeholder, so checkbuiltin/checkextension do not report it as
	// implemented. NULL/clear = real implementation. Client VM only (the server
	// leaves it NULL); set by the engine, never by csprogs.
	byte *builtin_stub;
	int builtin_stub_size;
	// Current call: arg count + trace flag.
	int argc;
	qbool trace;

	// Per-instance string tables, held as pointers to the owner's storage:
	//   server instance -> the global pr_strtbl/pr_newstrtbl/num_prstr
	//     (bound in PR1VM_BindServer);
	//   client instance -> its own arrays (bound on load).
	// PR1VM_Get/SetString use only these, with no VM-type condition.
	char **strtbl;
	char **newstrtbl;
	int *numstr;

	// Host interface: callbacks receive a ready string.
	void (*host_error)(pr1vm_t *vm, const char *msg);
	void (*host_print)(pr1vm_t *vm, const char *msg);
	void *host_udata;

	// Optional string accessor for the interpreter string ops. The client VM
	// installs a bounded one for untrusted csprogs; the server leaves it NULL
	// and uses PR1VM_GetString, which must not bound positive offsets since map
	// strings live beyond progs->numstrings.
	char *(*get_string)(pr1vm_t *vm, int num);

	// Optional OP_STATE handler. NULL (server) uses the classic fixed
	// nextthink/frame/think layout; the client CSQC VM installs one that
	// resolves the module's own field/global offsets.
	void (*stateop)(pr1vm_t *vm, float frame, func_t func);

	// Optional hook run when an outermost call (depth 0) returns normally. NULL
	// for the server; the client VM collects unreferenced temp strings here.
	void (*host_postcall)(pr1vm_t *vm);

	// Abort-stack: when abortbuf_valid is set (client VM), PR_RunError unwinds
	// here instead of continuing the faulting statement. abortbuf points at the
	// *outermost* active frame's stack-local jmp_buf (nested calls on the same VM
	// reuse it, so an error unwinds the whole VM); NULL when no frame is active.
	// context_prev_* remember the classic pr_globals/g_active that were active
	// before attach, to restore on abnormal unwind / UnLoad while attached.
	jmp_buf *abortbuf;
	qbool abortbuf_valid;
	qbool context_saved;
	float *context_prev_globals;
	pr1vm_t *context_prev_active;
};

// Instance PR1 is currently executing inside; NULL outside a call.
pr1vm_t *PR1VM_Active(void);

// Server instance (sv_pr1vm) -- default target of the PR_* wrappers.
pr1vm_t *PR1VM_Server(void);

// Zero the instance.
void PR1VM_Reset(pr1vm_t *vm);
// Fill the server mirrors from shared globals and sv.*. Exec state is not touched:
// BindServer may run on a nested (recursive) PR_ExecuteProgram.
void PR1VM_BindServer(pr1vm_t *vm);
// Detach the instance from the module: restore the classic context if still
// attached, free the builtin tables and zero everything except host_error,
// host_print and host_udata.
void PR1VM_UnLoad(pr1vm_t *vm);

// If this instance is the active one, put the classic pr_globals/g_active back
// to the context saved on attach. No-op otherwise.
void PR1VM_RestoreContext(pr1vm_t *vm);

// Byte-swap header and lumps and fill the instance mirrors. Does no
// version/CRC validation; that is the caller's job.
void PR1VM_LoadData(pr1vm_t *vm, dprograms_t *hdr);
// Server: instance mirrors -> shared "module" globals (read by PR2/sv_*.c).
void PR1VM_CommitServer(pr1vm_t *vm);

// Resolve by name on the instance (ED_Find* use the shared server globals).
dfunction_t *PR1VM_FindFunction(pr1vm_t *vm, const char *name);
int PR1VM_FindGlobal(pr1vm_t *vm, const char *name);
char *PR1VM_GetString(pr1vm_t *vm, int num);
void PR1VM_SetString(pr1vm_t *vm, string_t *address, char *s);

// Register a builtin by number. The table grows to num+1 slots; unfilled slots
// are NULL, and calling one is a runtime error.
void PR1VM_RegisterBuiltin(pr1vm_t *vm, int num, builtin_t fn);

// Client CSQC VM: flag the builtin at num as a no-op placeholder (the stub
// table grows like the builtin table). IsBuiltinStub is false for unset or
// out-of-range numbers.
void PR1VM_MarkBuiltinStub(pr1vm_t *vm, int num);
qbool PR1VM_IsBuiltinStub(pr1vm_t *vm, int num);

#ifdef CSQC_DEBUG
// Debug: provoke PR_RunError on the server instance (pr1vm_test_error).
void PR1VM_TestError_f(void);

// Debug: unit-test the client-VM bound predicates (pr1vm_test_guards).
void PR1VM_TestGuards_f(void);

// Debug: shared pass/fail counter used by engine-side bound predicate tests
// (PR1VM_TestGuards_f and the client-side ent_of group in csqc_progscheck).
void PR1VM_GuardCheck(const char *name, qbool ok, int *pass, int *fail);
#endif

int PR1VM_EnterFunction(pr1vm_t *vm, dfunction_t *f);
int PR1VM_LeaveFunction(pr1vm_t *vm);
void PR1VM_ExecuteProgram(pr1vm_t *vm, func_t fnum);

#endif /* PR1VM_H */
