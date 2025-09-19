# =========================================
# VitteLight - Makefile (v0.1.0)
# =========================================

# ---- Projet / version ----
APP          ?= vitte-cli
APP2         ?= vitlc           # autre binaire (compiler)
ORG          ?= vitte-light
VERSION      ?= 0.1.0
DESC         ?= "VitteLight VM tooling & CLI"

# ---- Répertoires ----
SRC_DIRS     := core interpreter compiler
BUILD_DIR    ?= build
DIST_DIR     ?= dist
INCLUDE_DIRS := core .
PREFIX       ?= /usr/local
BINDIR       ?= $(PREFIX)/bin
LIBDIR       ?= $(PREFIX)/lib
INCLUDEDIR   ?= $(PREFIX)/include/vittelight
PKGCFGDIR    ?= $(LIBDIR)/pkgconfig
FORMULADIR   ?= Formula

# ---- Détection plateforme ----
UNAME_S := $(shell uname -s)
# Windows MSYS/MinGW/Cygwin ?
ifeq ($(findstring MINGW,$(UNAME_S))$(findstring MSYS,$(UNAME_S))$(findstring CYGWIN,$(UNAME_S)),)
  # Unix (macOS/Linux)
  ifeq ($(UNAME_S),Darwin)
    OS            := macos
    EXE           :=
    SHARED_EXT    := dylib
    SHARED_FLAG   := -dynamiclib
    LD_FRAMEWORKS := -framework CoreFoundation
    SHA256        := shasum -a 256
  else
    OS            := linux
    EXE           :=
    SHARED_EXT    := so
    SHARED_FLAG   := -shared
    LD_FRAMEWORKS :=
    SHA256        := sha256sum
  endif
  MKDIR_P   := mkdir -p
  COPY_FILE := install -m 0644
  COPY_BIN  := install -m 0755
  RM        := rm -f
  RMDIR_R   := rm -rf
else
  # Windows (MSYS2/MinGW/Cygwin)
  OS            := windows
  EXE           := .exe
  SHARED_EXT    := dll
  SHARED_FLAG   := -shared
  LD_FRAMEWORKS :=
  SHA256        := sha256sum
  MKDIR_P   := mkdir -p
  COPY_FILE := cp
  COPY_BIN  := cp
  RM        := rm -f
  RMDIR_R   := rm -rf
endif

# Choix de sed (gsed si dispo, sinon sed)
SED := $(shell command -v gsed 2>/dev/null || command -v sed)

# ---- Compilateur / Flags ----
CC      ?= cc
AR      ?= ar
RANLIB  ?= ranlib

WARN    := -Wall -Wextra -Wpedantic
VIS     := -fvisibility=hidden
CSTD    := -std=c17
OPT_DBG := -O0 -g3
OPT_REL := -O3 -DNDEBUG
DEFS    := -DAPI_BUILD -D_FILE_OFFSET_BITS=64

INCFLAGS := $(addprefix -I,$(INCLUDE_DIRS))

# Liens
LDLIBS_BASE := -lm -lpthread
ifeq ($(OS),macos)
  LDLIBS := $(LDLIBS_BASE) $(LD_FRAMEWORKS)
else
  LDLIBS := $(LDLIBS_BASE) -ldl
endif

# ---- Sources (auto) ----
CORE_SRCS      := $(wildcard core/*.c)
INTERP_SRCS    := $(wildcard interpreter/*.c)
COMPILER_SRCS  := $(wildcard compiler/*.c)
ALL_SRCS       := $(CORE_SRCS) $(INTERP_SRCS) $(COMPILER_SRCS)

# ---- Objets ----
OBJ_DIR  := $(BUILD_DIR)/obj
BIN_DIR  := $(BUILD_DIR)/bin
LIB_DIR  := $(BUILD_DIR)/lib

OBJS      := $(patsubst %.c,$(OBJ_DIR)/%.o,$(ALL_SRCS))
CORE_OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(CORE_SRCS))

# ---- Binaries / Libraries ----
BIN_APP     := $(BIN_DIR)/$(APP)$(EXE)
BIN_APP2    := $(BIN_DIR)/$(APP2)$(EXE)
LIB_STATIC  := $(LIB_DIR)/libvittelight.a
LIB_SHARED  := $(LIB_DIR)/libvittelight.$(SHARED_EXT)

# ---- Profils ----
CFLAGS_DEBUG   := $(CSTD) $(WARN) $(VIS) $(DEFS) $(INCFLAGS) $(OPT_DBG)
CFLAGS_RELEASE := $(CSTD) $(WARN) $(VIS) $(DEFS) $(INCFLAGS) $(OPT_REL)

# Profil courant (modifiable via `make MODE=release`)
MODE ?= debug
ifeq ($(MODE),release)
  CFLAGS := $(CFLAGS_RELEASE)
else
  CFLAGS := $(CFLAGS_DEBUG)
endif

# =========================================
# Règles principales
# =========================================
.PHONY: all debug release clean distclean install uninstall test format lint \
        lib static shared pkgconfig dist tar gz zip check env \
        brew brew-tap brew-formula brew-archive

all: env lib $(BIN_APP) $(BIN_APP2)

debug:
	@$(MAKE) MODE=debug all

release:
	@$(MAKE) MODE=release all

env:
	@$(MKDIR_P) $(BUILD_DIR) $(BIN_DIR) $(LIB_DIR) $(DIST_DIR)
	@:

# ---- Libs ----
lib: static shared

static: $(LIB_STATIC)
shared: $(LIB_SHARED)

$(LIB_STATIC): $(CORE_OBJS) | $(LIB_DIR)
	@echo "AR  $@"
	@$(AR) rcs $@ $(CORE_OBJS)
	@$(RANLIB) $@

$(LIB_SHARED): $(CORE_OBJS) | $(LIB_DIR)
	@echo "LD  $@"
	@$(CC) $(SHARED_FLAG) -o $@ $(CORE_OBJS) $(LDLIBS)

# ---- Binaries ----
# On lie le CLI principal contre la lib statique + l’objet code.o (si présent)
$(BIN_APP): $(LIB_STATIC) $(OBJ_DIR)/core/code.o | $(BIN_DIR)
	@echo "LD  $@"
	@$(CC) -o $@ $(OBJ_DIR)/core/code.o $(LIB_STATIC) $(LDLIBS)

# Binaire 2 si la source existe
$(BIN_APP2): $(LIB_STATIC) $(OBJ_DIR)/interpreter/vitlc.o | $(BIN_DIR)
	@if [ -f interpreter/vitlc.c ]; then \
	  echo "LD  $@"; \
	  $(CC) -o $@ $(OBJ_DIR)/interpreter/vitlc.o $(LIB_STATIC) $(LDLIBS); \
	else \
	  echo "skip $(BIN_APP2) (interpreter/vitlc.c absent)"; \
	fi

# ---- Compilation objets ----
$(OBJ_DIR)/%.o: %.c
	@$(MKDIR_P) $(dir $@)
	@echo "CC  $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# ---- Install / Uninstall ----
install: all pkgconfig
	@echo "INSTALL bins → $(DESTDIR)$(BINDIR)"
	@$(MKDIR_P) $(DESTDIR)$(BINDIR)
	@[ -f $(BIN_APP) ]  && $(COPY_BIN)  $(BIN_APP)  $(DESTDIR)$(BINDIR)/$(APP)$(EXE)  || true
	@[ -f $(BIN_APP2) ] && $(COPY_BIN)  $(BIN_APP2) $(DESTDIR)$(BINDIR)/$(APP2)$(EXE) || true
	@echo "INSTALL libs → $(DESTDIR)$(LIBDIR)"
	@$(MKDIR_P) $(DESTDIR)$(LIBDIR)
	@[ -f $(LIB_STATIC) ] && $(COPY_FILE) $(LIB_STATIC) $(DESTDIR)$(LIBDIR)/ || true
	@[ -f $(LIB_SHARED) ] && $(COPY_FILE) $(LIB_SHARED) $(DESTDIR)$(LIBDIR)/ || true
	@echo "INSTALL headers → $(DESTDIR)$(INCLUDEDIR)"
	@$(MKDIR_P) $(DESTDIR)$(INCLUDEDIR)
	@cp -a core/*.h $(DESTDIR)$(INCLUDEDIR)/

uninstall:
	@$(RM)       $(DESTDIR)$(BINDIR)/$(APP)$(EXE) $(DESTDIR)$(BINDIR)/$(APP2)$(EXE)
	@$(RM)       $(DESTDIR)$(LIBDIR)/libvittelight.a
	@$(RM)       $(DESTDIR)$(LIBDIR)/libvittelight.$(SHARED_EXT)
	@$(RMDIR_R)  $(DESTDIR)$(INCLUDEDIR)
	@$(RM)       $(DESTDIR)$(PKGCFGDIR)/vittelight.pc

# ---- Pkg-config ----
pkgconfig: $(DIST_DIR)/vittelight.pc
	@$(MKDIR_P) $(DESTDIR)$(PKGCFGDIR)
	@install -m 0644 $(DIST_DIR)/vittelight.pc $(DESTDIR)$(PKGCFGDIR)/

$(DIST_DIR)/vittelight.pc:
	@$(MKDIR_P) $(DIST_DIR)
	@echo "prefix=$(PREFIX)"                              >  $@
	@echo "exec_prefix=\$${prefix}"                       >> $@
	@echo "libdir=\$${prefix}/lib"                        >> $@
	@echo "includedir=\$${prefix}/include"                >> $@
	@echo ""                                              >> $@
	@echo "Name: vittelight"                              >> $@
	@echo "Description: $(DESC)"                          >> $@
	@echo "Version: $(VERSION)"                           >> $@
	@echo "Libs: -L\$${libdir} -lvittelight"              >> $@
	@echo "Cflags: -I\$${includedir}/vittelight"          >> $@

# ---- QA ----
format:
	@clang-format -i $(shell ls **/*.c **/*.h 2>/dev/null || true)

lint:
	@cppcheck --std=c11 --enable=warning,style,performance --quiet . || true

test:
	@echo "(TODO) add unit tests"
	@:

bench:
	@$(BIN_APP) bench 1048576 200 || true

# ---- Archives (release) ----
tar: dist/$(ORG)-$(VERSION).tar.gz
gz:  tar
zip: dist/$(ORG)-$(VERSION).zip

dist/$(ORG)-$(VERSION).tar.gz:
	@$(MKDIR_P) dist
	@git archive --format=tar --prefix=$(ORG)-$(VERSION)/ HEAD | gzip -9 > $@
	@$(SHA256) $@

dist/$(ORG)-$(VERSION).zip:
	@$(MKDIR_P) dist
	@git archive --format=zip --prefix=$(ORG)-$(VERSION)/ HEAD > $@
	@$(SHA256) $@

# ---- Nettoyage ----
clean:
	@rm -rf $(BUILD_DIR)

distclean: clean
	@rm -rf $(DIST_DIR) $(FORMULADIR)

