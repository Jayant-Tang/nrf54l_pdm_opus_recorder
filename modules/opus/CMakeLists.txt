# Opus Zephyr module glue.
#
# Sources come from lib/opus (xiph/opus submodule, currently v1.5.2) and are
# collected with file(GLOB): platform-specific code lives in subdirectories
# (celt/arm, celt/x86, silk/arm, dnn, ...) which a non-recursive glob never
# matches, so only plain C paths are built. If an upgrade adds a new
# top-level source directory, revisit this file.

if(CONFIG_OPUS)

set(OPUS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/opus)

if(NOT EXISTS ${OPUS_DIR}/include/opus.h)
  message(FATAL_ERROR
    "lib/opus submodule not checked out. Run: git submodule update --init")
endif()

zephyr_library_named(opus)

# Audio codecs suffer badly at -Os (the Zephyr default); -O2 measured
# roughly 2x faster for the float encoder on Cortex-M33.
zephyr_library_compile_options(-O2)

# Upstream opus is not warning-clean under Zephyr's default flags; silence
# the noisy ones for the library only (app code keeps full warnings).
zephyr_library_compile_options(
  -Wno-double-promotion
  -Wno-maybe-uninitialized
  -Wno-unused-variable
)

# Building without config.h: OPUS_BUILD is mandatory; USE_ALLOCA selects
# stack-based temporary allocation (opus requires exactly one of VAR_ARRAYS,
# USE_ALLOCA, NONTHREADSAFE_PSEUDOSTACK). picolibc provides <alloca.h> and
# lrint/lrintf (otherwise opus falls back to plain C casts in float_cast.h).
zephyr_library_compile_definitions(
  OPUS_BUILD
  USE_ALLOCA
  HAVE_ALLOCA_H
  HAVE_LRINT
  HAVE_LRINTF
)

# Public headers for the application (#include <opus.h>)
zephyr_include_directories(${OPUS_DIR}/include)

# Internal header search paths
zephyr_library_include_directories(
  ${OPUS_DIR}/celt
  ${OPUS_DIR}/silk
  ${OPUS_DIR}/src
)

# Fixed-point only build (see modules/opus/Kconfig for the rationale).
# CONFIGURE_DEPENDS: re-run the glob on every build so added/removed
# upstream files are picked up automatically.
file(GLOB OPUS_SOURCES CONFIGURE_DEPENDS
  ${OPUS_DIR}/src/*.c
  ${OPUS_DIR}/celt/*.c
  ${OPUS_DIR}/silk/*.c
  ${OPUS_DIR}/silk/fixed/*.c
)

# Drop host tools and demos that carry their own main()
list(FILTER OPUS_SOURCES EXCLUDE REGEX
  "(opus_demo|opus_compare|repacketizer_demo|opus_custom_demo)\\.c$")

zephyr_library_include_directories(${OPUS_DIR}/silk/fixed)

# OPUS_ARM_INLINE_*: Cortex-M33 (ARMv8-M Mainline DSP) supports the ARMv5E
# instructions (SMULBB/SMLAWB/...), enabling opus's header-only inline-asm
# fast paths. No NEON on M33, so the *_neon_intr sources stay excluded.
zephyr_library_compile_definitions(
  FIXED_POINT
  OPUS_ARM_INLINE_ASM
  OPUS_ARM_INLINE_EDSP
)

zephyr_library_sources(${OPUS_SOURCES})

endif() # CONFIG_OPUS
