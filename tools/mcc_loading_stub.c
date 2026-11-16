/* Converter tools/tests have no display. The real controller is tested separately. */
#include "../port/linux/game/mcc_loading.h"
/* Deterministic clock: standalone checks exercise the byte budget. */
unsigned long system_milliseconds(void) { return 0; }
int mcc_loading_begin(char const *name) { (void)name; return 0; }
void mcc_loading_stage(enum mcc_loading_stage stage) { (void)stage; }
void mcc_loading_update(enum mcc_loading_stage stage, unsigned long done, unsigned long total)
{ (void)stage; (void)done; (void)total; }
void mcc_loading_pulse(void) { }
void mcc_loading_end(int success) { (void)success; }

#include "../port/linux/game/mcc_runtime.h"

static uint32_t mcc_runtime_offset(struct mcc_runtime const *runtime, uint32_t address)
{
	uint32_t actual = (uint32_t)(uintptr_t)runtime->tags;
	if (runtime->tags && address - actual < runtime->capacity) return address - actual;
	if (address - runtime->report.tag_base < runtime->capacity) return address - runtime->report.tag_base;
	return 0xFFFFFFFFu;
}

uint32_t mcc_runtime_linked(struct mcc_runtime const *runtime, uint32_t address)
{
	uint32_t offset = mcc_runtime_offset(runtime, address);
	return offset == 0xFFFFFFFFu ? address : runtime->report.tag_base + offset;
}

void *mcc_runtime_actual(struct mcc_runtime const *runtime, uint32_t address)
{
	uint32_t offset = mcc_runtime_offset(runtime, address);
	return offset == 0xFFFFFFFFu ? NULL : runtime->tags + offset;
}
