#---------------------------------------------------------------------
# KeyHunt-Cuda-2 GNU Makefile (CPU + CUDA GPU)
# Usage:
#   make
#   make gpu=1
#   make gpu=1 CCAP=90
#   make gpu=1 multiarch=1
#   make gpu=1 debug=1
#   make info
#   make bench [BENCH_ARGS="..."] [BENCH_CMD="..."]
#---------------------------------------------------------------------

SHELL := /bin/sh
comma := ,

BIN    := KeyHunt
OBJDIR := obj

COMMON_SOURCES := \
	Base58.cpp Bech32.cpp Bloom.cpp Int.cpp IntGroup.cpp IntMod.cpp \
	KeyHunt.cpp Main.cpp Point.cpp Random.cpp SECP256K1.cpp Timer.cpp \
	GPU/GPUGenerate.cpp \
	hash/ripemd160.cpp hash/sha256.cpp hash/sha512.cpp \
	hash/ripemd160_sse.cpp hash/sha256_sse.cpp

GPU_SOURCES := GPU/GPUEngine.cu

COMMON_OBJECTS := $(addprefix $(OBJDIR)/,$(COMMON_SOURCES:.cpp=.o))
GPU_OBJECTS    := $(addprefix $(OBJDIR)/,$(GPU_SOURCES:.cu=.o))

ifeq ($(strip $(gpu)),)
OBJECTS := $(COMMON_OBJECTS)
else
OBJECTS := $(COMMON_OBJECTS) $(GPU_OBJECTS)
endif

CXX := g++

# --------------------------------------------------------------------
# CUDA toolkit auto-detect (only required for gpu=1)

NVCC_BIN := $(shell command -v nvcc 2>/dev/null)
ifdef NVCC_BIN
CUDA := $(shell dirname $$(dirname $(NVCC_BIN)))
else
CUDA_CANDIDATES := /usr/local/cuda /usr/local/cuda-13.1 /usr/local/cuda-13.0 /usr/local/cuda-12.4 /usr/local/cuda-12.3 \
                   /usr/local/cuda-12.2 /usr/local/cuda-12.1 /usr/local/cuda-12.0 /usr/local/cuda-11.8 /opt/cuda
CUDA := $(firstword $(foreach d,$(CUDA_CANDIDATES),$(if $(wildcard $(d)/bin/nvcc),$(d),)))
endif

ifeq ($(strip $(gpu)),)
else
ifeq ($(strip $(CUDA)),)
$(error [HATA] CUDA bulunamadi (gpu=1 icin nvcc gerekli))
endif
endif

ifdef CUDA
NVCC := $(CUDA)/bin/nvcc
CUDA_VERSION := $(shell $(NVCC) --version 2>/dev/null | sed -n 's/.*release \([0-9][0-9]*\.[0-9]\).*/\1/p' | head -1)
CUDA_INCDIR := $(firstword $(wildcard $(CUDA)/include) $(wildcard $(CUDA)/targets/x86_64-linux/include))
CUDA_LIBDIR := $(firstword $(wildcard $(CUDA)/lib64) $(wildcard $(CUDA)/targets/x86_64-linux/lib))
endif

# --------------------------------------------------------------------
# Compute capability auto-detect

ifndef CCAP
ifneq ($(strip $(gpu)),)
CCAP_LIST := $(shell nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | tr -d '.' | sort -u | tr '\n' ' ')
CCAP := $(firstword $(CCAP_LIST))
ifeq ($(strip $(CCAP)),)
CCAP := 75
$(warning [UYARI] GPU tespit edilemedi, varsayilan sm_75)
endif
endif
endif

ccap_clean := $(shell printf "%s" "$(CCAP)" | tr -d '.')

ifdef multiarch
ifeq ($(strip $(gpu)),)
NVCC_ARCH :=
else
NVCC_SUPPORTED_SM := $(shell $(NVCC) --list-gpu-code 2>/dev/null | sed -n 's/^sm_//p' | tr '\n' ' ')
MULTIARCH_TARGETS := 60 70 75 80 86 87 88 89 90 90a 100 103 110 120 121
NVCC_ARCH := $(strip $(foreach sm,$(MULTIARCH_TARGETS),$(if $(filter $(sm),$(NVCC_SUPPORTED_SM)),-gencode=arch=compute_$(sm)$(comma)code=sm_$(sm))))
ifeq ($(strip $(NVCC_ARCH)),)
$(error [HATA] NVCC multiarch listesi bos; destekli mimariler: $(NVCC_SUPPORTED_SM))
endif
endif
else
ifeq ($(strip $(gpu)),)
NVCC_ARCH :=
else
NVCC_ARCH := -gencode=arch=compute_$(ccap_clean),code=sm_$(ccap_clean)
endif
endif

# --------------------------------------------------------------------
# Compiler / linker flags

COMMON_WARN := -Wno-write-strings
COMMON_OPT  := -O3
COMMON_DBG  := -g
COMMON_ARCH := -mssse3
COMMON_STD  := -std=c++17

ifeq ($(strip $(debug)),)
BUILD_OPT := $(COMMON_OPT)
NVCC_DBG  :=
else
BUILD_OPT := $(COMMON_DBG)
NVCC_DBG  := -G -g
endif

CXXFLAGS := $(COMMON_STD) -m64 $(COMMON_ARCH) $(COMMON_WARN) $(BUILD_OPT) -I.
LDFLAGS  := -lpthread

ifneq ($(strip $(gpu)),)
CXXFLAGS += -DWITHGPU -I$(CUDA_INCDIR)
LDFLAGS  += -L$(CUDA_LIBDIR) -lcudart

ifeq ($(strip $(CUDA_INCDIR)),)
$(error [HATA] CUDA include dizini bulunamadi: $(CUDA))
endif
ifeq ($(strip $(CUDA_LIBDIR)),)
$(error [HATA] CUDA lib dizini bulunamadi: $(CUDA))
endif
endif

NVCCFLAGS := $(NVCC_ARCH) -DWITHGPU -I. --compile -m64
NVCCFLAGS += --use_fast_math
NVCCFLAGS += --maxrregcount=128
NVCCFLAGS += --ptxas-options=-v,-O3
NVCCFLAGS += -Xcompiler -O3,-fPIC,-march=native
NVCCFLAGS += $(NVCC_DBG)

# --------------------------------------------------------------------

.PHONY: all clean info bench

all: $(BIN)

$(BIN): $(OBJECTS)
	@echo "[LINK] $@"
	$(CXX) $(OBJECTS) $(LDFLAGS) -o $@

$(OBJDIR)/%.o: %.cpp | $(OBJDIR) $(OBJDIR)/GPU $(OBJDIR)/hash
	@echo "[CXX ] $<"
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJDIR)/GPU/GPUEngine.o: GPU/GPUEngine.cu | $(OBJDIR) $(OBJDIR)/GPU $(OBJDIR)/hash
	@echo "[NVCC] $<"
	$(NVCC) $(NVCCFLAGS) $< -o $@

$(OBJDIR):
	@mkdir -p $(OBJDIR)

$(OBJDIR)/GPU: | $(OBJDIR)
	@mkdir -p $(OBJDIR)/GPU

$(OBJDIR)/hash: | $(OBJDIR)
	@mkdir -p $(OBJDIR)/hash

info:
	@echo "=== KeyHunt Build Info ==="
	@echo "gpu             : $(if $(gpu),$(gpu),0)"
	@echo "debug           : $(if $(debug),$(debug),0)"
	@echo "multiarch       : $(if $(multiarch),$(multiarch),0)"
	@echo "CUDA            : $(if $(CUDA),$(CUDA),<not-detected>)"
	@echo "CUDA_VERSION    : $(if $(CUDA_VERSION),$(CUDA_VERSION),<unknown>)"
	@echo "CCAP            : $(if $(CCAP),$(CCAP),<none>)"
	@echo "CCAP_LIST       : $(if $(CCAP_LIST),$(CCAP_LIST),<none>)"
	@echo "NVCC_ARCH       : $(if $(NVCC_ARCH),$(NVCC_ARCH),<none>)"
	@echo "CUDA_LIBDIR     : $(if $(CUDA_LIBDIR),$(CUDA_LIBDIR),<none>)"
	@echo
	@command -v nvcc >/dev/null 2>&1 && nvcc --version || true
	@echo
	@command -v nvidia-smi >/dev/null 2>&1 && nvidia-smi --query-gpu=index,name,compute_cap --format=csv,noheader || true

bench: $(BIN)
	@echo "[BENCH] Smoke: --check"
	@./$(BIN) --check || exit $$?
	@echo
	@echo "[BENCH] Smoke: -l"
	@./$(BIN) -l || exit $$?
	@if [ -n "$(BENCH_CMD)" ]; then \
		echo; \
		echo "[BENCH] Running custom command: $(BENCH_CMD)"; \
		sh -lc "$(BENCH_CMD)"; \
	elif [ -n "$(BENCH_ARGS)" ]; then \
		echo; \
		echo "[BENCH] Running ./$(BIN) $(BENCH_ARGS)"; \
		./$(BIN) $(BENCH_ARGS); \
	else \
		echo; \
		echo "[BENCH] No BENCH_ARGS/BENCH_CMD supplied. Smoke tests completed."; \
	fi

clean:
	@echo "[CLEAN]"
	@rm -f $(COMMON_OBJECTS) $(GPU_OBJECTS) $(BIN)
