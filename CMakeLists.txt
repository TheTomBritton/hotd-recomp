# =============================================================================
# The House of the Dead - static recompilation hosts
#
#   py -3 run_lift.py --all  # -> src/recomp/gen/  (needs work/functions.json)
#
#   Windows (MSVC x86, pcrecomp's runtime/native32):
#     build.cmd              # vcvarsall x86 + CMake + Ninja -> build/hotd.exe
#   macOS and Linux (64-bit, pcrecomp's runtime/win32hle, SDL2):
#     ./build-macos.sh       # -> build-macos/hotd
#   The browser (Emscripten, the same win32hle):
#     ./build-web.sh         # -> build-web/hotd1.js + hotd1.wasm
# =============================================================================
cmake_minimum_required(VERSION 3.20)
project(hotd C)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(PCRECOMP "${CMAKE_SOURCE_DIR}/../pcrecomp" CACHE PATH "pcrecomp checkout")
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/recomp/gen/*.c")
list(FILTER GEN_SOURCES EXCLUDE REGEX "/[.]_[^/]*$")
if(NOT GEN_SOURCES)
  message(FATAL_ERROR "src/recomp/gen is empty -- run `py -3 run_lift.py --all` first")
endif()
set(GEN_OPT "-O2" CACHE STRING "optimisation for the generated C (MSVC: /O2)")

# ---- the browser: the lifted C on runtime/win32hle, compiled to WebAssembly
# with Emscripten (build-web.sh). One linear memory, so the guest's 32-bit
# addresses are the program's own (RECOMP_FLAT_MEMORY): the image goes at
# 0x00400000..0x013D1600 and the toolchain's static data, stack and heap
# start above it (GLOBAL_BASE, 24 MB). The game runs on a pthread
# (PROXY_TO_PTHREAD) so it may block as a Win32 program does; its files come
# from WASMFS: the game folder served beside the page, and saves in the
# browser's storage (src/web/web.c).
if(EMSCRIPTEN)
  set(HLE ${PCRECOMP}/runtime/win32hle)
  file(GLOB HLE_SOURCES CONFIGURE_DEPENDS "${HLE}/*.c")
  list(FILTER HLE_SOURCES EXCLUDE REGEX "_selftest\\.c$")
  add_executable(hotd src/hle/main.c src/hle/hotd.c src/hle/eh_setjmp.c src/web/web.c ${HLE_SOURCES} ${GEN_SOURCES})
  set_target_properties(hotd PROPERTIES OUTPUT_NAME "hotd1" SUFFIX ".js")
  target_include_directories(hotd PRIVATE src/hle src/web src/recomp/gen ${HLE} ${PCRECOMP}/runtime/recomp32)
  set(WEB_GLOBAL_BASE 0x1800000)
  target_compile_definitions(hotd PRIVATE RECOMP_RETADDR=0u RECOMP_LOCAL_REGS RECOMP_FLAT_MEMORY HLE_FIXED_END=${WEB_GLOBAL_BASE}u)
  set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;${GEN_OPT};-fno-strict-aliasing;-fwrapv")
  target_compile_options(hotd PRIVATE -fno-strict-aliasing -fwrapv -pthread --use-port=sdl2 --use-port=sdl2_ttf)
  set(WEB_LINK_OPT "-O1" CACHE STRING "optimisation at link (wasm-opt)")
  set(WEB_ASSERTIONS 0 CACHE STRING "Emscripten runtime assertions (1: readable aborts, slower)")
  target_link_options(hotd PRIVATE ${WEB_LINK_OPT} -g0 --profiling-funcs -pthread --use-port=sdl2 --use-port=sdl2_ttf
    -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=8
    -sWASMFS -lfetchfs.js -lopfs.js --js-library ${CMAKE_SOURCE_DIR}/src/web/wasmfs_fetch.js
    -sOFFSCREEN_FRAMEBUFFER
    -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
    -sGLOBAL_BASE=24MB
    -sINITIAL_MEMORY=512MB -sMAXIMUM_MEMORY=2048MB -sALLOW_MEMORY_GROWTH
    -sSTACK_SIZE=16MB -sDEFAULT_PTHREAD_STACK_SIZE=4MB
    -sEXIT_RUNTIME -sENVIRONMENT=web,worker
    -sASSERTIONS=${WEB_ASSERTIONS} -sALLOW_UNIMPLEMENTED_SYSCALLS
    -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,stringToNewUTF8,UTF8ToString
    -sMODULARIZE=0 -sEXPORT_NAME=HOTD1)
  return()
endif()

# ---- native macOS and Linux: the lifted C on runtime/win32hle, a 64-bit
# program. The guest's 4 GB is one block of the host's memory (win32hle's
# guest_mem.c); on a Mac at a fixed address, so the lifted code adds a
# constant (RECOMP_MEM_BASE).
if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  pkg_check_modules(SDL2_TTF REQUIRED IMPORTED_TARGET SDL2_ttf)
  set(HLE ${PCRECOMP}/runtime/win32hle)
  file(GLOB HLE_SOURCES CONFIGURE_DEPENDS "${HLE}/*.c")
  list(FILTER HLE_SOURCES EXCLUDE REGEX "_selftest\\.c$|/[.]_[^/]*$")
  add_executable(hotd src/hle/main.c src/hle/hotd.c src/hle/eh_setjmp.c ${HLE_SOURCES} ${GEN_SOURCES})
  target_include_directories(hotd PRIVATE src/hle src/recomp/gen ${HLE} ${PCRECOMP}/runtime/recomp32)
  target_compile_definitions(hotd PRIVATE RECOMP_RETADDR=0u RECOMP_LOCAL_REGS)
  if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_compile_definitions(hotd PRIVATE RECOMP_FLAT_MEMORY)
  elseif(APPLE)
    set(HOTD_MEM_BASE "0x100000000000ull" CACHE STRING "host address of the guest's 4 GB (empty: anywhere)")
    if(HOTD_MEM_BASE)
      target_compile_definitions(hotd PRIVATE RECOMP_MEM_BASE=${HOTD_MEM_BASE})
    endif()
  endif()
  set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;${GEN_OPT};-fno-strict-aliasing;-fwrapv")
  target_compile_options(hotd PRIVATE -fno-strict-aliasing -fwrapv -Wall -Wno-unused-function -Wno-unused-parameter)
  target_link_libraries(hotd PRIVATE PkgConfig::SDL2 PkgConfig::SDL2_TTF pthread m)
  option(HOTD_TRACE "record every lifted function entry (main.c's tracer: --calltrace-from S)" OFF)
  if(HOTD_TRACE)
    target_compile_definitions(hotd PRIVATE RECOMP_TRACE=1)
  endif()
  return()
endif()

# ---- Windows: the lifted C on runtime/native32 (MSVC x86, 32-bit) --------
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "the Windows host must be 32-bit: use build.cmd (vcvarsall x86)")
endif()
if(NOT MSVC)
  message(FATAL_ERROR "runtime/native32 is MSVC x86 inline assembly")
endif()
# The C runtime linked in: a test VM has no 32-bit VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(hotd
  src/runtime/host.c
  src/runtime/ddraw_window.c
  src/runtime/menu.c
  src/runtime/net.c
  src/runtime/hotd.manifest
  ${PCRECOMP}/runtime/native32/native32.c
  ${PCRECOMP}/runtime/recomp32/image_loader.c
  ${PCRECOMP}/runtime/recomp32/recomp_trace.c
  ${GEN_SOURCES}
)
target_include_directories(hotd PRIVATE
  src/runtime src/recomp/gen ${PCRECOMP}/runtime/native32 ${PCRECOMP}/runtime/recomp32)
target_compile_definitions(hotd PRIVATE RECOMP_RETADDR=0u _CRT_SECURE_NO_WARNINGS
                                            RECOMP_FLAT_MEMORY RECOMP_LOCAL_REGS)
# Linked high and not relocatable, so the guest's 0x00400000..0x013D1600 is free.
target_link_options(hotd PRIVATE /BASE:0x60000000 /FIXED:NO /DYNAMICBASE:NO
                                       /NXCOMPAT /LARGEADDRESSAWARE /STACK:0x1000000
                                       /MANIFESTUAC:NO)
set(GEN_OPT_MSVC "/O2" CACHE STRING "optimisation for the generated C")
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/w;${GEN_OPT_MSVC}")
set_source_files_properties(src/runtime/host.c src/runtime/ddraw_window.c src/runtime/menu.c src/runtime/net.c PROPERTIES COMPILE_OPTIONS "/W3")
target_link_libraries(hotd PRIVATE user32 gdi32 kernel32 ole32 uuid dxguid winmm ws2_32)

option(HOTD_TRACE "record a ring buffer of lifted function entries" OFF)
if(HOTD_TRACE)
  target_compile_definitions(hotd PRIVATE RECOMP_TRACE=1)
endif()
