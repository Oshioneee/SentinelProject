# We need to collect all C/C++ files
file(GLOB_RECURSE SOURCES "*.cpp" "*.c" "*.cc")

# Filter out unneeded ports
list(FILTER SOURCES EXCLUDE REGEX "edge-impulse-sdk/porting/.*")
# Re-add espressif port
list(APPEND SOURCES "edge-impulse-sdk/porting/espressif/ei_classifier_porting.cpp")

# Exclude some unneeded hardware specific files
list(FILTER SOURCES EXCLUDE REGEX "edge-impulse-sdk/classifier/ei_run_camera.cpp")

idf_component_register(
    SRCS "${SOURCES}"
    INCLUDE_DIRS "." "edge-impulse-sdk" "model-parameters" "tflite-model"
    REQUIRES esp_timer
)

# Set compile options needed by Edge Impulse
target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -w)
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    -DEI_PORTING_ESPRESSIF=1
    -DEI_CLASSIFIER_TFLITE_ENABLE_ESP_NN=0
)

