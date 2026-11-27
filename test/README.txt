Tests run on Linux/macOS with gcc against the OBS 31.1.1 headers (libobs calls are stubbed).
L = path to obs-studio/libobs, /tmp/gen = generated obsconfig.h.

Capture Smoother filter:
  gcc -O2 -I../src -I$L -I/tmp/gen harness.c ../src/capture-smoother-filter.c ../src/sc-core.c stubs2.c \
      $L/util/bmem.c $L/util/dstr.c $L/util/utf8.c -lm -lpthread -o csh && ./csh
Motion Smoother filter (mock NVIDIA GPU):
  gcc -O2 -DSC_TEST_HARNESS -I../src -I$L -I/tmp/gen motion_harness.c ../src/motion-smoother-filter.c \
      ../src/sc-timing.c ../src/sc-flowmath.c stubs2.c $L/util/bmem.c $L/util/dstr.c $L/util/utf8.c \
      -lm -lpthread -o mh && ./mh
Retiming engine against reference vectors:
  gcc -O2 -I../src timing_test.c ../src/sc-timing.c -lm -o tt && ./tt vectors/*.csv
  SCT_FILL_ONLY=1 ./tt vectors_fill/*.csv
Flow math:
  gcc -O2 -I../src flowmath_test.c ../src/sc-flowmath.c -lm -o ft && ./ft
