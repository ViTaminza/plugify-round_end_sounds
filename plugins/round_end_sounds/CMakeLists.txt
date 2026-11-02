# round_end_sounds — Plugify C++ port of the Metamod:Source plugin cs2-round-end-sounds
# (a track at the end of the round, per-player on/off + volume, MVP-anthem muting).
#
# Pure s2sdk (docs/PLUGIN_STANDARD.md §7): no pisexcompat, no guard library, no plgdb. hudmenu is
# an optional functional dependency only for the player's per-player menu-type choice
# (hudmenu::GetClientMenuType), not a wrapper library.
# (this plugin uses no SQL) and no other shared wrapper. The map gate (PITFALLS B1/B2), the
# client/controller/handle guards (C2/C1), the map-gated schema offsets (C8), the update-driven
# cookie reconcile/fade (B4/C9, deliberately no s2sdk timer), the event hooks/commands/menus and
# their symmetric teardown (C11/C12/A7) all live in src/.
#
#   round_end_sounds_core            engine-independent core (config/phrase parsing, track picking,
#                                    preview/fade arithmetic, music-kit rules; + its tests)
#   plugify-round_end_sounds         the plugin shared library (entry "bin/round_end_sounds")
#   round_end_sounds_tests          the engine-free core tests (kept as the HEAD target name, so an
#                                    existing build tree overwrites its binary instead of leaving the
#                                    old test to be picked up by test.sh)
#   round_end_sounds_runtime_tests   the plugin TU with recording __s2sdk_* fakes: config/phrase
#                                    resolution + language chain, audio (user messages, precache,
#                                    EmitSound), map start/end gate, slots/handles and reload

set(ROUND_END_SOUNDS_VERSION "1.1.0")

# --- s2sdk / permissions bindings ---------------------------------------------
# The __s2sdk_* / __permissions_* binding globals have to be compiled into every plugin; the root
# CMakeLists builds them as the object libraries s2sdk_bindings / permissions_bindings. These are
# the standalone fallbacks for a configure that only includes this directory - named differently
# on purpose, so they can never collide with the root targets when the whole tree is configured.
if(TARGET s2sdk_bindings)
    set(ROUND_END_SOUNDS_S2SDK_BINDINGS "$<TARGET_OBJECTS:s2sdk_bindings>")
elseif(EXISTS "${CMAKE_SOURCE_DIR}/sdk/include/s2sdk/s2sdk.cpp")
    add_library(round_end_sounds_s2sdk_bindings OBJECT
            "${CMAKE_SOURCE_DIR}/sdk/include/s2sdk/s2sdk.cpp")
    target_include_directories(round_end_sounds_s2sdk_bindings SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(round_end_sounds_s2sdk_bindings PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    set(ROUND_END_SOUNDS_S2SDK_BINDINGS "$<TARGET_OBJECTS:round_end_sounds_s2sdk_bindings>")
else()
    message(FATAL_ERROR
            "round_end_sounds needs the generated s2sdk bindings (sdk/include/s2sdk/s2sdk.cpp)")
endif()

if(TARGET permissions_bindings)
    set(ROUND_END_SOUNDS_PERMISSIONS_BINDINGS "$<TARGET_OBJECTS:permissions_bindings>")
elseif(EXISTS "${CMAKE_SOURCE_DIR}/sdk/include/permissions/permissions.cpp")
    add_library(round_end_sounds_permissions_bindings OBJECT
            "${CMAKE_SOURCE_DIR}/sdk/include/permissions/permissions.cpp")
    target_include_directories(round_end_sounds_permissions_bindings SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(round_end_sounds_permissions_bindings PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    set(ROUND_END_SOUNDS_PERMISSIONS_BINDINGS "$<TARGET_OBJECTS:round_end_sounds_permissions_bindings>")
else()
    message(FATAL_ERROR
            "round_end_sounds needs the permissions bindings (sdk/include/permissions/permissions.cpp)")
endif()

# hudmenu is declared as an OPTIONAL dependency: its GetClientMenuType() implements the player's
# own "!menu" choice exactly like pisexcompat did. The bindings are compiled in like every other
# plugify dependency (no shared wrapper library); when the plugin is absent the slots stay null and
# the default HUD/button menu type is used.
if(TARGET hudmenu_bindings)
    set(ROUND_END_SOUNDS_HUDMENU_BINDINGS "$<TARGET_OBJECTS:hudmenu_bindings>")
elseif(EXISTS "${CMAKE_SOURCE_DIR}/sdk/include/hudmenu/hudmenu.cpp")
    add_library(round_end_sounds_hudmenu_bindings OBJECT
            "${CMAKE_SOURCE_DIR}/sdk/include/hudmenu/hudmenu.cpp")
    target_include_directories(round_end_sounds_hudmenu_bindings SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(round_end_sounds_hudmenu_bindings PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    set(ROUND_END_SOUNDS_HUDMENU_BINDINGS "$<TARGET_OBJECTS:round_end_sounds_hudmenu_bindings>")
else()
    message(FATAL_ERROR
            "round_end_sounds needs the hudmenu bindings (sdk/include/hudmenu/hudmenu.cpp)")
endif()

# --- engine-independent core --------------------------------------------------
add_library(round_end_sounds_core STATIC src/re_core.cpp)
target_include_directories(round_end_sounds_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(round_end_sounds_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(round_end_sounds_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- plugin -------------------------------------------------------------------
add_library(plugify-round_end_sounds SHARED
        src/plugin.cpp
        src/sound_volume.cpp
        ${ROUND_END_SOUNDS_S2SDK_BINDINGS}
        ${ROUND_END_SOUNDS_PERMISSIONS_BINDINGS}
        ${ROUND_END_SOUNDS_HUDMENU_BINDINGS}
)
target_include_directories(plugify-round_end_sounds PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated plg/s2sdk/permissions headers must not fail -Werror.
target_include_directories(plugify-round_end_sounds SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-round_end_sounds PRIVATE
        plugify_sdk
        round_end_sounds_core
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-round_end_sounds PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-round_end_sounds PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="round_end_sounds"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-round_end_sounds PROPERTIES OUTPUT_NAME round_end_sounds)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-round_end_sounds PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/round_end_sounds.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/round_end_sounds.pplugin"
)

# --- tests --------------------------------------------------------------------
# The top-level CMakeLists declares PLUGIFY_PORT_TESTS *after* the plugin directories, so on a
# first configure it is not defined here yet: default it to the same ON. An explicit
# -DPLUGIFY_PORT_TESTS=OFF (the exported standalone build) is already in the cache and still wins.
if(NOT DEFINED PLUGIFY_PORT_TESTS)
    set(PLUGIFY_PORT_TESTS ON)
endif()
if(PLUGIFY_PORT_TESTS)
# round_end_sounds_tests: config/phrases (the production files), the picker, phrases, colour tags,
# preview/fade arithmetic, the packed volume parameter, the handle rule and every map-gate
# transition. No engine, no s2sdk.
add_executable(round_end_sounds_tests tests/re_core_tests.cpp)
target_link_libraries(round_end_sounds_tests PRIVATE
        round_end_sounds_core
        -static-libstdc++ -static-libgcc
)
target_compile_options(round_end_sounds_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(round_end_sounds_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        ROUND_END_SOUNDS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
)
set_target_properties(round_end_sounds_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

# round_end_sounds_runtime_tests: tests/round_end_sounds_runtime_tests.cpp includes
# ../src/plugin.cpp, so the real callbacks run against recording __s2sdk_* fakes (and the real
# permissions binding globals, left null unless a test sets them).
add_executable(round_end_sounds_runtime_tests
        tests/round_end_sounds_runtime_tests.cpp
        src/sound_volume.cpp
        ${ROUND_END_SOUNDS_S2SDK_BINDINGS}
        ${ROUND_END_SOUNDS_PERMISSIONS_BINDINGS}
        ${ROUND_END_SOUNDS_HUDMENU_BINDINGS}
)
target_include_directories(round_end_sounds_runtime_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated plg/s2sdk/permissions headers must not fail -Werror.
target_include_directories(round_end_sounds_runtime_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(round_end_sounds_runtime_tests PRIVATE
        plugify_sdk
        round_end_sounds_core
        -static-libstdc++ -static-libgcc
)
target_compile_options(round_end_sounds_runtime_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(round_end_sounds_runtime_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="round_end_sounds"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
        ROUND_END_SOUNDS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
)
set_target_properties(round_end_sounds_runtime_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()
