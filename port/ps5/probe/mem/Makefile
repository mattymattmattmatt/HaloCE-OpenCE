# The PS5 memory probe (main.c says what it asks), built with the PS5 payload
# SDK (v0.42, the version PS5_Vulkan pins).
#
#   make                     builds mem.elf
#   make test PS5_HOST=...   sends it to the console's ELF loader
PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

CFLAGS := -O2 -g -Wall -Werror

all: mem.elf

main.o: main.c
	$(CC) $(CFLAGS) -c $< -o $@

payload.o: payload.S
	$(CC) -c $< -o $@

mem.elf: main.o payload.o
	$(CC) -o $@ $^

clean:
	rm -f main.o payload.o mem.elf

test: mem.elf
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $<

.PHONY: all clean test
