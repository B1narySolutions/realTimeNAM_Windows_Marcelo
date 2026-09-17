.DEFAULT_GOAL := build
JOBS ?= 4
include make/models.mk

.PHONY: install build model upload program-dfu monitor format compiledb test clean help
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
	@echo 'make clean    Remove firmware and libDaisy build outputs.'
