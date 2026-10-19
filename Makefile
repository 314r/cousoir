BUILD_DIR ?= build
PREFIX ?= $(HOME)/.local

ifeq ($(shell uname -s),Darwin)
RUN_EXECUTABLE = $(BUILD_DIR)/reliure.app/Contents/MacOS/reliure
else
RUN_EXECUTABLE = $(BUILD_DIR)/reliure
endif

.PHONY: all build install clean run test fixtures oracle

all: build

build:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD_DIR) --parallel

install: build
	cmake --install $(BUILD_DIR) --prefix $(PREFIX)

clean:
	cmake --build $(BUILD_DIR) --target clean

run: build
	"$(RUN_EXECUTABLE)"

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

# Rebuilds tests/data/library and its calibredb listing with Calibre.
fixtures:
	tools/make_fixture_library.sh

# Records Calibre's title and author sorts for tests/calibre_text_test.cpp.
oracle:
	CALIBRE_OVERRIDE_LANG=fr calibre-debug -e tools/oracle/text_oracle.py -- tests/data/oracle/text.json
