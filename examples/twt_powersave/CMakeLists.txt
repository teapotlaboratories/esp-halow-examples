# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.16)

# The HaLow component (mm-esp32-halow) is vendored at the repo root, shared by every
# example, rather than pulled from the ESP Component Registry — the registry build of
# morsemicro/halow does not carry the features these examples demonstrate.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

# The MM6108 firmware component is GENERATED under the build dir from
# vendor/morse-firmware (no firmware blobs are committed). See
# cmake/mm-fw-gen/morse_firmware.cmake.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/mm-fw-gen/morse_firmware.cmake")
morse_firmware_generate_component()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(twt_powersave)
