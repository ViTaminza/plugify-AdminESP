# AdminESP — Plugify C++ plugin: admin-only player outlines for the admin_system core.
# Port of the Metamod:Source plugin "AdminESP".
#
#   adminesp_core       engine-free config/access/colour/phrase logic (also used by
#                       test/adminesp_core_tests.cpp)
#   plugify-AdminESP    the plugin shared library (entry "bin/AdminESP" -> bin/libAdminESP.so)
#
# Pure s2sdk: no pisexcompat and no s2guard (docs/PLUGIN_STANDARD.md §7). The guards of
# handoff/weaponskins/PITFALLS.md live in src/plugin.cpp itself - the B1/B2 map gate, the C2
# client checks, the C8 schema lookups, the C11/C12 command/event ownership and the A7 teardown.
# Like every admin_system module the .so compiles the binding globals the plugify module fills at
# load time:
#   * s2sdk (__s2sdk_*),
#   * the generated admin_system consumer bindings (__admin_system_*) plus the hand-written
#     consumer wrapper plugins/admin_system/include/admin_api.hpp.

set(ADMINESP_VERSION "2.5.0")

# --- core ---------------------------------------------------------------------
add_library(adminesp_core STATIC src/adminesp_core.cpp)
target_include_directories(adminesp_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(adminesp_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(adminesp_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- generated admin_system bindings -----------------------------------------
# Every consumer plugin needs the __admin_system_* globals compiled into its own library.
# plugins/admin_system provides admin_system_bindings_obj, but "AdminESP" sorts *before*
# "admin_system" (list(SORT) in the top-level CMakeLists is case sensitive: 'A' < 'a'), so that
# target does not exist yet while this file is configured. The fallback target below therefore
# carries a *unique* name (a generic one would collide with the object library admin_system
# creates unconditionally later in the same configure run); use the shared target when it does
# exist, e.g. when this directory is built on its own after admin_system.
if(TARGET admin_system_bindings_obj)
    set(ADMINESP_ADMIN_SYSTEM_BINDINGS_TARGET admin_system_bindings_obj)
else()
    add_library(adminesp_admin_system_bindings_obj OBJECT
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include/admin_system/admin_system.cpp")
    target_include_directories(adminesp_admin_system_bindings_obj SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(adminesp_admin_system_bindings_obj PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    set(ADMINESP_ADMIN_SYSTEM_BINDINGS_TARGET adminesp_admin_system_bindings_obj)
endif()

# --- plugin -------------------------------------------------------------------
add_library(plugify-AdminESP SHARED
        src/plugin.cpp
        # Binding globals (__s2sdk_* / __admin_system_*) filled by the plugify module from the
        # plugins declared as dependencies.
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:${ADMINESP_ADMIN_SYSTEM_BINDINGS_TARGET}>
)
target_include_directories(plugify-AdminESP PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: the generated admin_system/s2sdk/plg headers and admin_api.hpp are not ours to fix;
# their warnings must not fail -Werror on our sources.
target_include_directories(plugify-AdminESP SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include"
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-AdminESP PRIVATE
        adminesp_core
        plugify_sdk
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-AdminESP PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-AdminESP PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="AdminESP"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-AdminESP PROPERTIES OUTPUT_NAME AdminESP)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-AdminESP PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/AdminESP.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/AdminESP.pplugin"
)

# --- tests ---------------------------------------------------------------------
# test/adminesp_core_tests: engine-free core (production adminesp.ini / AdminESP.phrases.txt,
# KV1 grammar, colours, phrases, access, signed handles).
# test/adminesp_plugin_tests: the plugin TU is compiled into the test and every __s2sdk_* /
# __admin_system_* binding slot is a recording fake: the protected-ESP transmit scenario, signed
# handles and the retire list, the identity-bound pending rebuilds, command ownership/contexts,
# the map/schema gate, teardown and the fail-soft starts. Written into build/tests so ../test.sh
# runs them with the rest (*_tests convention).
#
# The top-level CMakeLists declares the PLUGIFY_PORT_TESTS option *after* the plugin directories,
# so on a first configure it is not defined here yet: default it to the same ON. An explicit
# -DPLUGIFY_PORT_TESTS=OFF is already in the cache and still wins.
if(NOT DEFINED PLUGIFY_PORT_TESTS)
    set(PLUGIFY_PORT_TESTS ON)
endif()
if(PLUGIFY_PORT_TESTS)
    add_executable(adminesp_core_tests test/adminesp_core_tests.cpp)
    target_link_libraries(adminesp_core_tests PRIVATE adminesp_core -static-libstdc++ -static-libgcc)
    target_compile_options(adminesp_core_tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(adminesp_core_tests PRIVATE
            _GLIBCXX_USE_CXX11_ABI=1
            ADMINESP_TEST_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/test/data/adminesp.ini"
            ADMINESP_TEST_PHRASES="${CMAKE_CURRENT_SOURCE_DIR}/test/data/AdminESP.phrases.txt")
    set_target_properties(adminesp_core_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

    add_executable(adminesp_plugin_tests
            test/adminesp_plugin_tests.cpp
            $<TARGET_OBJECTS:s2sdk_bindings>
            $<TARGET_OBJECTS:${ADMINESP_ADMIN_SYSTEM_BINDINGS_TARGET}>
    )
    target_include_directories(adminesp_plugin_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_include_directories(adminesp_plugin_tests SYSTEM PRIVATE
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include"
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_link_libraries(adminesp_plugin_tests PRIVATE
            adminesp_core
            plugify_sdk
            -static-libstdc++ -static-libgcc)
    target_compile_options(adminesp_plugin_tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(adminesp_plugin_tests PRIVATE
            _GLIBCXX_USE_CXX11_ABI=1
            PLUGIFY_MODULE="AdminESP"
            PLUGIFY_IS_DEBUG=0
            PLUGIFY_IS_RELEASE=1
            ADMINESP_TEST_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/test/data/adminesp.ini"
            ADMINESP_TEST_PHRASES="${CMAKE_CURRENT_SOURCE_DIR}/test/data/AdminESP.phrases.txt")
    set_target_properties(adminesp_plugin_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()
