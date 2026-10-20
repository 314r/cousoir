BUILD_DIR ?= build
PREFIX ?= $(HOME)/.local

ifeq ($(shell uname -s),Darwin)
RUN_EXECUTABLE = $(BUILD_DIR)/cousoir.app/Contents/MacOS/cousoir
else
RUN_EXECUTABLE = $(BUILD_DIR)/cousoir
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

# Rebuilds tests/data/library and its calibredb listing, and the book files of
# tests/data/import, with Calibre.
fixtures:
	tools/make_fixture_library.sh
	tools/make_import_fixtures.sh

# Records what Calibre computes, for the tests and the tables Cousoir takes
# from it: title and author sorts, folder and file names, the transliteration
# table and Calibre's "Unknown" and "Cover" in every language, and the OPF a
# conversion reads.
oracle:
	CALIBRE_OVERRIDE_LANG=fr calibre-debug -e tools/oracle/text_oracle.py -- tests/data/oracle/text.json
	CALIBRE_OVERRIDE_LANG=fr calibre-debug -e tools/oracle/paths_oracle.py -- tests/data/oracle/paths.json
	calibre-debug -e tools/oracle/tables_oracle.py -- .
	CALIBRE_OVERRIDE_LANG=fr calibre-debug -e tools/oracle/conversion_oracle.py -- tests/data/library tests/data/oracle/conversion.json
