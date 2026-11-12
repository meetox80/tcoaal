cmake_minimum_required(VERSION 3.16)
project(tcoaal_builder C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(ENGINES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../build" CACHE PATH "build folder holding the console engines to embed")
set(ART_DIR "${CMAKE_CURRENT_SOURCE_DIR}/art")

add_executable(tcoaal_embed tools/embed.c)

set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN})
set(ENGINE_BLOBS)
foreach(pair "psp/EBOOT.PBP:embedded_psp_eboot" "vita/eboot.bin:embedded_vita_eboot" "vita/param.sfo:embedded_vita_sfo"
             "3ds/tcoaal.3dsx:embedded_3ds_engine" "3ds/tcoaal.cia:embedded_3ds_cia" "wii/tcoaal.dol:embedded_wii_dol"
             "wiiu/tcoaal.rpx:embedded_wiiu_rpx" "ps2/tcoaal.elf:embedded_ps2_elf" "ps3/EBOOT.BIN:embedded_ps3_eboot"
             "xenon/xenon.elf:embedded_xenon_elf" "ps1/tcoaal.exe:embedded_ps1_exe")
  string(REPLACE ":" ";" parts ${pair})
  list(GET parts 0 engine_file)
  list(GET parts 1 engine_sym)
  if(EXISTS ${ENGINES_DIR}/${engine_file})
    add_custom_command(OUTPUT ${GEN}/${engine_sym}.c
      COMMAND tcoaal_embed ${ENGINES_DIR}/${engine_file} ${GEN}/${engine_sym}.c ${engine_sym}
      DEPENDS tcoaal_embed ${ENGINES_DIR}/${engine_file})
  else()
    file(WRITE ${GEN}/${engine_sym}.c "#include <stddef.h>\nconst unsigned char ${engine_sym}[1] = {0};\nconst size_t ${engine_sym}_size = 0;\n")
  endif()
  list(APPEND ENGINE_BLOBS ${GEN}/${engine_sym}.c)
endforeach()

set(ART_BLOBS)
foreach(pair "psp.png:embedded_psp_art" "vita.png:embedded_vita_art" "3ds.png:embedded_3ds_art" "wii.png:embedded_wii_art"
             "wiiu.png:embedded_wiiu_art" "ps2.png:embedded_ps2_art" "ds3.png:embedded_ps3_art" "x360.png:embedded_xenon_art")
  string(REPLACE ":" ";" parts ${pair})
  list(GET parts 0 art_file)
  list(GET parts 1 art_sym)
  add_custom_command(OUTPUT ${GEN}/${art_sym}.c
    COMMAND tcoaal_embed ${ART_DIR}/${art_file} ${GEN}/${art_sym}.c ${art_sym}
    DEPENDS tcoaal_embed ${ART_DIR}/${art_file})
  list(APPEND ART_BLOBS ${GEN}/${art_sym}.c)
endforeach()

file(GLOB BUILDER_SRC CONFIGURE_DEPENDS src/*.c)
add_executable(tcoaal-builder ${BUILDER_SRC} third_party/miniz.c ${ENGINE_BLOBS} ${ART_BLOBS})
target_include_directories(tcoaal-builder PRIVATE src third_party)
target_compile_definitions(tcoaal-builder PRIVATE _DEFAULT_SOURCE _FILE_OFFSET_BITS=64 MINIZ_NO_STDIO)
if(WIN32)
  target_link_libraries(tcoaal-builder PRIVATE ole32 shell32 user32 gdi32 advapi32 uuid)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(tcoaal-builder PRIVATE Threads::Threads m)
endif()

set(LIB_SRC ${BUILDER_SRC})
list(FILTER LIB_SRC EXCLUDE REGEX "/main\\.c$")
add_executable(tcoaal-builder-tests tests/test.c ${LIB_SRC} third_party/miniz.c ${ENGINE_BLOBS} ${ART_BLOBS})
target_include_directories(tcoaal-builder-tests PRIVATE src third_party)
target_compile_definitions(tcoaal-builder-tests PRIVATE _DEFAULT_SOURCE MINIZ_NO_STDIO)
if(WIN32)
  target_link_libraries(tcoaal-builder-tests PRIVATE ole32 shell32 user32 gdi32 advapi32 uuid)
else()
  target_link_libraries(tcoaal-builder-tests PRIVATE Threads::Threads m)
endif()
enable_testing()
add_test(NAME builder COMMAND tcoaal-builder-tests)
