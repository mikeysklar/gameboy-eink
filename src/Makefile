# Native module build for gbeink (Game Boy on SSD1683 e-paper). OPT is appended after
# the dynruntime.mk include so the last -O wins over its -Os default.
#   make MPY_DIR=../../circuitpython ARCH=armv6m   -> gbeink.mpy
#   make host_test && ./host_test Tetris.gb        -> host checks, no MPY_DIR needed
ifeq ($(MAKECMDGOALS),host_test)
host_test: host_main.c gbeink.c peanut_gb_cgb.h
	cc -O2 -Wall -Wno-unused-function -Wno-unused-parameter -DHOST_TEST -o $@ host_main.c
else
MPY_DIR ?= ../../circuitpython
ARCH ?= armv6m
MOD = gbeink
# Cortex-M0+ has no divide: __aeabi_* and thumb1 case helpers come from libgcc
LINK_RUNTIME = 1
SRC = gbeink.c
BUILD = build/$(ARCH)
include $(MPY_DIR)/py/dynruntime.mk
OPT ?= -O2
CFLAGS += $(OPT)
endif
