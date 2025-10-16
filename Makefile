#############################
# Makefile for pg_running_stats (macOS + Linux)
#############################

EXTENSION   = pg_running_stats
MODULE_big  = pg_running_stats
OBJS        = running_stats.o
DATA        = pg_running_stats--1.0.sql
PGFILEDESC  = "pg_running_stats - mergeable running statistics (Welford/Chan)"

# Detect Homebrew prefix (macOS)
BREW_PREFIX := $(shell brew --prefix 2>/dev/null)

# If Homebrew found, include its include/lib dirs
ifdef BREW_PREFIX
PG_CPPFLAGS += -I$(BREW_PREFIX)/include
SHLIB_LINK  += -L$(BREW_PREFIX)/lib -Wl,-rpath,$(BREW_PREFIX)/lib
endif

# Standard PGXS build
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
