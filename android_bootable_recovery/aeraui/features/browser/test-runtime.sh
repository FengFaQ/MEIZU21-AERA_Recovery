#!/bin/bash
set -euo pipefail
browser_sources=$(cd -- "$(dirname -- "$0")" && pwd)
xz_sources=${AERA_XZ_SOURCE:-/home/koaan/Desktop/AERA_16.0/external/xz-embedded}
test_output=${1:-/tmp/aera-browser-tests}
mkdir -p "$test_output"
objects=()
for source in xz_crc32.c xz_dec_stream.c xz_dec_lzma2.c xz_dec_bcj.c; do
  cc -O1 -g -fsanitize=address,undefined -DXZ_DEC_ARM64 \
    -I"$xz_sources/userspace" -I"$xz_sources/linux/include/linux" \
    -c "$xz_sources/linux/lib/xz/$source" -o "$test_output/$source.o"
  objects+=("$test_output/$source.o")
done
c++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$xz_sources/linux/include/linux" -I"$browser_sources/.." \
  -I"$browser_sources/prebuilt" \
  "$browser_sources/runtime_test.cpp" "${objects[@]}" -lcrypto -o "$test_output/runtime_test"
"$test_output/runtime_test" "$browser_sources/prebuilt/runtime.xz"
c++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$browser_sources/session.cpp" "$browser_sources/session_test.cpp" -o "$test_output/session_test"
"$test_output/session_test"
