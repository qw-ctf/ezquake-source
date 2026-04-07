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

// csqc_progs_internal.h -- internal shared header between csqc_progs.c and csqc_builtins.c

#ifndef CSQC_PROGS_INTERNAL_H
#define CSQC_PROGS_INTERNAL_H

// CSQC builtin table management
void CSQC_InitBuiltins(pr_context_t *ctx);
void CSQC_ShutdownBuiltins(pr_context_t *ctx);

#endif // CSQC_PROGS_INTERNAL_H
