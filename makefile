# CalcFront - TI-84 Plus CE build (needs the CE C toolchain, https://ce-programming.github.io/toolchain/)
# Profiling build (benchmark, timings shown in game): make CFLAGS="-Wall -Wextra -Oz -fno-inline-functions -fno-jump-tables -DCF_PERF"
NAME = CALCFRNT
DESCRIPTION = "CalcFront"
ICON = gfx/icon.png
COMPRESSED = YES
COMPRESSED_MODE = zx0
ARCHIVED = YES
HAS_PRINTF = NO

# No automatic inlining: inlining big functions into their callers makes the
# eZ80 code larger and spills more registers. Hot little helpers are marked
# always_inline (FORCE_INLINE) in the source instead.
CFLAGS = -Wall -Wextra -Oz -fno-inline-functions -fno-jump-tables
CXXFLAGS = -Wall -Wextra -Oz -fno-inline-functions -fno-jump-tables
LTOFLAGS = $(CFLAGS)

include $(shell cedev-config --makefile)
