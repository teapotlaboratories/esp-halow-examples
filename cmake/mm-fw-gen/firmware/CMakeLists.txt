# SPDX-License-Identifier: Apache-2.0
# Tracks ESP Component Registry: morsemicro/firmware 2.10.4-esp32-2
#
# Morse Micro firmware component (TEMPLATE).
#
# This file is COPIED into build/<app>/mm-fw-gen/firmware/ by
# morse_firmware_generate_component() (cmake/mm-fw-gen/morse_firmware.cmake) and
# runs there as the IDF component named `firmware`. It converts the upstream ELF
# in vendor/morse-firmware (the single source of truth) to Morse TLV .mbin at
# build time (convert-bin-to-mbin.py), then objcopy-wraps it into the
# firmware_binary_*/bcf_binary_* objects morselib's mmhal_wlan_binaries.c links
# against. Nothing firmware-related is committed under components/; the firmware
# version is whatever vendor/morse-firmware is pinned at.
#
# Because this runs as a COPY under build/<app>, repo paths are resolved from
# CMAKE_SOURCE_DIR (= the app dir firmware/<app>, so the repo root is two up).

idf_component_register()

# objcopy output description per IDF target (for wrapping the .mbin into a .o).
if(${IDF_TARGET} STREQUAL "esp32s3")
    set(BFDNAME "elf32-xtensa-le")
    set(ARCH "xtensa")
elseif(${IDF_TARGET} STREQUAL "esp32c6")
    set(BFDNAME "elf32-littleriscv")
    set(ARCH "riscv")
elseif(${IDF_TARGET} STREQUAL "esp32c3")
    set(BFDNAME "elf32-littleriscv")
    set(ARCH "riscv")
else()
    message(FATAL_ERROR "Target ${IDF_TARGET} not known. Exiting...")
endif()

# Repo root: the app's top-level CMakeLists is firmware/<app>/CMakeLists.txt, so
# CMAKE_SOURCE_DIR is firmware/<app> and the repo root is two levels up.
set(_repo "${CMAKE_SOURCE_DIR}/../..")
set(MORSE_FW_DIR "${_repo}/vendor/morse-firmware")
set(MBIN_CONVERTER
    "${_repo}/components/halow/components/mm-iot-sdk/framework/tools/buildsystem/convert-bin-to-mbin.py")
idf_build_get_property(python PYTHON)

if(NOT EXISTS "${MORSE_FW_DIR}/firmware")
    message(FATAL_ERROR "vendor/morse-firmware not checked out at ${MORSE_FW_DIR} "
                        "(run: git submodule update --init vendor/morse-firmware)")
endif()
if(NOT EXISTS "${MBIN_CONVERTER}")
    message(FATAL_ERROR "mbin converter not found: ${MBIN_CONVERTER}")
endif()

# Configured .mbin name -> source ELF name (same basename, .bin extension).
string(REGEX REPLACE "\\.mbin$" ".bin" FW_BIN_NAME  "${CONFIG_MM_FW_FILE}")
string(REGEX REPLACE "\\.mbin$" ".bin" BCF_BIN_NAME "${CONFIG_MM_BCF_FILE}")

# Firmware ELF lives flat in firmware/; BCF ELF under a vendor subdir of bcf/.
set(FW_ELF "${MORSE_FW_DIR}/firmware/${FW_BIN_NAME}")
file(GLOB_RECURSE BCF_ELF_MATCHES "${MORSE_FW_DIR}/bcf/${BCF_BIN_NAME}")
list(LENGTH BCF_ELF_MATCHES _n_bcf)
if(_n_bcf GREATER 0)
    list(GET BCF_ELF_MATCHES 0 BCF_ELF)
endif()

if(NOT EXISTS "${FW_ELF}")
    message(FATAL_ERROR "firmware ELF '${FW_BIN_NAME}' not found under ${MORSE_FW_DIR}/firmware/")
endif()
if(NOT BCF_ELF OR NOT EXISTS "${BCF_ELF}")
    message(FATAL_ERROR "BCF ELF '${BCF_BIN_NAME}' not found under ${MORSE_FW_DIR}/bcf/")
endif()
message(STATUS "morse firmware: ${FW_ELF} -> ${CONFIG_MM_FW_FILE}")
message(STATUS "morse firmware: ${BCF_ELF} -> ${CONFIG_MM_BCF_FILE}")

# --- build-time ELF -> Morse TLV .mbin (outputs under build/<app>) -----------
set(FW_MBIN  "${CMAKE_CURRENT_BINARY_DIR}/${CONFIG_MM_FW_FILE}")
set(BCF_MBIN "${CMAKE_CURRENT_BINARY_DIR}/${CONFIG_MM_BCF_FILE}")

add_custom_command(
    OUTPUT "${FW_MBIN}"
    COMMAND "${python}" "${MBIN_CONVERTER}" -o "${FW_MBIN}" "${FW_ELF}"
    DEPENDS "${FW_ELF}" "${MBIN_CONVERTER}"
    COMMENT "Converting firmware ELF -> ${CONFIG_MM_FW_FILE}"
    VERBATIM)
add_custom_command(
    OUTPUT "${BCF_MBIN}"
    COMMAND "${python}" "${MBIN_CONVERTER}" -o "${BCF_MBIN}" "${BCF_ELF}"
    DEPENDS "${BCF_ELF}" "${MBIN_CONVERTER}"
    COMMENT "Converting BCF ELF -> ${CONFIG_MM_BCF_FILE}"
    VERBATIM)

# --- wrap each .mbin into a linkable object with the symbols morselib expects --
# UNCHANGED FROM THE ORIGINAL component (morsemicro/firmware 2.10.4-esp32-2): the
# objcopy -I binary, --redefine-sym _binary_<id>_{start,size,end} ->
# {firmware,bcf}_binary_*, --rename-section to .rodata._fw_mbin, and the INTERFACE
# link. IDF's native binary-embed would emit _binary_<filename>_* names instead,
# which morselib does not reference. Only the .bin->.mbin step above is new.
function(make_binary_object __file __prefix __out_var)
    set(__object "${__file}.o")
    string(MAKE_C_IDENTIFIER "${__file}" __id)
    add_custom_command(
        OUTPUT "${__object}"
        COMMAND ${CMAKE_OBJCOPY} -I binary -O ${BFDNAME} -B ${ARCH} "${__file}" "${__object}"
            --redefine-sym _binary_${__id}_start=${__prefix}_start
            --redefine-sym _binary_${__id}_size=${__prefix}_size
            --redefine-sym _binary_${__id}_end=${__prefix}_end
            --rename-section
            .data=.rodata._fw_mbin,contents,alloc,load,readonly,data
        DEPENDS "${__file}"
        COMMENT "Wrapping ${__file} as ${__prefix}"
        VERBATIM)
    set(${__out_var} "${__object}" PARENT_SCOPE)
endfunction()

make_binary_object("${FW_MBIN}"  firmware_binary FW_OBJ)
make_binary_object("${BCF_MBIN}" bcf_binary      BCF_OBJ)

add_custom_target(morsefirmware ALL DEPENDS "${FW_OBJ}" "${BCF_OBJ}")
add_dependencies(${COMPONENT_TARGET} morsefirmware)
target_link_libraries(${COMPONENT_TARGET} INTERFACE "${FW_OBJ}" "${BCF_OBJ}")
