# The deko3d probe (source/main.cpp says what it answers), built as hello2
# is: the cross compiler named directly, elf2nro given an explicit --nacp.
#
# Needs devkitPro's deko3d (dkp-pacman -S deko3d) and UAM's compiler built
# for the console, which the port's own build makes: run `ninja switch-uam`
# at the top of the repository first.
export DEVKITPRO   := /opt/devkitpro
export DEVKITARM64 := $(DEVKITPRO)/devkitA64
export PORTLIBS    := $(DEVKITPRO)/portlibs/switch
export LIBNX       := $(DEVKITPRO)/libnx
export PATH        := $(DEVKITARM64)/bin:$(DEVKITPRO)/tools/bin:$(PATH)

TOP      := ../../../..
UAM_SRC  := $(TOP)/build/switch/third_party/uam
UAM_GEN  := $(TOP)/build/switch/uam/meson
UAM_LIB  := $(TOP)/build/switch/uam/libuam.a

PREFIX := aarch64-none-elf-
CC  := $(PREFIX)gcc
CXX := $(PREFIX)g++
LD  := $(PREFIX)ld
NM  := $(PREFIX)nm
OBJCOPY := $(PREFIX)objcopy

ARCH_FLAGS := -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS     := -I$(TOP)/port/switch/host -D__SWITCH__ -I$(LIBNX)/include -I$(PORTLIBS)/include -I$(PORTLIBS)/include/SDL2 $(ARCH_FLAGS) -O2 -Wall
# compile.cpp includes UAM's compiler, which needs its own paths and defines
# (as UAM's meson build gives them)
UAM_FLAGS  := -std=c++11 -DNDEBUG -DDESKTOP -D_USE_MATH_DEFINES -D_GNU_SOURCE -DHAVE_POSIX_MEMALIGN \
              -I$(UAM_SRC)/source -I$(UAM_SRC)/mesa-imported -I$(UAM_GEN)/mesa-imported \
              -I$(UAM_GEN)/mesa-imported/glsl -I$(UAM_GEN)/mesa-imported/glsl/glcpp
# (newlib's malloc lock timed: the memory corpus, main.cpp)
LDFLAGS    := -specs=$(LIBNX)/switch.specs -g $(ARCH_FLAGS) -Wl,--wrap=__malloc_lock
# SDL2 is devkitPro's, which is linked against Mesa, so Mesa comes along
# even though the probe opens no window with it
LIBS       := -L$(LIBNX)/lib -L$(PORTLIBS)/lib -lSDL2 -lEGL -lGLESv2 -lglapi -ldrm_nouveau \
              -ldeko3d -lnx -lpthread -lm

all: deko3d.nro

build/main.o: source/main.cpp
	@mkdir -p build
	$(CXX) -MMD -MP -MF build/main.d $(CFLAGS) -std=gnu++17 -c $< -o $@

# the host's allocator for UAM (port/switch/host/host_uam_alloc.c), outside
# the combined object below, so that its own calls reach newlib's malloc;
# switchable here, to time newlib's and it in one run
build/uam_alloc.o: $(TOP)/port/switch/host/host_uam_alloc.c $(TOP)/port/switch/host/host_uam_alloc.h
	@mkdir -p build
	$(CC) -MMD -MP -MF build/uam_alloc.d $(CFLAGS) -std=gnu11 -DHOST_UAM_ALLOC_SWITCHABLE -c $< -o $@

build/compile.o: source/compile.cpp $(UAM_LIB)
	@mkdir -p build
	$(CXX) -MMD -MP -MF build/compile.d $(CFLAGS) $(UAM_FLAGS) -c $< -o $@

$(UAM_LIB):
	@echo "UAM's library is missing: run 'ninja switch-uam' at the top of the repository" && false

# UAM is made of Mesa's GLSL compiler, and so is the Mesa that SDL2 brings
# in: linked side by side, the two clash (multiple definitions, and C++
# vtables in COMDAT groups of the same names, of which the linker keeps only
# the first). So UAM and compile.cpp become one object, every symbol it
# defines is renamed (which renames the groups too), and all but
# probe_compile and probe_compile_memory are made local. UAM's calls to
# pthread_mutex_lock go to main.cpp's probe_mutex_lock, which times the
# waits (the memory corpus).
build/uam_compiler.o: build/compile.o $(UAM_LIB)
	$(LD) -r -o build/uam_combined.o build/compile.o --whole-archive $(UAM_LIB) --no-whole-archive
	$(NM) --defined-only build/uam_combined.o | awk '{print $$3}' | \
		grep -v -e '^probe_compile$$' -e '^probe_compile_memory$$' -e '^\$$' | \
		sort -u | awk '{print $$1" uam_"$$1}' > build/uam_symbols.txt
	echo "pthread_mutex_lock probe_mutex_lock" >> build/uam_symbols.txt
	printf '%s\n' "malloc host_uam_malloc" "calloc host_uam_calloc" "realloc host_uam_realloc" \
		"free host_uam_free" "strdup host_uam_strdup" "strndup host_uam_strndup" "_Znwm host_uam_new" \
		"_Znam host_uam_new_array" "_ZdlPv host_uam_delete" "_ZdaPv host_uam_delete_array" >> build/uam_symbols.txt
	$(OBJCOPY) --redefine-syms=build/uam_symbols.txt --keep-global-symbol=probe_compile \
		--keep-global-symbol=probe_compile_memory --keep-global-symbol=probe_mutex_lock \
		$(foreach f,malloc calloc realloc free strdup strndup new delete new_array delete_array,--keep-global-symbol=host_uam_$(f)) \
		build/uam_combined.o $@

deko3d.elf: build/main.o build/uam_compiler.o build/uam_alloc.o
	$(CXX) $(LDFLAGS) -o $@ build/main.o build/uam_compiler.o build/uam_alloc.o $(LIBS) -Wl,-Map,build/deko3d.map

deko3d.nacp:
	nacptool --create "deko3d probe" "thelinkin3000" "0.1.0" $@

deko3d.nro: deko3d.elf deko3d.nacp
	elf2nro $< $@ --nacp=deko3d.nacp

clean:
	rm -rf build deko3d.elf deko3d.nacp deko3d.nro

-include build/*.d
