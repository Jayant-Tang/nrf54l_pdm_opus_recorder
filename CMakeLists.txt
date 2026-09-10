#
# Copyright (c) 2026
#
# SPDX-License-Identifier: Apache-2.0
#

cmake_minimum_required(VERSION 3.20.0)

# Opus codec and Ogg container: glue layers in modules/, sources in lib/
# (git submodules). Must be set before find_package(Zephyr).
set(ZEPHYR_EXTRA_MODULES
  ${CMAKE_CURRENT_SOURCE_DIR}/modules/opus
  ${CMAKE_CURRENT_SOURCE_DIR}/modules/ogg
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(learning_zephyr_pdm)

target_sources(app PRIVATE
  src/main.c
  src/opus_enc.c
)
target_sources_ifdef(CONFIG_FILE_SYSTEM app PRIVATE src/recorder.c)
target_sources_ifdef(CONFIG_BT app PRIVATE src/smp_bt.c)
