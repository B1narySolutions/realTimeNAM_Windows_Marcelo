.DEFAULT_GOAL := build
JOBS ?= 4
include make/models.mk

.PHONY: install build model upload program-dfu monitor format compiledb test live-mic list-devices check-devices render-wav check-files clean help
install:
	bash scripts/install.sh
	@if ls $(A2_MODELS) >/dev/null 2>&1; then \
	  echo 'Amp models already downloaded; skipping.'; \
	else \
	  bash scripts/download_models.sh; \
	fi
	$(MAKE) $(A2_HEADER)
	@echo 'Setup complete. Run make to build the firmware.'

build:
	@command -v arm-none-eabi-g++ >/dev/null || { echo 'Missing ARM compiler; run make install.'; exit 1; }
	@test -f libs/libDaisy/core/Makefile || { echo 'Run make install first.'; exit 1; }
	@test -f libs/NeuralAmpModelerCore/Dependencies/eigen/Eigen/Core || { echo 'Missing NAM dependencies; run make install.'; exit 1; }
	$(MAKE) -C libs/libDaisy -j$(JOBS) OPT=-Os
	$(MAKE) -f make/firmware.mk all

upload: build
	@command -v dfu-util >/dev/null || { echo 'Run make install first.'; exit 1; }
	$(MAKE) -f make/firmware.mk program-dfu

program-dfu: upload

monitor:
	@bash scripts/monitor.sh "$(PORT)"

test:
	$(MAKE) -f tests/Makefile test

model: $(A2_HEADER)

compiledb: model
	@command -v compiledb >/dev/null || { echo 'Missing compiledb; install it with brew install compiledb.'; exit 1; }
	@command -v arm-none-eabi-g++ >/dev/null || { echo 'Missing ARM compiler; run make install.'; exit 1; }
	@test -f libs/libDaisy/core/Makefile || { echo 'Run make install first.'; exit 1; }
	@test -f libs/NeuralAmpModelerCore/Dependencies/eigen/Eigen/Core || { echo 'Missing NAM dependencies; run make install.'; exit 1; }
	compiledb -n -f make -B -f make/firmware.mk compile-objects GCC_PATH="$$(dirname "$$(command -v arm-none-eabi-g++)")"

format:
	@command -v clang-format >/dev/null || { echo 'Missing clang-format; install it with brew install clang-format.'; exit 1; }
	find src tests -type f \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) -exec clang-format -i --style=file {} +

clean:
	rm -rf build
	@if test -f libs/libDaisy/Makefile; then $(MAKE) -C libs/libDaisy clean; fi

help:
	@echo 'make install  Full setup: tools, pinned libs, amp models, and the embedded header.'
	@echo 'make build    Build libDaisy and three-amp A2-Lite firmware.'
	@echo 'make model    Convert the downloaded A2-Lite models to embedded float data.'
	@echo 'make test     Run upstream A2 comparisons and sanitized host audio tests.'
	@echo 'make upload   Build and flash via USB; enter BOOT + RESET mode first.'
	@echo 'make monitor  Open USB serial in screen (optional PORT=/dev/cu.usbmodem...).'
	@echo 'make format   Format C/C++ files under src/ using .clang-format.'
	@echo 'make compiledb Generate compile_commands.json for clangd without building.'
	@echo 'make list-devices  Build and run the audio device lister to find IN/OUT indices.'
	@echo 'make live-mic IN=<in> OUT=<out> [AMP=1]  Build and run live mic mode.'
	@echo 'make render-wav IN=<in.wav> OUT=<out.wav> [AMP=1]  Render a WAV through an amp.'
	@echo 'make clean    Remove firmware and libDaisy build outputs.'

# Host tools: live mic monitoring through an amp model (needs PortAudio).
NAM_DIR = libs/NeuralAmpModelerCore
UNAME_S := $(shell uname -s)
ifneq (,$(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)))
EXE = .exe
endif

HOST_FLAGS = -std=c++17 -O2 -DNAM_SAMPLE_FLOAT -DNAM_USE_INLINE_GEMM
HOST_INCLUDES = -Ibuild/generated -Isrc -I$(NAM_DIR) -I$(NAM_DIR)/NAM \
  -I$(NAM_DIR)/Dependencies/eigen -I$(NAM_DIR)/Dependencies/nlohmann
NAM_SRC = src/audio/nam_audio.cpp src/models/amp_models.cpp src/models/a2_lite.cpp \
  src/audio/nam_processor.cpp $(NAM_DIR)/NAM/dsp.cpp
NAM_HEADERS = src/audio/nam_audio.h src/models/amp_models.h src/models/a2_lite.h \
  src/audio/nam_processor.h

# PortAudio flags: pkg-config if available, else Homebrew prefix, else plain -lportaudio.
BREW_PREFIX := $(shell brew --prefix 2>/dev/null)
PA_CFLAGS ?= $(shell pkg-config --cflags portaudio-2.0 2>/dev/null)
PA_LIBS ?= $(shell pkg-config --libs portaudio-2.0 2>/dev/null)
ifeq ($(strip $(PA_LIBS)),)
ifneq ($(strip $(BREW_PREFIX)),)
PA_CFLAGS = -I$(BREW_PREFIX)/include
PA_LIBS = -L$(BREW_PREFIX)/lib -lportaudio
else
PA_LIBS = -lportaudio
endif
endif

AMP ?= 1

list-devices: build/tools/list_devices$(EXE)
	./build/tools/list_devices$(EXE)

build/tools/list_devices$(EXE): tools/list_devices.cpp Makefile
	@mkdir -p $(@D)
	$(CXX) -std=c++17 $(PA_CFLAGS) tools/list_devices.cpp $(PA_LIBS) -o $@

live-mic: check-devices build/tools/live_mic$(EXE)
	./build/tools/live_mic$(EXE) $(AMP) $(IN) $(OUT)

check-devices:
	@test -n "$(IN)" -a -n "$(OUT)" || { \
	  echo 'Set input and output device indices, e.g. make live-mic IN=15 OUT=13 AMP=1'; \
	  echo 'Run make list-devices to look them up (indices shift when devices change).'; \
	  exit 1; }

build/tools/live_mic$(EXE): tools/live_mic.cpp $(NAM_SRC) $(NAM_HEADERS) $(A2_HEADER) Makefile
	@test -f $(NAM_DIR)/NAM/dsp.cpp || { echo 'Missing NAM Core; run make install.'; exit 1; }
	@mkdir -p $(@D)
	$(CXX) $(HOST_FLAGS) $(HOST_INCLUDES) $(PA_CFLAGS) tools/live_mic.cpp $(NAM_SRC) $(PA_LIBS) -o $@

render-wav: check-files build/tools/render_wav$(EXE)
	./build/tools/render_wav$(EXE) $(IN) $(OUT) $(AMP)

check-files:
	@test -n "$(IN)" -a -n "$(OUT)" || { \
	  echo 'Set input and output files, e.g. make render-wav IN=solo.wav OUT=fender.wav AMP=1'; \
	  exit 1; }

build/tools/render_wav$(EXE): tools/render_wav.cpp $(NAM_SRC) $(NAM_HEADERS) $(A2_HEADER) Makefile
	@test -f $(NAM_DIR)/NAM/dsp.cpp || { echo 'Missing NAM Core; run make install.'; exit 1; }
	@mkdir -p $(@D)
	$(CXX) $(HOST_FLAGS) $(HOST_INCLUDES) tools/render_wav.cpp $(NAM_SRC) -o $@
