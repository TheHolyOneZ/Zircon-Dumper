cmake_minimum_required(VERSION 3.25)
project(Zircon VERSION 0.2.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Link the CRT statically. A released build is then three files that run on a clean
# Windows install, with no "VCRUNTIME140.dll was not found" for anyone who has never
# installed Visual Studio.
#
# It matters more for the payload than for the shells: an injected DLL that imports
# MSVCP140.dll lands in a process that may already have a different version of it loaded,
# and the game owns that decision. Carrying our own removes the question.
# One version, taken from project() above and handed to everything that prints one: the
# CLI, the dump header, and the VERSIONINFO block in the .rc files.
#
# It was written out by hand three times before. Two of them were fixed and the third, the
# .rc, was missed, which is how the dump header announced 0.1.0 for the whole of 0.2.0.
# Nothing compares them, so nothing catches it.
#
# A release build drops the -dev suffix: cmake -B build -DZIRCON_RELEASE=ON
option(ZIRCON_RELEASE "Build as a release: drop the -dev suffix from the version" OFF)
if(ZIRCON_RELEASE)
  set(ZIRCON_VERSION_STRING "${PROJECT_VERSION}")
else()
  set(ZIRCON_VERSION_STRING "${PROJECT_VERSION}-dev")
endif()

add_compile_definitions(
  ZIRCON_VERSION="${ZIRCON_VERSION_STRING}"
  ZIRCON_VER_MAJOR=${PROJECT_VERSION_MAJOR}
  ZIRCON_VER_MINOR=${PROJECT_VERSION_MINOR}
  ZIRCON_VER_PATCH=${PROJECT_VERSION_PATCH}
)
message(STATUS "Zircon version: ${ZIRCON_VERSION_STRING}")

option(ZIRCON_STATIC_CRT "Link the MSVC runtime statically" ON)
if(MSVC AND ZIRCON_STATIC_CRT)
  cmake_policy(SET CMP0091 NEW)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(ZIRCON_BUILD_CLI   "Build the zircon command-line shell"   ON)
option(ZIRCON_BUILD_DLL   "Build the injectable zircon payload"   ON)
option(ZIRCON_BUILD_GUI   "Build the zircon GUI shell"            ON)
option(ZIRCON_BUILD_TESTS "Build the test suite"                  ON)
option(ZIRCON_BUILD_PLUGINS "Build the plugins that ship with the tool" ON)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /Zc:preprocessor /EHsc)
else()
  add_compile_options(-Wall -Wextra)
endif()

# We call the W forms of every Win32 API. Without these, resource macros such as
# IDC_ARROW expand to their ANSI form and will not bind to LoadCursorW.
if(WIN32)
  add_compile_definitions(UNICODE _UNICODE)
endif()

set(ZIRCON_OUT ${CMAKE_BINARY_DIR}/bin)

# Plugins land in their own directory rather than beside the executable: loading
# third-party code is opt-in, and a folder the user points at deliberately is easier
# to reason about than a pile of DLLs next to zircon.exe.
# The generator expression is deliberate: with a plain path a multi-config generator
# appends the config itself and plugins land in bin/plugins/Release rather than beside the
# executable that loads them.
set(ZIRCON_PLUGIN_OUT ${ZIRCON_OUT}/$<CONFIG>/plugins)

# ---------------------------------------------------------------------------------
# Libraries. The dependency rule is one-directional: core knows nothing about Unreal,
# engine knows nothing about output formats. The three shells below are thin — all the
# real work lives here, which is what makes shipping a CLI, a DLL and a GUI cheap.
# ---------------------------------------------------------------------------------

add_library(zircon_core STATIC
  src/core/src/MemorySource.cpp
  src/core/src/Log.cpp
  src/core/src/Term.cpp
  src/core/src/PeImage.cpp
  src/core/src/PatternScanner.cpp
  src/core/src/ProcessList.cpp
  src/core/src/Injector.cpp
  src/core/src/CachedMemorySource.cpp
  src/core/src/InternalMemorySource.cpp
  src/core/src/ExternalMemorySource.cpp
  src/core/src/DumpMemorySource.cpp
  src/core/src/StaticMemorySource.cpp
)
target_include_directories(zircon_core PUBLIC src/core/include)
if(WIN32)
  target_link_libraries(zircon_core PUBLIC psapi)
endif()

add_library(zircon_engine STATIC
  src/engine/src/UnrealDetect.cpp
  src/engine/src/EngineProfile.cpp
  src/engine/src/ObjectArray.cpp
  src/engine/src/NamePool.cpp
  src/engine/src/ObjectLayout.cpp
  src/engine/src/StructLayout.cpp
  src/engine/src/ClassLayout.cpp
  src/engine/src/Hooks.cpp
  src/engine/src/PropertyLayout.cpp
  src/engine/src/TypeResolver.cpp
  src/engine/src/FunctionLayout.cpp
  src/engine/src/EnumLayout.cpp
  src/engine/src/Kismet.cpp
  src/engine/src/ValueReader.cpp
  src/engine/src/ValueWriter.cpp
  src/engine/src/ProcessEvent.cpp
  src/engine/src/DumpBuilder.cpp
)
target_include_directories(zircon_engine PUBLIC src/engine/include)
target_link_libraries(zircon_engine PUBLIC zircon_core zircon_ir)

# The IR is pure data: no memory access, no Unreal knowledge, no OS. It deliberately
# links nothing, so the CMake graph itself enforces that emitters and diffing can be
# built and tested without a target process anywhere in the picture.
add_library(zircon_ir STATIC
  src/ir/src/Model.cpp
  src/ir/src/Json.cpp
)
target_include_directories(zircon_ir PUBLIC src/ir/include)

# All static libs get linked into a DLL, so they must be position-independent.
set_target_properties(zircon_core zircon_engine zircon_ir
                      PROPERTIES POSITION_INDEPENDENT_CODE ON)

# emit/ knows nothing about memory or the engine: an emitter is a pure function of the
# IR. The CMake graph enforces that by linking only zircon_ir.
add_library(zircon_emit STATIC
  src/emit/src/Util.cpp
  src/emit/src/Registry.cpp
  src/emit/src/CppSdk.cpp
  src/emit/src/Usmap.cpp
  src/emit/src/Ida.cpp
  src/emit/src/Ghidra.cpp
  src/emit/src/ReClass.cpp
  src/emit/src/Docs.cpp
  src/emit/src/Graphs.cpp
  src/emit/src/Json.cpp
)
target_include_directories(zircon_emit PUBLIC src/emit/include PRIVATE src/emit/src)
target_link_libraries(zircon_emit PUBLIC zircon_ir)
set_target_properties(zircon_emit PROPERTIES POSITION_INDEPENDENT_CODE ON)

# diff/ compares two IRs. Like emit/, it is pure data in and text out, so it links only
# zircon_ir and can be tested against fixtures with no game running.
add_library(zircon_diff STATIC
  src/diff/src/Diff.cpp
  src/diff/src/Report.cpp
)
target_include_directories(zircon_diff PUBLIC src/diff/include)
target_link_libraries(zircon_diff PUBLIC zircon_ir)
set_target_properties(zircon_diff PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------------
# Shells
# ---------------------------------------------------------------------------------

if(ZIRCON_BUILD_CLI)
  # The .rc carries the icon and the version block. MSVC compiles it as part of the
  # target; on anything else it is simply not added, since neither matters there.
  set(ZIRCON_RC_CLI "")
  set(ZIRCON_RC_GUI "")
  set(ZIRCON_RC_DLL "")
  if(WIN32)
    set(ZIRCON_RC_CLI res/zircon_cli.rc)
    set(ZIRCON_RC_GUI res/zircon_gui.rc)
    set(ZIRCON_RC_DLL res/zircon_payload.rc)
  endif()

  add_executable(zircon src/app/main.cpp ${ZIRCON_RC_CLI})
  target_link_libraries(zircon PRIVATE zircon_engine zircon_emit zircon_diff zircon_plugin)
  set_target_properties(zircon PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${ZIRCON_OUT})
endif()

if(ZIRCON_BUILD_GUI)
  add_library(zircon_imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
  )
  target_include_directories(zircon_imgui PUBLIC third_party/imgui)
  # Vendored third-party code is not held to this project's warning policy; its warnings
  # are not ours to fix and would drown our own.
  if(MSVC)
    target_compile_options(zircon_imgui PRIVATE /W0)
  endif()

  # The browser itself, shared by the standalone window and the injected payload.
  add_library(zircon_gui_lib STATIC src/gui/Host.cpp src/gui/Browser.cpp)
  target_include_directories(zircon_gui_lib PUBLIC src/gui res)
  # zircon_emit so the browser can write a dump itself, not only browse one.
  target_link_libraries(zircon_gui_lib PUBLIC zircon_engine zircon_emit zircon_imgui)
  set_target_properties(zircon_gui_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)

  add_executable(zircon_gui WIN32 src/gui/main_gui.cpp ${ZIRCON_RC_GUI})
  target_link_libraries(zircon_gui PRIVATE zircon_gui_lib)
  set_target_properties(zircon_gui PROPERTIES
    OUTPUT_NAME              zircon-gui
    RUNTIME_OUTPUT_DIRECTORY ${ZIRCON_OUT})
endif()

# plugin/ sits above emit/: it turns the IR into something a third party can read through
# a C ABI, and registers their emitters into emit's registry. It knows nothing about
# memory or the engine, same as emit.
add_library(zircon_plugin STATIC
  src/plugin/src/Nodes.cpp
  src/plugin/src/Api.cpp
  src/plugin/src/Loader.cpp
)
target_include_directories(zircon_plugin PUBLIC src/plugin/include include)
target_link_libraries(zircon_plugin PUBLIC zircon_ir zircon_emit zircon_engine zircon_core)
set_target_properties(zircon_plugin PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------------
# Plugins that ship with the tool. These are ordinary plugins built from the public
# header: nothing here is linked into zircon.exe, and nothing here can reach past ZnApi.
# ---------------------------------------------------------------------------------

if(ZIRCON_BUILD_PLUGINS)
  # Lua 5.4, vendored. Built as C, and held to its own warning level rather than ours.
  add_library(lua_static STATIC
  third_party/lua/lapi.c
  third_party/lua/lauxlib.c
  third_party/lua/lbaselib.c
  third_party/lua/lcode.c
  third_party/lua/lcorolib.c
  third_party/lua/lctype.c
  third_party/lua/ldblib.c
  third_party/lua/ldebug.c
  third_party/lua/ldo.c
  third_party/lua/ldump.c
  third_party/lua/lfunc.c
  third_party/lua/lgc.c
  third_party/lua/linit.c
  third_party/lua/liolib.c
  third_party/lua/llex.c
  third_party/lua/lmathlib.c
  third_party/lua/lmem.c
  third_party/lua/loadlib.c
  third_party/lua/lobject.c
  third_party/lua/lopcodes.c
  third_party/lua/loslib.c
  third_party/lua/lparser.c
  third_party/lua/lstate.c
  third_party/lua/lstring.c
  third_party/lua/lstrlib.c
  third_party/lua/ltable.c
  third_party/lua/ltablib.c
  third_party/lua/ltm.c
  third_party/lua/lundump.c
  third_party/lua/lutf8lib.c
  third_party/lua/lvm.c
  third_party/lua/lzio.c
  )
  target_include_directories(lua_static PUBLIC third_party/lua)
  set_target_properties(lua_static PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
  if(MSVC)
    target_compile_options(lua_static PRIVATE /W0)
    # Lua uses fopen and getenv throughout; they are not deprecated, only unfashionable.
    target_compile_definitions(lua_static PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()

  add_library(zircon_lua MODULE src/plugins/lua/LuaPlugin.c)
  target_link_libraries(zircon_lua PRIVATE lua_static)
  target_include_directories(zircon_lua PRIVATE include)
  if(MSVC)
    target_compile_definitions(zircon_lua PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
  set_target_properties(zircon_lua PROPERTIES
    PREFIX                   ""
    SUFFIX                   ".dll"
    OUTPUT_NAME              zircon_lua
    LIBRARY_OUTPUT_DIRECTORY ${ZIRCON_PLUGIN_OUT}
    RUNTIME_OUTPUT_DIRECTORY ${ZIRCON_PLUGIN_OUT})

  # The example scripts belong beside the DLL that loads them.
  add_custom_command(TARGET zircon_lua POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/plugins ${ZIRCON_PLUGIN_OUT})
endif()

# The payload is defined after the GUI so it can link the browser: injected, the tool
# both dumps and opens the live UI, and neither half is worth duplicating.
if(ZIRCON_BUILD_DLL)
  add_library(zircon_payload SHARED src/dll/dllmain.cpp ${ZIRCON_RC_DLL})
  target_link_libraries(zircon_payload PRIVATE zircon_engine zircon_emit zircon_ir)
  if(ZIRCON_BUILD_GUI)
    target_link_libraries(zircon_payload PRIVATE zircon_gui_lib)
    target_compile_definitions(zircon_payload PRIVATE ZIRCON_WITH_GUI=1)
  endif()
  set_target_properties(zircon_payload PROPERTIES
    OUTPUT_NAME              zircon
    RUNTIME_OUTPUT_DIRECTORY ${ZIRCON_OUT}
    LIBRARY_OUTPUT_DIRECTORY ${ZIRCON_OUT}
    ARCHIVE_OUTPUT_DIRECTORY ${ZIRCON_OUT})
endif()

# ---------------------------------------------------------------------------------
# Tests. These must never need a game installed.
# ---------------------------------------------------------------------------------

if(ZIRCON_BUILD_TESTS)
  enable_testing()
  add_executable(zircon_tests tests/test_core.cpp)
  target_link_libraries(zircon_tests PRIVATE zircon_engine)
  set_target_properties(zircon_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${ZIRCON_OUT})
  add_test(NAME core COMMAND zircon_tests)

  add_executable(zircon_plugin_tests tests/test_plugin.cpp)
  target_link_libraries(zircon_plugin_tests PRIVATE zircon_plugin)
  set_target_properties(zircon_plugin_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${ZIRCON_OUT})
  add_test(NAME plugin COMMAND zircon_plugin_tests)

  add_executable(zircon_ir_tests tests/test_ir.cpp)
  target_link_libraries(zircon_ir_tests PRIVATE zircon_ir)
  set_target_properties(zircon_ir_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${ZIRCON_OUT})
  add_test(NAME ir COMMAND zircon_ir_tests)

  add_executable(zircon_emit_tests tests/test_emit_usmap.cpp)
  target_link_libraries(zircon_emit_tests PRIVATE zircon_emit)
  set_target_properties(zircon_emit_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${ZIRCON_OUT})
  add_test(NAME emit COMMAND zircon_emit_tests)

  add_executable(zircon_diff_tests tests/test_diff.cpp)
  target_link_libraries(zircon_diff_tests PRIVATE zircon_diff)
  set_target_properties(zircon_diff_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${ZIRCON_OUT})
  add_test(NAME diff COMMAND zircon_diff_tests)

  add_executable(zircon_kismet_tests tests/test_kismet.cpp)
  target_link_libraries(zircon_kismet_tests PRIVATE zircon_engine)
  set_target_properties(zircon_kismet_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${ZIRCON_OUT})
  add_test(NAME kismet COMMAND zircon_kismet_tests)

  add_executable(zircon_write_tests tests/test_write.cpp)
  target_link_libraries(zircon_write_tests PRIVATE zircon_engine)
  set_target_properties(zircon_write_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${ZIRCON_OUT})
  add_test(NAME write COMMAND zircon_write_tests)
endif()
