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

// csqc_progs.h -- minimal client-side QuakeC (csaddon.dat) support

#ifndef CSQC_PROGS_H
#define CSQC_PROGS_H

#include "q_shared.h"

void  CSQC_RegisterCvars(void);
qbool CSQC_Load(void);
void  CSQC_Unload(void);
qbool CSQC_ConsoleCommand(const char *cmd_text);
qbool CSQC_IsLoaded(void);

#endif // CSQC_PROGS_H
