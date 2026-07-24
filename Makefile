LBITS := $(shell getconf LONG_BIT)
ifeq ($(LBITS),64)
	CFLAGS += -m64 -fPIC
	SOURCES = HDE/hde64.c
	SOURCES_NOPY = HDE/hde64.c
	SUFFIX = .x64
else
	CFLAGS += -m32 -fPIC
	SOURCES = HDE/hde32.c
	SOURCES_NOPY =  HDE/hde32.c
	SUFFIX = .x86
endif

BINDIR = bin
CC = gcc
CFLAGS += -shared -std=gnu11 -pthread
LDFLAGS_NOPY += -ldl
LDFLAGS += $(shell (python3-config --libs --embed || python3-config --libs) | grep lpython)
SOURCES_NOPY += dllmain.c commands.c simple_hook.c hooks.c misc.c maps_parser.c trampoline.c patches.c demos.c
SOURCES += dllmain.c commands.c python_embed.c python_dispatchers.c simple_hook.c hooks.c misc.c maps_parser.c trampoline.c patches.c demos.c
OBJS = $(SOURCES:.c=.o)
OBJS_DEBUG = $(SOURCES:.c=.o)
OBJS_NOPY = $(SOURCES_NOPY:.c=.o)
OUTPUT = $(BINDIR)/minqlxtended$(SUFFIX).so
OUTPUT_DEBUG = $(BINDIR)/minqlxtended$(SUFFIX)_debug.so
OUTPUT_NOPY = $(BINDIR)/minqlxtended_nopy.so
PYMODULE = $(BINDIR)/minqlxtended.zip
PYMODULE_DEBUG = $(BINDIR)/minqlxtended_debug.zip
PYFILES = $(wildcard python/minqlxtended/*.py)

.PHONY: depend clean

all: CFLAGS += $(shell python3-config --includes) -O2 -Wall
all: VERSION := MINQLXTENDED_VERSION=\"$(shell python3 python/version.py)\"
all: $(OUTPUT) $(PYMODULE)
	@echo Done!

debug: CFLAGS += $(shell python3-config --includes) -gdwarf-2 -Wall -O0 -fvar-tracking -DDEBUG
debug: VERSION := MINQLXTENDED_VERSION=\"$(shell python3 python/version.py -d)\"
debug: $(OUTPUT_DEBUG) $(PYMODULE_DEBUG)
	@echo Done!

nopy: CFLAGS += -Wall -DNOPY -O2
nopy: VERSION := MINQLXTENDED_VERSION=\"$(shell git describe --long --tags --dirty --always)-nopy\"
nopy: $(OUTPUT_NOPY)
	@echo Done!

nopy_debug: CFLAGS += -gdwarf-2 -Wall -O0 -DNOPY
nopy_debug: VERSION := MINQLXTENDED_VERSION=\"$(shell git describe --long --tags --dirty --always)-nopy\"
nopy_debug: $(OUTPUT_NOPY)
	@echo Done!

$(OUTPUT): $(OBJS)
	$(CC) $(CFLAGS) -D$(VERSION) -o $(OUTPUT) $(OBJS) $(LDFLAGS)

$(OUTPUT_DEBUG): $(OBJS_DEBUG)
	$(CC) $(CFLAGS) -D$(VERSION) -o $(OUTPUT_DEBUG) $(OBJS_DEBUG) $(LDFLAGS)

$(OUTPUT_NOPY): $(OBJS_NOPY)
	$(CC) $(CFLAGS) -D$(VERSION) -o $(OUTPUT_NOPY) $(OBJS_NOPY) $(LDFLAGS_NOPY)

$(PYMODULE): $(PYFILES)
	@python3 -m zipfile -c $(PYMODULE) python/minqlxtended

$(PYMODULE_DEBUG): $(PYFILES)
	@python3 -m zipfile -c $(PYMODULE_DEBUG) python/minqlxtended

.c.o:
	$(CC) $(CFLAGS) -D$(VERSION) -c $< -o $@

clean:
	@echo Cleaning...
	@$(RM) *.o *~ $(OUTPUT) $(OUTPUT_NOPY)
	@$(RM) HDE/*.o HDE/*~ $(OUTPUT) $(OUTPUT_NOPY)
	@$(RM) $(PYMODULE)
	@echo Done!
