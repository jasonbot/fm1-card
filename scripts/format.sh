#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
CLANG_FORMAT=${CLANG_FORMAT:-clang-format}
"$CLANG_FORMAT" -i \
  $(git ls-files 'components/fm1_core/**/*.cpp' 'components/fm1_core/**/*.hpp' \
                  'components/fm1_platform/**/*.cpp' 'components/fm1_platform/**/*.hpp' \
                  'components/fm1_ui/**/*.cpp' 'components/fm1_ui/**/*.hpp' \
                  'main/*.cpp' 'main/*.hpp' 'tests/*.cpp')
