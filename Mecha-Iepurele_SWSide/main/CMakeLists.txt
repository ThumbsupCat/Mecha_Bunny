file(GLOB_RECURSE MODEL_SRCS
    "../edge-impulse-sdk/*.cpp"
    "../edge-impulse-sdk/*.c"
    "../edge-impulse-sdk/*.cc"
    "../edge-impulse-sdk/*.S"
    "../edge-impulse-sdk/*.s"
    "../tflite-model/*.cpp"
    "../tflite-model/*.cc"
)

idf_component_register(
    SRCS "main.cpp" ${MODEL_SRCS}
    INCLUDE_DIRS
        "."
        ".."
        "../edge-impulse-sdk"
        "../model-parameters"
        "../tflite-model"
        "../edge-impulse-sdk/porting/espressif/esp-dsp/modules/fft/include"
        "../edge-impulse-sdk/porting/espressif/esp-dsp/modules/common/include"
        "../edge-impulse-sdk/porting/espressif/esp-dsp/modules/fft/fixed"
        "../edge-impulse-sdk/porting/espressif/esp-dsp/modules/fft/float"
    PRIV_REQUIRES
        esp_http_server
        esp_http_client
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        spiffs
        driver
        mbedtls
        esp_timer
        lwip
        esp_system
        esp_driver_uart
        esp_driver_i2s
        esp_driver_gpio
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error
    -Wno-maybe-uninitialized
    -Wno-uninitialized
    -Wno-missing-field-initializers
    -Wno-deprecated-declarations
    -Wno-unused-function
    -Wno-unused-variable
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    EI_CLASSIFIER_ALLOCATION_STATIC=1
    CONFIG_DSP_MAX_FFT_SIZE=1024
)

target_link_libraries(${COMPONENT_LIB} PRIVATE m)
