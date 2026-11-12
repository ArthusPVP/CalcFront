# CalcFront - TI-84 Plus CE build (needs the CE C toolchain, https://ce-programming.github.io/toolchain/)
# Profiling build (timings shown in game): make CFLAGS="-Wall -Wextra -Oz -DCF_PERF"
NAME = CALCFRNT
DESCRIPTION = "CalcFront"
COMPRESSED = YES
COMPRESSED_MODE = zx0
ARCHIVED = YES
HAS_PRINTF = NO

CFLAGS = -Wall -Wextra -Oz
CXXFLAGS = -Wall -Wextra -Oz

include $(shell cedev-config --makefile)
