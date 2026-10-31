/*
COMPILE.CPP

UAM's compiler, run on the console: GLSL in, a DKSH file out. Built with
UAM's own include paths (see the Makefile), so that main.cpp needs none of
them.
*/

#include "compiler_iface.h"

#include <stdio.h>
#include <stdlib.h>

extern "C" int probe_compile(int fragment, const char *glsl, const char *path)
{
	DekoCompiler compiler{fragment ? pipeline_stage_fragment : pipeline_stage_vertex};

	if (!compiler.CompileGlsl(glsl))
		return 0;
	compiler.OutputDksh(path);
	return 1;
}

/* the same, the DKSH into memory (as the game's host compiles: open_memstream,
port/switch/host/host_dk_compiler.cpp): *dksh is malloc'd, the caller frees
it; nothing touches the card */
extern "C" int probe_compile_memory(int fragment, const char *glsl, char **dksh, size_t *size)
{
	DekoCompiler compiler{fragment ? pipeline_stage_fragment : pipeline_stage_vertex};
	FILE *file;

	*dksh = NULL;
	*size = 0;
	if (!compiler.CompileGlsl(glsl))
		return 0;
	file = open_memstream(dksh, size);
	if (!file)
		return 0;
	compiler.WriteDksh(file);
	fclose(file);
	return *dksh != NULL;
}
