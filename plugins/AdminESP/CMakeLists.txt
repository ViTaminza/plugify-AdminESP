# AdminESP — Plugify C++ plugin: admin-only player outlines for the admin_system core.
# Port of the Metamod:Source plugin "AdminESP" (ref/src/AdminESP).
#
#   adminesp_core       engine-free config/access/colour logic (also used by
#                       test/adminesp_core_tests.cpp)
#   plugify-AdminESP    the plugin shared library (entry "bin/AdminESP" -> bin/libAdminESP.so)
#
# Like every admin_system module it links:
#   * pisexcompat (through the core; brings the s2sdk binding globals),
#   * the generated admin_system consumer bindings (__admin_system_*) and the hand-written
#     consumer wrapper plugins/admin_system/include/admin_api.hpp.

set(ADMINESP_VERSION "2.4.0")

# --- core ---------------------------------------------------------------------
add_library(adminesp_core STATIC src/adminesp_core.cpp)
target_include_directories(adminesp_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# pisexcompat brings the KeyValues parser (config/phrases) and, through its own PUBLIC link, the
# s2sdk binding globals.
target_link_libraries(adminesp_core PUBLIC pisexcompat)
target_compile_options(adminesp_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(adminesp_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- generated admin_system bindings -----------------------------------------
# Every consumer plugin needs the __admin_system_* globals compiled into its own library.
# plugins/admin_system provides admin_system_bindings_obj, but "AdminESP" sorts *before*
# "admin_system" (list(SORT) in the top-level CMakeLists is case sensitive: 'A' < 'a'), so that
# target does not exist yet while this file is configured. The same checked-in generated file is
# therefore compiled into a private object library here; a second copy of the globals inside
# another .so is exactly how the binding pattern works (see s2sdk_bindings).
add_library(adminesp_admin_system_bindings OBJECT
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include/admin_system/admin_system.cpp")
target_include_directories(adminesp_admin_system_bindings SYSTEM PUBLIC
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_compile_definitions(adminesp_admin_system_bindings PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- plugin -------------------------------------------------------------------
add_library(plugify-AdminESP SHARED
        src/plugin.cpp
        # Binding globals (__s2sdk_* / __admin_system_* / __hudmenu_*) filled by the plugify
        # module from the plugins declared as dependencies.
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:adminesp_admin_system_bindings>
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
# Engine-free tests of the core against the production adminesp.ini. The binary is written into
# build/tests so ../test.sh runs it with the rest (*_tests convention).
add_executable(adminesp_core_tests test/adminesp_core_tests.cpp)
target_link_libraries(adminesp_core_tests PRIVATE adminesp_core -static-libstdc++ -static-libgcc)
target_compile_options(adminesp_core_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(adminesp_core_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        ADMINESP_TEST_CONFIG="${CMAKE_SOURCE_DIR}/ref/prod-27016/addons/configs/adminesp.ini"
        ADMINESP_TEST_PHRASES="${CMAKE_SOURCE_DIR}/ref/prod-27016/addons/translations/AdminESP.phrases.txt")
set_target_properties(adminesp_core_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
