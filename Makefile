BUILD_DIR ?= build
PREFIX ?= $(HOME)/.local
# As many compilers at once as the cores and the free memory allow; make
# JOBS=n sets it.
JOBS ?= $(shell tools/build_jobs.sh)

ifeq ($(shell uname -s),Darwin)
RUN_EXECUTABLE = $(BUILD_DIR)/cousoir.app/Contents/MacOS/cousoir
else
RUN_EXECUTABLE = $(BUILD_DIR)/cousoir
endif

.PHONY: all build install clean run test fixtures oracle

all: build

build:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD_DIR) --parallel $(or $(JOBS),1)

install: build
	cmake --install $(BUILD_DIR) --prefix $(PREFIX)

clean:
	cmake --build $(BUILD_DIR) --target clean

run: build
	"$(RUN_EXECUTABLE)"

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

# Rebuilds tests/data/library and its calibredb listing, the book files of
# tests/data/import, and tests/data/library-custom, the same library with a
# custom column of each kind, with Calibre.
fixtures: build
	COUSOIR_TEST_TOOL=$(abspath $(BUILD_DIR))/cousoir-test-tool tools/make_fixture_library.sh
	COUSOIR_TEST_TOOL=$(abspath $(BUILD_DIR))/cousoir-test-tool tools/make_import_fixtures.sh
	tools/make_custom_library.sh

# Records what Calibre computes, for the tests and the tables Cousoir takes
# from it: title and author sorts, folder and file names, the transliteration
# table and Calibre's "Unknown" and "Cover" in every language, the OPF a
# conversion reads, how its search folds text, and what its templates read
# besides the books.
oracle:
	CALIBRE_OVERRIDE_LANG=fr calibre-debug -e tools/oracle/text_oracle.py -- tests/data/oracle/text.json
	CALIBRE_OVERRIDE_LANG=fr calibre-debug -e tools/oracle/paths_oracle.py -- tests/data/oracle/paths.json
	calibre-debug -e tools/oracle/tables_oracle.py -- .
	CALIBRE_OVERRIDE_LANG=fr calibre-debug -e tools/oracle/conversion_oracle.py -- tests/data/library tests/data/oracle/conversion.json
	CALIBRE_OVERRIDE_LANG=fr calibre-debug -e tools/oracle/search_oracle.py -- . tests/data/oracle/search.json
	calibre-debug -e tools/oracle/template_tables_oracle.py -- .
	calibre-debug -e tools/oracle/language_fixture.py -- tests/data/calibre-resources
	CALIBRE_OVERRIDE_LANG=en calibre-debug -e tools/oracle/source_oracle.py -- tests/data/oracle/source.json
