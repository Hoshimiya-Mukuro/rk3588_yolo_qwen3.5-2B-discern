# Makefile for nc2rtsp — USB Camera → RTSP Server
# Must be run from the project/ directory (where this Makefile lives).
# Quick start:
#   make                 # cross-compile with default toolchain
#   make native          # native build (host gcc)
#   make clean           # clean build artifacts
#   make V=1             # verbose

# ─── Toolchain ──────────────────────────────────────────────────────
CROSS   ?= /opt/gcc-arm/bin/aarch64-none-linux-gnu-
CC       = $(CROSS)gcc
CXX      = $(CROSS)g++
AR       = $(CROSS)ar
STRIP    = $(CROSS)strip --strip-unneeded

# ─── Directories (relative to project/ where Makefile is) ───────────
SRCDIR  := src
INCDIR  := include
LIBDIR  := lib
OUTDIR  := build
BINDIR  := $(OUTDIR)/bin
TARGET  := $(BINDIR)/nc2rtsp

# ─── Source Files (with relative paths) ─────────────────────────────
C_SRC   := $(SRCDIR)/uvc.c $(SRCDIR)/ringbuf.c $(SRCDIR)/encode.c $(SRCDIR)/decode.c \
           $(SRCDIR)/vlm_link.c $(SRCDIR)/image_drawing.c
CXX_SRC := $(SRCDIR)/devicesource.cpp $(SRCDIR)/rtsp_s.cpp $(SRCDIR)/yolo.cc $(SRCDIR)/yolo_postprocess.cc main.cpp

# ─── Object Files (flat structure in build/) ────────────────────────
C_OBJS   := $(OUTDIR)/uvc.o $(OUTDIR)/ringbuf.o $(OUTDIR)/encode.o $(OUTDIR)/decode.o \
            $(OUTDIR)/vlm_link.o $(OUTDIR)/image_drawing.o
CXX_OBJS := $(OUTDIR)/devicesource.o $(OUTDIR)/rtsp_s.o $(OUTDIR)/yolo.o $(OUTDIR)/yolo_postprocess.o $(OUTDIR)/main.o
ALL_OBJS := $(C_OBJS) $(CXX_OBJS)

# ─── Include Paths ──────────────────────────────────────────────────
INCLUDES := -I$(SRCDIR) \
            -I. \
            -I$(INCDIR)/live555 \
            -I$(INCDIR)/rockchip \
            -I$(INCDIR)/rga \
            -I$(INCDIR)/rknn \
            -I$(INCDIR)/jpeg

# ─── Libraries ──────────────────────────────────────────────────────
LIB_LIVE555 := -lliveMedia -lgroupsock -lBasicUsageEnvironment -lUsageEnvironment
LIB_RGA     := -lrga
LIB_RKNN    := -lrknnrt
LIB_SYS     := -lpthread -lrt -ldl

# 统一从项目 lib/ 链接（虚拟机侧已备好 .so 软链），交叉/原生只差工具链
LDFLAGS = $(LIB_LIVE555) -L$(LIBDIR) -Wl,--allow-multiple-definition \
          -lrockchip_mpp -lturbojpeg \
          $(LIB_RGA) $(LIB_RKNN) \
          $(LIB_SYS)

# ─── Compiler Flags ─────────────────────────────────────────────────
ifdef ENABLE_MPP
EXTRA_DEFS := -DENABLE_MPP
endif

BASE_CFLAGS   := -std=c11   -Wall -Wextra -O2 -g $(EXTRA_DEFS) $(INCLUDES)
BASE_CXXFLAGS := -std=c++11 -Wall -Wextra -O2 -g $(EXTRA_DEFS) $(INCLUDES)

# ─── Verbose ────────────────────────────────────────────────────────
V ?= 0
ifeq ($(V),1)
AT :=
else
AT := @
endif

# ─── Targets ────────────────────────────────────────────────────────
.PHONY: all native clean strip info libs

all: $(TARGET)

native: CROSS :=
native: all

# lib/ 下放的是带版本号的 .so（librga.so.2.1.0 / librockchip_mpp.so.0），
# 而 ld 的 -lrga / -lrockchip_mpp 需要无版本号的名字，这里自动补软链接（幂等）。
libs:
	$(AT)for pair in librga.so.2.1.0:librga.so librockchip_mpp.so.0:librockchip_mpp.so; do \
	    src=$${pair%%:*}; dst=$${pair##*:}; \
	    if [ -e "$(LIBDIR)/$$src" ] && [ ! -e "$(LIBDIR)/$$dst" ]; then \
	        ln -sf "$$src" "$(LIBDIR)/$$dst"; \
	        printf "  LINK  %s/%s -> %s\n" "$(LIBDIR)" "$$dst" "$$src"; \
	    fi; \
	done

$(TARGET): $(ALL_OBJS) | $(BINDIR) libs
	$(AT)printf "  LD    %s\n" $@
	$(CXX) $(ALL_OBJS) $(LDFLAGS) -o $@

# ─── Explicit Rules (relative paths, no VPATH needed) ───────────────
$(OUTDIR)/uvc.o: src/uvc.c src/uvc.h
	$(AT)mkdir -p $(OUTDIR)
	$(AT)printf "  CC    %s\n" $<
	$(CC) $(BASE_CFLAGS) -c $< -o $@

$(OUTDIR)/ringbuf.o: src/ringbuf.c src/ringbuf.h
	$(AT)mkdir -p $(OUTDIR)
	$(AT)printf "  CC    %s\n" $<
	$(CC) $(BASE_CFLAGS) -c $< -o $@

$(OUTDIR)/encode.o: src/encode.c src/encode.h src/decode.h src/test_time.h src/yolo.h src/image_drawing.h
	$(AT)mkdir -p $(OUTDIR)
	$(AT)printf "  CC    %s\n" $<
	$(CC) $(BASE_CFLAGS) -c $< -o $@

$(OUTDIR)/vlm_link.o: src/vlm_link.c src/vlm_link.h src/yolo.h
	$(AT)mkdir -p $(OUTDIR)
	$(AT)printf "  CC    %s\n" $<
	$(CC) $(BASE_CFLAGS) -c $< -o $@

$(OUTDIR)/image_drawing.o: src/image_drawing.c src/image_drawing.h src/common.h src/font.h
	$(AT)mkdir -p $(OUTDIR)
	$(AT)printf "  CC    %s\n" $<
	$(CC) $(BASE_CFLAGS) -c $< -o $@

$(OUTDIR)/decode.o: src/decode.c src/decode.h
	$(AT)mkdir -p $(OUTDIR)
	$(AT)printf "  CC    %s\n" $<
	$(CC) $(BASE_CFLAGS) -c $< -o $@

$(OUTDIR)/devicesource.o: src/devicesource.cpp src/devicesource.hh compat.h
	$(AT)mkdir -p $(OUTDIR)
	$(AT)printf "  CXX   %s\n" $<
	$(CXX) $(BASE_CXXFLAGS) -c $< -o $@

$(OUTDIR)/rtsp_s.o: src/rtsp_s.cpp src/rtsp_s.hh compat.h src/test_time.h
	$(AT)mkdir -p $(OUTDIR)
	$(AT)printf "  CXX   %s\n" $<
	$(CXX) $(BASE_CXXFLAGS) -c $< -o $@

$(OUTDIR)/main.o: main.cpp compat.h src/test_time.h
	$(AT)mkdir -p $(OUTDIR)
	$(AT)printf "  CXX   %s\n" $<
	$(CXX) $(BASE_CXXFLAGS) -c $< -o $@

$(OUTDIR)/yolo.o: src/yolo.cc src/yolo.h src/yolo_postprocess.h src/decode.h src/vlm_link.h
	$(AT)mkdir -p $(OUTDIR)
	$(AT)printf "  CXX   %s\n" $<
	$(CXX) $(BASE_CXXFLAGS) -c $< -o $@

$(OUTDIR)/yolo_postprocess.o: src/yolo_postprocess.cc src/yolo_postprocess.h
	$(AT)mkdir -p $(OUTDIR)
	$(AT)printf "  CXX   %s\n" $<
	$(CXX) $(BASE_CXXFLAGS) -c $< -o $@

$(BINDIR):
	$(AT)mkdir -p $(BINDIR)

strip: $(TARGET)
	$(AT)printf "  STRIP %s\n" $<
	$(STRIP) $(TARGET)

clean:
	rm -rf $(OUTDIR)

# ─── Diagnostic ─────────────────────────────────────────────────────
info:
	@echo "=== Build Configuration ==="
	@echo "CROSS     = $(CROSS)"
	@echo "CC        = $(CROSS)gcc"
	@echo "CXX       = $(CROSS)g++"
	@echo "CFLAGS    = $(BASE_CFLAGS)"
	@echo "CXXFLAGS  = $(BASE_CXXFLAGS)"
	@echo "LDFLAGS   = $(LDFLAGS)"
	@echo "OBJS      = $(ALL_OBJS)"
	@echo "TARGET    = $(TARGET)"
