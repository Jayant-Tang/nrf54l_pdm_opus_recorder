#
# Copyright (c) 2026
#
# SPDX-License-Identifier: Apache-2.0
#

cmake_minimum_required(VERSION 3.20.0)

# Opus codec: glue layer in modules/opus, sources in lib/opus (git submodule).
# Must be set before find_package(Zephyr).
set(ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/modules/opus)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(learning_zephyr_pdm)

target_sources(app PRIVATE
  src/main.c
)
