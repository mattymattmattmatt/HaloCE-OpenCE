/*
LIBSCEAUDIOIN_STUB.C

The import stub for libSceAudioIn, which the payload SDK has none of: a
shared object whose SONAME names the module and whose global functions are
the ones the title imports, which is all ps5-native-tool reads of a stub
(PS5_Vulkan's tooling/native/elf_object.cpp, read_stub). Its code is never
run: the console binds the imports to the module, which
host_microphone_ps5.c loads first (sceSysmoduleLoadModuleInternal). A
title's sceKernelDlsym finds nothing in a system module, so the stub is
the way (port/ps5/PS5.md, phase 7, probe 2). tools/ps5_build.py builds it
as it builds the AGC stubs. The names are shadPS4's export table's
(src/core/libraries/audio/audioin.cpp).
*/

#include <stdint.h>

int32_t sceAudioInInit(void);
int32_t sceAudioInOpen(int32_t user, uint32_t type, uint32_t index, uint32_t length, uint32_t rate, uint32_t format);
int32_t sceAudioInInput(int32_t handle, void *buffer);
int32_t sceAudioInClose(int32_t handle);
int32_t sceAudioInGetSilentState(int32_t handle);

int32_t sceAudioInInit(void)
{
	return -1;
}

int32_t sceAudioInOpen(int32_t user, uint32_t type, uint32_t index, uint32_t length, uint32_t rate, uint32_t format)
{
	(void)user;
	(void)type;
	(void)index;
	(void)length;
	(void)rate;
	(void)format;
	return -1;
}

int32_t sceAudioInInput(int32_t handle, void *buffer)
{
	(void)handle;
	(void)buffer;
	return -1;
}

int32_t sceAudioInClose(int32_t handle)
{
	(void)handle;
	return -1;
}

int32_t sceAudioInGetSilentState(int32_t handle)
{
	(void)handle;
	return -1;
}
