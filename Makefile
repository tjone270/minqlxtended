PYTHON ?= python3
PYTHON_CONFIG ?= $(PYTHON)-config

LBITS := $(shell getconf LONG_BIT)
ifneq ($(LBITS),64)
$(error minqlxtended requires a 64-bit toolchain; there is no 32-bit Quake Live dedicated server support)
endif

CFLAGS += -m64 -fPIC
SOURCES = src/hook/HDE/hde64.c
SOURCES_NOPY = src/hook/HDE/hde64.c
SUFFIX = .x64

BINDIR = bin
BUILDDIR = build
CC = gcc
CFLAGS += -shared -std=gnu11 -pthread -Isrc
CFLAGS += $(EXTRA_CFLAGS)
LDFLAGS_NOPY += -ldl -Wl,--no-undefined
LDFLAGS += -ldl -Wl,--no-undefined $(shell $(PYTHON_CONFIG) --ldflags --embed | grep lpython)
COMMON_SOURCES = src/server/dllmain.c src/server/hooks.c src/server/commands.c \
                 src/server/misc.c src/server/maps_parser.c \
                 src/hook/simple_hook.c src/hook/trampoline.c src/hook/patches.c \
                 src/hook/protect.c \
                 src/features/demos.c src/features/profile.c
SOURCES_NOPY += $(COMMON_SOURCES)
SOURCES += $(COMMON_SOURCES) \
           src/features/reliable.c src/features/scoreboard.c src/features/game_events.c \
           src/features/console_command.c \
           src/python/python_embed.c src/python/python_dispatchers.c src/python/python_objects.c

# One object directory per target. The four sets of flags differ, and a shared directory
# would let stale -O0 debug objects link into the release .so.
OBJS = $(SOURCES:%.c=$(BUILDDIR)/rel/%.o)
OBJS_DEBUG = $(SOURCES:%.c=$(BUILDDIR)/dbg/%.o)
OBJS_NOPY = $(SOURCES_NOPY:%.c=$(BUILDDIR)/nopy/%.o)
OBJS_NOPY_DEBUG = $(SOURCES_NOPY:%.c=$(BUILDDIR)/nopydbg/%.o)

DEPS = $(OBJS:.o=.d) $(OBJS_DEBUG:.o=.d) $(OBJS_NOPY:.o=.d) $(OBJS_NOPY_DEBUG:.o=.d)
OUTPUT = $(BINDIR)/minqlxtended$(SUFFIX).so
OUTPUT_DEBUG = $(BINDIR)/minqlxtended$(SUFFIX)_debug.so
OUTPUT_NOPY = $(BINDIR)/minqlxtended_nopy.so
OUTPUT_NOPY_DEBUG = $(BINDIR)/minqlxtended_nopy_debug.so
PYMODULE = $(BINDIR)/minqlxtended.zip
PYMODULE_DEBUG = $(BINDIR)/minqlxtended_debug.zip
# py.typed ships in the zip as well, so editing it has to rebuild. The stub for the C module
# stays behind: _minqlxtended is a top-level module, and python/_minqlxtended.pyi is where a
# type checker pointed at python/ will find it.
PYFILES = $(wildcard python/minqlxtended/*.py python/minqlxtended/py.typed)
PYSTAGE = $(BUILDDIR)/pymodule/minqlxtended

.PHONY: all debug nopy nopy_debug clean

all: CFLAGS += $(shell $(PYTHON_CONFIG) --includes) -O2 -Wall
all: VERSION := MINQLXTENDED_VERSION=\"$(shell $(PYTHON) python/version.py)\"
all: $(OUTPUT) $(PYMODULE)
	@echo Done!

debug: CFLAGS += $(shell $(PYTHON_CONFIG) --includes) -gdwarf-2 -Wall -O0 -fvar-tracking -DDEBUG
debug: VERSION := MINQLXTENDED_VERSION=\"$(shell $(PYTHON) python/version.py -d)\"
debug: $(OUTPUT_DEBUG) $(PYMODULE_DEBUG)
	@echo Done!

nopy: CFLAGS += -Wall -DNOPY -O2
nopy: VERSION := MINQLXTENDED_VERSION=\"$(shell $(PYTHON) python/version.py)-nopy\"
nopy: $(OUTPUT_NOPY)
	@echo Done!

nopy_debug: CFLAGS += -gdwarf-2 -Wall -O0 -fvar-tracking -DNOPY -DDEBUG
nopy_debug: VERSION := MINQLXTENDED_VERSION=\"$(shell $(PYTHON) python/version.py -d)-nopy\"
nopy_debug: $(OUTPUT_NOPY_DEBUG)
	@echo Done!

$(OUTPUT): $(OBJS)
	$(CC) $(CFLAGS) -D$(VERSION) -o $(OUTPUT) $(OBJS) $(LDFLAGS)

$(OUTPUT_DEBUG): $(OBJS_DEBUG)
	$(CC) $(CFLAGS) -D$(VERSION) -o $(OUTPUT_DEBUG) $(OBJS_DEBUG) $(LDFLAGS)

$(OUTPUT_NOPY): $(OBJS_NOPY)
	$(CC) $(CFLAGS) -D$(VERSION) -o $(OUTPUT_NOPY) $(OBJS_NOPY) $(LDFLAGS_NOPY)

$(OUTPUT_NOPY_DEBUG): $(OBJS_NOPY_DEBUG)
	$(CC) $(CFLAGS) -D$(VERSION) -o $(OUTPUT_NOPY_DEBUG) $(OBJS_NOPY_DEBUG) $(LDFLAGS_NOPY)

$(PYMODULE): $(PYFILES)
	@$(RM) -r $(dir $(PYSTAGE))
	@mkdir -p $(PYSTAGE)
	@cp $(PYFILES) $(PYSTAGE)/
	@$(RM) $(PYMODULE)
	@$(PYTHON) -m zipfile -c $(PYMODULE) $(PYSTAGE)

$(PYMODULE_DEBUG): $(PYFILES)
	@$(RM) -r $(dir $(PYSTAGE))
	@mkdir -p $(PYSTAGE)
	@cp $(PYFILES) $(PYSTAGE)/
	@$(RM) $(PYMODULE_DEBUG)
	@$(PYTHON) -m zipfile -c $(PYMODULE_DEBUG) $(PYSTAGE)

$(BUILDDIR)/rel/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -D$(VERSION) -c $< -o $@

$(BUILDDIR)/dbg/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -D$(VERSION) -c $< -o $@

$(BUILDDIR)/nopy/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -D$(VERSION) -c $< -o $@

$(BUILDDIR)/nopydbg/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -D$(VERSION) -c $< -o $@

# Below the rules. On a first build the .d files do not exist yet and this is a no-op.
-include $(DEPS)

clean:
	@echo Cleaning...
	@$(RM) -r $(BUILDDIR)
	@$(RM) src/*.o src/*~ src/*/*.o src/*/*~ src/hook/HDE/*.o src/hook/HDE/*~
	@$(RM) $(OUTPUT) $(OUTPUT_DEBUG) $(OUTPUT_NOPY) $(OUTPUT_NOPY_DEBUG)
	@$(RM) $(PYMODULE) $(PYMODULE_DEBUG)
	@echo Done!
