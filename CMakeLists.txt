cmake_minimum_required(VERSION 3.20)

project(ESPboy_Emulator LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# The machine: CPU, memory, peripherals, the board. No SDL in here.
add_library(espboy_core STATIC
    src/xt_decode.c
    src/xt_cpu.c
    src/esp8266.c
    src/periph.c
    src/i2c.c
    src/onewire.c
    src/st7735.c
    src/audio.c
    src/speaker_filter.c
    src/gif.c
    src/loader.c
    src/sdk_params.c
)
target_include_directories(espboy_core PUBLIC src)
if(NOT MSVC)
    target_compile_options(espboy_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        target_compile_options(espboy_core PRIVATE -O3)
    endif()
endif()
if(NOT WIN32 AND NOT EMSCRIPTEN)
    target_link_libraries(espboy_core PUBLIC m)
endif()

# ---------------------------------------------------------------------------
# The emulator with its SDL3 front end
#
# Natively SDL3 is found as an installed package (static when there is one,
# so the Windows exe runs on its own). With USE_VENDORED_SDL it is instead
# downloaded at a fixed release and built here as a static library, which is
# what the release builds (.github/workflows/build.yml) do. Under Emscripten
# SDL3 comes from emscripten's own port.
# ---------------------------------------------------------------------------
option(USE_VENDORED_SDL "Download SDL3 and link it statically" OFF)
set(SDL3_VENDORED_TAG "release-3.4.12" CACHE STRING "SDL3 git tag USE_VENDORED_SDL builds")

if(USE_VENDORED_SDL AND NOT EMSCRIPTEN)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG ${SDL3_VENDORED_TAG}
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(SDL3)
endif()

add_executable(ESPboy_Emulator src/main.c)
target_link_libraries(ESPboy_Emulator PRIVATE espboy_core)

if(EMSCRIPTEN)
    set_target_properties(ESPboy_Emulator PROPERTIES SUFFIX ".html")
    target_compile_options(ESPboy_Emulator PRIVATE -sUSE_SDL=3)
    target_link_options(ESPboy_Emulator PRIVATE
        -sUSE_SDL=3
        -sALLOW_MEMORY_GROWTH=1
        -sINITIAL_MEMORY=134217728
        -sSTACK_SIZE=1048576
        -sFORCE_FILESYSTEM=1
        -lidbfs.js
        -sEXPORTED_RUNTIME_METHODS=FS,ccall,UTF8ToString,HEAPU8
        --shell-file ${PROJECT_SOURCE_DIR}/web/shell.html
    )
    target_compile_options(espboy_core PRIVATE -O3)
else()
    if(USE_VENDORED_SDL)
        target_link_libraries(ESPboy_Emulator PRIVATE SDL3::SDL3-static)
        if(MINGW)
            target_link_options(ESPboy_Emulator PRIVATE -static-libgcc -static)
        endif()
    else()
        find_package(SDL3 REQUIRED CONFIG)
        if(TARGET SDL3::SDL3-static AND MINGW)
            target_link_libraries(ESPboy_Emulator PRIVATE SDL3::SDL3-static)
            target_link_options(ESPboy_Emulator PRIVATE -static-libgcc -static)
        else()
            target_link_libraries(ESPboy_Emulator PRIVATE SDL3::SDL3)
        endif()
    endif()
    if(NOT MSVC)
        target_compile_options(ESPboy_Emulator PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
    if(WIN32)
        set_target_properties(ESPboy_Emulator PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
endif()

if(NOT EMSCRIPTEN)
    add_executable(espboy_headless tools/headless.c)
    target_link_libraries(espboy_headless PRIVATE espboy_core)
    if(WIN32)
        target_link_libraries(espboy_headless PRIVATE shell32)
    endif()

    add_executable(xtdis tools/xtdis.c)
    target_link_libraries(xtdis PRIVATE espboy_core)
endif()
