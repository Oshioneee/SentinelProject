cmake_minimum_required(VERSION 3.16)

set(EI_SDK_FOLDER "${CMAKE_CURRENT_SOURCE_DIR}/edge-impulse-sdk")

# ESP-IDF early expansion phase — declare REQUIRES here so include paths are wired up
if(CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_register(REQUIRES esp_timer freertos log heap)
    return()
endif()

include(${EI_SDK_FOLDER}/cmake/utils.cmake)

RECURSIVE_FIND_FILE_EXCLUDE_DIR(CPP_SOURCES "${EI_SDK_FOLDER}" "CMSIS" "*.cpp")
RECURSIVE_FIND_FILE_EXCLUDE_DIR(CC_SOURCES  "${EI_SDK_FOLDER}" "CMSIS" "*.cc")
RECURSIVE_FIND_FILE_EXCLUDE_DIR(C_SOURCES   "${EI_SDK_FOLDER}" "CMSIS" "*.c")

RECURSIVE_FIND_FILE_APPEND(ASM_SOURCES "${EI_SDK_FOLDER}/porting/espressif/ESP-NN"              "*.s")
RECURSIVE_FIND_FILE_APPEND(ASM_SOURCES "${EI_SDK_FOLDER}/porting/espressif/ESP-NN"              "*.S")
RECURSIVE_FIND_FILE_APPEND(ASM_SOURCES "${EI_SDK_FOLDER}/porting/espressif/esp-dsp/modules/fft" "*.s")
RECURSIVE_FIND_FILE_APPEND(ASM_SOURCES "${EI_SDK_FOLDER}/porting/espressif/esp-dsp/modules/fft" "*.S")

list(APPEND C_SOURCES "${EI_SDK_FOLDER}/CMSIS/DSP/Source/StatisticsFunctions/arm_rms_f32.c")

RECURSIVE_FIND_FILE_APPEND(MODEL_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tflite-model" "*.cpp")

idf_component_register(
    SRCS
        ${CPP_SOURCES}
        ${CC_SOURCES}
        ${C_SOURCES}
        ${ASM_SOURCES}
        ${MODEL_SOURCES}
    INCLUDE_DIRS
        "."
        "tflite-model"
        "${EI_SDK_FOLDER}"
        "${EI_SDK_FOLDER}/porting"
        "${EI_SDK_FOLDER}/porting/espressif/esp-dsp/modules/common/include"
        "${EI_SDK_FOLDER}/porting/espressif/esp-dsp/modules/fft/include"
        "${EI_SDK_FOLDER}/porting/espressif/ESP-NN"
        "${EI_SDK_FOLDER}/porting/espressif/ESP-NN/include"
    REQUIRES
        esp_timer
        freertos
        log
        heap
)

target_compile_definitions(${COMPONENT_TARGET} PUBLIC asm=__asm__)

# Suppress warnings caused by Edge Impulse SDK's older TFLite sources
# being incompatible with the newer TfLiteRegistration struct in IDF v6.0
target_compile_options(${COMPONENT_TARGET} PRIVATE
    -Wno-error=missing-field-initializers
    -Wno-missing-field-initializers
    -Wno-error=deprecated-declarations
    -Wno-error=maybe-uninitialized
    -Wno-maybe-uninitialized
    -Wno-error=unused-variable
    -Wno-error=unused-function
    -Wno-error=sign-compare
    -Wno-error=type-limits
    -Wno-error=narrowing
)