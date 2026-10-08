cmake_minimum_required(VERSION 3.20)
project(prime C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The game image is supplied by the user (see README): Metroid Prime (USA, GM8E01,
# Rev 2) as an uncompressed .iso or a .ciso, placed in rom/.
file(GLOB _rom_images ${CMAKE_SOURCE_DIR}/rom/*.iso ${CMAKE_SOURCE_DIR}/rom/*.ciso)
list(LENGTH _rom_images _rom_count)
if(_rom_count GREATER 0)
  list(GET _rom_images 0 _default_iso)
else()
  set(_default_iso "")
endif()
set(GAME_ISO "${_default_iso}" CACHE FILEPATH "Your Metroid Prime (USA, Rev 2) disc image (.iso or .ciso)")

# Without an image we can still compile the runtime, which is enough to check a
# port. Only the recompiled game code and the final link need the disc.
if(GAME_ISO AND EXISTS "${GAME_ISO}")
  set(HAVE_GAME_ISO TRUE)
else()
  set(HAVE_GAME_ISO FALSE)
  if(GAME_ISO)
    message(WARNING "GAME_ISO is set to '${GAME_ISO}' but that file does not exist.")
  endif()
  message(WARNING
    "No game image found, so only the `runtime_check` target (compile the runtime, no link) "
    "is available. Put your own Metroid Prime (USA, Rev 2) .iso or .ciso in ${CMAKE_SOURCE_DIR}/rom/ "
    "or pass -DGAME_ISO=/path/to/game.iso to build the game. See README.md.")
endif()

set(GAME_DOL ${CMAKE_BINARY_DIR}/main.dol)
set(DTK_SYMBOLS ${CMAKE_SOURCE_DIR}/analysis/symbols.txt)
set(RECOMP_GEN_DIR ${CMAKE_BINARY_DIR}/gen)

# ---- Recompiled game code ----
if(HAVE_GAME_ISO)
  execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/recomp/extract_dol.py "${GAME_ISO}" ${GAME_DOL}
                  RESULT_VARIABLE EXTRACT_RESULT)
  if(NOT EXTRACT_RESULT EQUAL 0)
    message(FATAL_ERROR "could not extract main.dol from ${GAME_ISO}")
  endif()

  file(GLOB RECOMP_TOOL_SOURCES ${CMAKE_SOURCE_DIR}/recomp/*.py ${CMAKE_SOURCE_DIR}/recomp/*.txt)
  execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/recomp/recomp.py ${GAME_DOL} ${DTK_SYMBOLS} ${RECOMP_GEN_DIR}
                  RESULT_VARIABLE RECOMP_RESULT)
  if(NOT RECOMP_RESULT EQUAL 0)
    message(FATAL_ERROR "recompilation failed")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RECOMP_TOOL_SOURCES} ${GAME_ISO} ${DTK_SYMBOLS})
  include(${RECOMP_GEN_DIR}/sources.cmake)

  add_library(game_recomp STATIC ${RECOMP_SOURCES})
  target_include_directories(game_recomp PUBLIC ${CMAKE_SOURCE_DIR}/runtime ${RECOMP_GEN_DIR})
  # See ENTER()/RET() in runtime/recomp.h. ON maintains a guest call stack, which answers
  # "which guest function did that?" and gives a crash dump something to print. PUBLIC
  # because the CPU struct grows with it: the runtime and the generated code must agree
  # on the layout.
  option(MP_TRACE_CALLS "Maintain a guest call stack (diagnostics)" OFF)
  if(MP_TRACE_CALLS)
    target_compile_definitions(game_recomp PUBLIC MP_TRACE_CALLS)
  endif()
  if(MSVC)
    # Generated code is enormous and machine-written: keep the optimizer honest about
    # aliasing and floating point, and silence the noise it legitimately produces.
    target_compile_options(game_recomp PRIVATE /O2 /fp:precise /fp:except- /wd4101 /wd4102 /wd4723 /bigobj)
  else()
    target_compile_options(game_recomp PRIVATE -O2 -fno-strict-aliasing -Wno-unused-label -Wno-unused-variable
                           -Wno-parentheses-equality -ffp-contract=off)
  endif()
endif()

# ---- Runtime ----
# MP_BENCH_ONLY builds just the headless benchmark: no renderer, no SDL2, no GL. That
# is what a cross-build for a device uses, since it only needs a C++ toolchain and the
# recompiled code.
option(MP_BENCH_ONLY "Build only the headless benchmark (no SDL2, no renderer)" OFF)

file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/runtime/*.cpp)

# Alternative entry points and the SDL-free audio stub: none of these may land in a
# target that already has main.cpp or audio.cpp.
list(REMOVE_ITEM RUNTIME_SOURCES
  ${CMAKE_SOURCE_DIR}/runtime/bench_main.cpp
  ${CMAKE_SOURCE_DIR}/runtime/audio_stub.cpp)

# Everything below the renderer and the SDL frontend -- the CPU, OS, hardware and GX
# front end, including the batch queue in gx/xf.cpp -- is free of both GL and SDL.
set(CORE_SOURCES ${RUNTIME_SOURCES})
list(REMOVE_ITEM CORE_SOURCES
  ${CMAKE_SOURCE_DIR}/runtime/main.cpp
  ${CMAKE_SOURCE_DIR}/runtime/audio.cpp
  ${CMAKE_SOURCE_DIR}/runtime/audio_sdl.cpp
  ${CMAKE_SOURCE_DIR}/runtime/png.cpp
  ${CMAKE_SOURCE_DIR}/runtime/gx/render_gl.cpp
  ${CMAKE_SOURCE_DIR}/runtime/gx/gl_load.cpp
  ${CMAKE_SOURCE_DIR}/runtime/gx/shadergen.cpp)
list(APPEND CORE_SOURCES ${CMAKE_SOURCE_DIR}/runtime/audio_stub.cpp)

set(BENCH_SOURCES ${CORE_SOURCES} ${CMAKE_SOURCE_DIR}/runtime/bench_main.cpp)

# Render target: desktop OpenGL 3.3 core. GL entry points are resolved at runtime via a
# vendored glad loader; macOS links the system framework directly and needs neither.
set(GLAD_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/runtime/gx/glad/include)
set(GLAD_SOURCE ${CMAKE_SOURCE_DIR}/runtime/gx/glad/src/gl.c)
if(NOT APPLE)
  list(APPEND RUNTIME_SOURCES ${GLAD_SOURCE})
endif()

# SDL2: pkg-config where it exists; on Windows there are no prebuilt ARM64 binaries,
# so build it from source.
if(MP_BENCH_ONLY)
  set(MP_SDL_TARGET "")
elseif(WIN32)
  include(FetchContent)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL2
    URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz
    URL_HASH SHA256=24b574f71c87a763f50704bbb630cbe38298d544a1f890f099a4696b1d6beba4
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(SDL2)
  set(MP_SDL_TARGET SDL2::SDL2-static)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  set(MP_SDL_TARGET PkgConfig::SDL2)
endif()

if(MSVC)
  set(MP_RUNTIME_WARNINGS /W3 /wd4244 /wd4267)
else()
  set(MP_RUNTIME_WARNINGS -Wall -Wno-unused-function)
endif()

# Platform libraries the runtime needs beyond SDL2. The benchmark links none of this.
set(MP_PLATFORM_LIBS "")
if(MP_BENCH_ONLY)
elseif(APPLE)
  list(APPEND MP_PLATFORM_LIBS "-framework OpenGL")
elseif(WIN32)
  # opengl32 for the GL entry points, dbghelp to symbolize crash backtraces.
  list(APPEND MP_PLATFORM_LIBS opengl32 dbghelp)
else()
  find_package(OpenGL REQUIRED)
  list(APPEND MP_PLATFORM_LIBS OpenGL::GL)
endif()

# Headless benchmark: reports how fast the guest advances, with no graphics or audio
# in the way. Used to compare host CPUs -- see docs/dev/diagnostics.md.
if(HAVE_GAME_ISO)
  add_executable(prime_bench ${BENCH_SOURCES})
  target_include_directories(prime_bench PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${RECOMP_GEN_DIR})
  target_link_libraries(prime_bench PRIVATE game_recomp)
  target_compile_options(prime_bench PRIVATE ${MP_RUNTIME_WARNINGS})
  target_compile_definitions(prime_bench PRIVATE MP_DEFAULT_ISO="${GAME_ISO}")
  if(WIN32)
    target_link_libraries(prime_bench PRIVATE dbghelp)
  endif()
endif()

# Compile-only target: available even without a disc image, so the runtime and any
# port work can be checked on its own. With an image present `prime` already
# compiles these sources, so keep it out of the default build rather than doubling
# every build; `ninja runtime_check` still works.
if(NOT MP_BENCH_ONLY)
  if(HAVE_GAME_ISO)
    add_library(runtime_check OBJECT EXCLUDE_FROM_ALL ${RUNTIME_SOURCES})
  else()
    add_library(runtime_check OBJECT ${RUNTIME_SOURCES})
  endif()
  target_include_directories(runtime_check PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${RECOMP_GEN_DIR} ${GLAD_INCLUDE_DIR})
  target_link_libraries(runtime_check PRIVATE ${MP_SDL_TARGET})
  target_compile_options(runtime_check PRIVATE ${MP_RUNTIME_WARNINGS})
  target_compile_definitions(runtime_check PRIVATE MP_DEFAULT_ISO="${GAME_ISO}")
endif()

if(HAVE_GAME_ISO AND NOT MP_BENCH_ONLY)
  add_executable(prime ${RUNTIME_SOURCES})
  target_include_directories(prime PRIVATE ${GLAD_INCLUDE_DIR})
  target_link_libraries(prime PRIVATE game_recomp ${MP_SDL_TARGET} ${MP_PLATFORM_LIBS})
  target_compile_options(prime PRIVATE ${MP_RUNTIME_WARNINGS})
  target_compile_definitions(prime PRIVATE MP_DEFAULT_ISO="${GAME_ISO}")
endif()
