# Makefile — Vitte Light
# Usage:
#   make [all|debug|release]
#   make install PREFIX=/usr/local
#   make print-flags
#
# Outputs:
#   build/bin/vitli, build/bin/vitlc
#   build/lib/libvitl.a, build/lib/libvitl.so

# -------------------------------------------------------------------
# Toolchain + install dirs
# -------------------------------------------------------------------
CC      ?= cc
AR      ?= ar
PKG     ?= pkg-config

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
INCDIR  ?= $(PREFIX)/include/vitl

# -------------------------------------------------------------------
# Build mode
# -------------------------------------------------------------------
BUILD   ?= debug

CSTD    := -std=c17
WARN    := -Wall -Wextra -Wpedantic -Wundef -Wpointer-arith -Wformat=2 \
           -Wshadow -Wstrict-prototypes -Wno-unused-parameter
GEN     := -MMD -MP
BASEC   := $(CSTD) $(WARN) $(GEN) -fPIC

ifeq ($(BUILD),release)
  OPT := -O3 -DNDEBUG
else
  OPT := -O0 -g3 -fno-omit-frame-pointer
endif

CPPFLAGS += -Iincludes -Icore -Ilibraries
CFLAGS   += $(BASEC) $(OPT)
LDFLAGS  +=
LDLIBS   += -lm

# -------------------------------------------------------------------
# Platform libs
# -------------------------------------------------------------------
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
  LDLIBS += -ldl -lpthread -lrt
endif
ifeq ($(UNAME_S),Darwin)
  LDLIBS += -lpthread
endif

# -------------------------------------------------------------------
# pkg-config helpers
# -------------------------------------------------------------------
define have
$(shell $(PKG) --exists $(1) && echo 1 || echo 0)
endef
define use
CPPFLAGS += -D$(2) $(shell $(PKG) --cflags $(1))
LDLIBS   += $(shell $(PKG) --libs   $(1))
endef

# Core optional deps
ifneq ($(call have,openssl),0)
  $(eval $(call use,openssl,VL_HAVE_OPENSSL))
endif
ifneq ($(call have,sqlite3),0)
  $(eval $(call use,sqlite3,VL_HAVE_SQLITE3))
endif
ifneq ($(call have,zlib),0)
  $(eval $(call use,zlib,VL_HAVE_ZLIB))
endif
ifneq ($(call have,libcurl),0)
  $(eval $(call use,libcurl,VL_HAVE_CURL))
endif
ifneq ($(call have,libarchive),0)
  $(eval $(call use,libarchive,VL_HAVE_LIBARCHIVE))
endif
ifneq ($(call have,libavformat libavcodec libavutil libswresample libswscale),0)
  CPPFLAGS += -DVL_HAVE_FFMPEG
  CFLAGS   += $(shell $(PKG) --cflags libavformat libavcodec libavutil libswresample libswscale)
  LDLIBS   += $(shell $(PKG) --libs   libavformat libavcodec libavutil libswresample libswscale)
endif
ifneq ($(call have,freetype2),0)
  $(eval $(call use,freetype2,VL_HAVE_FREETYPE))
endif
ifneq ($(call have,harfbuzz),0)
  $(eval $(call use,harfbuzz,VL_HAVE_HARFBUZZ))
endif
ifneq ($(call have,libpcre2-8),0)
  $(eval $(call use,libpcre2-8,VL_HAVE_PCRE2))
endif
ifneq ($(call have,yyjson),0)
  $(eval $(call use,yyjson,VL_HAVE_YYJSON))
endif
# yaml can be yaml-0.1 or libyaml-0.1
ifneq ($(call have,libyaml-0.1),0)
  $(eval $(call use,libyaml-0.1,VL_HAVE_YAML))
else ifneq ($(call have,yaml-0.1),0)
  $(eval $(call use,yaml-0.1,VL_HAVE_YAML))
endif
ifneq ($(call have,libxml-2.0),0)
  $(eval $(call use,libxml-2.0,VL_HAVE_LIBXML2))
endif
ifneq ($(call have,lmdb),0)
  $(eval $(call use,lmdb,VL_HAVE_LMDB))
endif
ifneq ($(call have,hiredis),0)
  $(eval $(call use,hiredis,VL_HAVE_REDIS))
endif
ifneq ($(call have,libwebp),0)
  $(eval $(call use,libwebp,VL_HAVE_WEBP))
endif
ifneq ($(call have,libzmq),0)
  $(eval $(call use,libzmq,VL_HAVE_ZMQ))
endif
ifneq ($(call have,libssh2),0)
  $(eval $(call use,libssh2,VL_HAVE_SSH2))
endif
ifneq ($(call have,libssh),0)
  $(eval $(call use,libssh,VL_HAVE_SSH))
endif
ifneq ($(call have,libpq),0)
  $(eval $(call use,libpq,VL_HAVE_PG))
endif
ifneq ($(call have,mariadb),0)
  $(eval $(call use,mariadb,VL_HAVE_MYSQL))
else ifneq ($(call have,libmysqlclient),0)
  $(eval $(call use,libmysqlclient,VL_HAVE_MYSQL))
endif
ifneq ($(call have,rdkafka),0)
  $(eval $(call use,rdkafka,VL_HAVE_RDKAFKA))
endif
ifneq ($(call have,libffi),0)
  $(eval $(call use,libffi,VL_HAVE_FFI))
endif
ifneq ($(call have,libmosquitto),0)
  $(eval $(call use,libmosquitto,VL_HAVE_MQTT))
endif
ifneq ($(call have,rabbitmq),0)
  $(eval $(call use,rabbitmq,VL_HAVE_RABBITMQ_C))
endif
ifneq ($(call have,portaudio-2.0),0)
  $(eval $(call use,portaudio-2.0,VL_HAVE_PORTAUDIO))
endif
ifneq ($(call have,ncurses),0)
  $(eval $(call use,ncurses,VL_HAVE_NCURSES))
endif
ifneq ($(call have,sdl2),0)
  $(eval $(call use,sdl2,VL_HAVE_SDL2))
endif
ifneq ($(call have,libprotobuf-c),0)
  $(eval $(call use,libprotobuf-c,VL_HAVE_PROTOBUF_C))
endif
ifneq ($(call have,msgpack),0)
  $(eval $(call use,msgpack,VL_HAVE_MSGPACK))
endif

# Optional BLAKE3: pass BLAKE3_DIR=/path/to/BLAKE3 (provides blake3.o)
ifdef BLAKE3_DIR
  CPPFLAGS += -DVL_HAVE_BLAKE3 -I$(BLAKE3_DIR)
  LDLIBS   += $(BLAKE3_DIR)/blake3.o
endif

# -------------------------------------------------------------------
# Sources
# -------------------------------------------------------------------
SRC_CORE       := $(wildcard core/*.c)
SRC_LIBS       := $(wildcard libraries/*.c)
SRC_INTERP     := interpreter/vitli.c
SRC_COMPILER   := compiler/vitlc.c

OBJ_DIR        := build/obj
BIN_DIR        := build/bin
LIB_DIR        := build/lib

OBJ_CORE       := $(patsubst core/%.c,$(OBJ_DIR)/core/%.o,$(SRC_CORE))
OBJ_LIBS       := $(patsubst libraries/%.c,$(OBJ_DIR)/libraries/%.o,$(SRC_LIBS))
OBJ_INTERP     := $(patsubst interpreter/%.c,$(OBJ_DIR)/interpreter/%.o,$(SRC_INTERP))
OBJ_COMPILER   := $(patsubst compiler/%.c,$(OBJ_DIR)/compiler/%.o,$(SRC_COMPILER))

ALL_OBJS       := $(OBJ_CORE) $(OBJ_LIBS)
ALL_DEPS       := $(ALL_OBJS:.o=.d) $(OBJ_INTERP:.o=.d) $(OBJ_COMPILER:.o=.d)

# -------------------------------------------------------------------
# Targets
# -------------------------------------------------------------------
.PHONY: all debug release clean distclean install uninstall print-flags

all: $(BIN_DIR)/vitli $(BIN_DIR)/vitlc

debug:
	@$(MAKE) all BUILD=debug

release:
	@$(MAKE) all BUILD=release

# Binaries
$(BIN_DIR)/vitli: $(OBJ_INTERP) $(ALL_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDLIBS) -o $@

$(BIN_DIR)/vitlc: $(OBJ_COMPILER) $(ALL_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDLIBS) -o $@

# Libraries
$(LIB_DIR)/libvitl.a: $(ALL_OBJS)
	@mkdir -p $(LIB_DIR)
	$(AR) rcs $@ $^

$(LIB_DIR)/libvitl.so: $(ALL_OBJS)
	@mkdir -p $(LIB_DIR)
	$(CC) -shared $(CFLAGS) $^ $(LDFLAGS) $(LDLIBS) -o $@

# Patterns
$(OBJ_DIR)/core/%.o: core/%.c | $(OBJ_DIR)/core
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/libraries/%.o: libraries/%.c | $(OBJ_DIR)/libraries
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/interpreter/%.o: interpreter/%.c | $(OBJ_DIR)/interpreter
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/compiler/%.o: compiler/%.c | $(OBJ_DIR)/compiler
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# Dirs
$(OBJ_DIR)/core $(OBJ_DIR)/libraries $(OBJ_DIR)/interpreter $(OBJ_DIR)/compiler:
	@mkdir -p $@

# Deps
-include $(ALL_DEPS)

# Housekeeping
clean:
	rm -rf $(OBJ_DIR)

distclean: clean
	rm -rf $(BIN_DIR) $(LIB_DIR)

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN_DIR)/vitli $(DESTDIR)$(BINDIR)/
	install -m 0755 $(BIN_DIR)/vitlc $(DESTDIR)$(BINDIR)/
	install -d $(DESTDIR)$(INCDIR)
	cp -a includes/*.h core/*.h libraries/*.h $(DESTDIR)$(INCDIR) 2>/dev/null || true

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/vitli $(DESTDIR)$(BINDIR)/vitlc
	rm -rf $(DESTDIR)$(INCDIR)

print-flags:
	@echo "CPPFLAGS=$(CPPFLAGS)"
	@echo "CFLAGS=$(CFLAGS)"
	@echo "LDFLAGS=$(LDFLAGS)"
	@echo "LDLIBS=$(LDLIBS)"
