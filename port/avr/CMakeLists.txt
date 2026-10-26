# AVR port (ATmega328P + ATmega2560): C setup plus the context-switch
# assembly. MCU and F_CPU arrive from the toolchain file / board presets.
add_library(mini_rtos_port STATIC port.c port_asm.S)
target_include_directories(mini_rtos_port PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_include_directories(mini_rtos_port PRIVATE ${PROJECT_SOURCE_DIR}/kernel)
