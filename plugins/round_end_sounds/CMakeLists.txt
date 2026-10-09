# round_end_sounds — Plugify C++ port of the Metamod:Source plugin ref/src/cs2-round-end-sounds
# (a track at the end of the round, per-player on/off + volume, MVP-anthem muting).
#
#   round_end_sounds_core     engine-free core (config, track picking, phrases; also used by
#                             test/re_core_tests.cpp)
#   plugify-round_end_sounds  the plugin shared library (entry "bin/round_end_sounds")

set(ROUND_END_SOUNDS_VERSION "1.0.1")

# --- core ---------------------------------------------------------------------
add_library(round_end_sounds_core STATIC src/re_core.cpp)
target_include_directories(round_end_sounds_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(round_end_sounds_core PUBLIC pisexcompat)
target_compile_options(round_end_sounds_core PRIVATE -Wall -Wextra -Werror)
# SYSTEM: warnings from the generated s2sdk/plg headers must not fail -Werror on our sources.
target_include_directories(round_end_sounds_core SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_compile_definitions(round_end_sounds_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- plugin -------------------------------------------------------------------
add_library(plugify-round_end_sounds SHARED
        src/plugin.cpp
        src/sound_volume.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:permissions_bindings>
)
target_include_directories(plugify-round_end_sounds PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(plugify-round_end_sounds SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-round_end_sounds PRIVATE
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

# --- tests ---------------------------------------------------------------------
add_executable(round_end_sounds_tests test/re_core_tests.cpp)
target_link_libraries(round_end_sounds_tests PRIVATE round_end_sounds_core -static-libstdc++ -static-libgcc)
target_compile_options(round_end_sounds_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(round_end_sounds_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        RES_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/ref/prod-27016/addons")
set_target_properties(round_end_sounds_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
