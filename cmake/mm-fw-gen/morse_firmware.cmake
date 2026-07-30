# SPDX-License-Identifier: Apache-2.0
#
# Makes the Morse Micro firmware available to the build WITHOUT committing any
# firmware files under components/. The firmware "component" is a template that
# lives next to this helper (cmake/mm-fw-gen/firmware/); at configure time it is
# COPIED into the build tree and that copy converts the upstream ELF in
# vendor/morse-firmware (the single source of truth) to .mbin -> .o under
# build/<app>. See cmake/mm-fw-gen/firmware/CMakeLists.txt for the conversion.
#
# Usage — from an app's top-level CMakeLists, BEFORE include(project.cmake):
#     set(EXTRA_COMPONENT_DIRS "...")
#     include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/mm-fw-gen/morse_firmware.cmake")
#     morse_firmware_generate_component()
#     include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Dir of this helper (cmake/mm-fw-gen), captured at include time.
set(MM_FW_GEN_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Copy the `firmware` component template into the build tree and add it to
# EXTRA_COMPONENT_DIRS so IDF discovers it as the `firmware` component (which
# satisfies halow's `morsemicro/firmware` dependency). Call before project().
function(morse_firmware_generate_component)
    set(_dst_parent "${CMAKE_BINARY_DIR}/mm-fw-gen")
    # file(COPY src DESTINATION dir) copies the `firmware` dir INTO _dst_parent,
    # i.e. -> ${_dst_parent}/firmware/{CMakeLists.txt,Kconfig,idf_component.yml}.
    file(COPY "${MM_FW_GEN_DIR}/firmware" DESTINATION "${_dst_parent}")

    # Re-run configure (and thus re-copy) if the template changes.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${MM_FW_GEN_DIR}/firmware/CMakeLists.txt"
        "${MM_FW_GEN_DIR}/firmware/Kconfig"
        "${MM_FW_GEN_DIR}/firmware/idf_component.yml")

    list(APPEND EXTRA_COMPONENT_DIRS "${_dst_parent}")
    set(EXTRA_COMPONENT_DIRS "${EXTRA_COMPONENT_DIRS}" PARENT_SCOPE)
    message(STATUS "morse firmware: component copied to ${_dst_parent}/firmware")
endfunction()
