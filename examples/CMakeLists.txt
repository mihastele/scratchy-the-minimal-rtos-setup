# Small demos, one per concept. Each is also a port smoke test: the same
# interleave must appear on every chip.
if(MRT_TARGET STREQUAL "host")
  add_executable(blinky_host blinky_host.c)
  target_link_libraries(blinky_host PRIVATE mini_rtos_kernel mini_rtos_port)
elseif(MRT_TARGET STREQUAL "avr")
  # One firmware per example: .elf for simavr, .hex for flashing.
  function(mrt_add_avr_example name)
    add_executable(${name} ${name}.c)
    # Explicit .elf (a toolchain-level EXECUTABLE_SUFFIX proved unreliable
    # across generators — the per-target property always sticks).
    set_target_properties(${name} PROPERTIES SUFFIX ".elf")
    target_link_libraries(${name} PRIVATE mini_rtos_kernel mini_rtos_port)
    add_custom_command(TARGET ${name} POST_BUILD
      COMMAND ${MRT_AVR_OBJCOPY} -O ihex -R .eeprom
        "$<TARGET_FILE:${name}>"
        "$<TARGET_FILE_DIR:${name}>/$<TARGET_FILE_BASE_NAME:${name}>.hex"
      COMMENT "Generating ${name}.hex")
  endfunction()
  mrt_add_avr_example(blinky_avr)
  mrt_add_avr_example(avr_selftest)
endif()
