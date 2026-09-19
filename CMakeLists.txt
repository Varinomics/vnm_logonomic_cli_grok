cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0141 NEW)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)
set(CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV OFF CACHE BOOL "Do not capture the configure environment" FORCE)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV)
if(POLICY CMP0168)
    cmake_policy(SET CMP0168 NEW)
elseif(CMAKE_GENERATOR STREQUAL "FASTBuild")
    message(FATAL_ERROR "FASTBuild package builds require direct FetchContent population (CMP0168).")
endif()
project(logonomic_cli_grok VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(LOGONOMIC_CLI_ADDON_SDK_SOURCE "" CACHE PATH "Local Logonomic CLI add-on SDK source override")
set(LOGONOMIC_CLI_ADDON_SDK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
if(LOGONOMIC_CLI_ADDON_SDK_SOURCE)
    add_subdirectory("${LOGONOMIC_CLI_ADDON_SDK_SOURCE}" "${CMAKE_BINARY_DIR}/sdk")
else()
    include(FetchContent)
    FetchContent_Declare(logonomic_sdk_source
        GIT_REPOSITORY https://github.com/Varinomics/logonomic.git
        GIT_TAG master
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR sdk/logonomic_cli_addon_sdk)
    FetchContent_MakeAvailable(logonomic_sdk_source)
endif()

function(grok_target target)
    target_link_libraries(${target} PRIVATE logonomic_cli_addon_sdk::sdk)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        target_compile_definitions(${target} PRIVATE
            _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

add_library(grok_provider STATIC
    src/grok_accounts.cpp
    src/grok_acp.cpp
    src/grok_launcher.cpp
    src/grok_configuration.cpp
    src/grok_lifecycle.cpp
    src/grok_preparation.cpp
    src/grok_probes.cpp
    src/grok_usage.cpp
    src/grok_usage_records.cpp)
grok_target(grok_provider)
target_include_directories(grok_provider PUBLIC src)
target_link_libraries(grok_provider PUBLIC logonomic_cli_addon_sdk::sdk)

add_executable(grok_adapter src/adapter.cpp)
target_link_libraries(grok_adapter PRIVATE grok_provider)
grok_target(grok_adapter)
add_executable(grok_helper src/grok_helper.cpp)
grok_target(grok_helper)
logonomic_cli_addon_add_launch_bridge(grok_launch_bridge)
grok_target(grok_launch_bridge)

logonomic_cli_addon_package_platform(GROK_PLATFORM)
set(GROK_EXECUTABLE_SUFFIX "${CMAKE_EXECUTABLE_SUFFIX}")
logonomic_cli_addon_package_directory(logonomic.cli.grok GROK_PACKAGE_DIRECTORY)
configure_file(package/logonomic-cli-addon.json.in generated/logonomic-cli-addon.json @ONLY)
file(GLOB_RECURSE package_files CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/package/assets/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/package/capabilities/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/package/provenance/*")
# Directory membership is an input too: removing a resource, or adding one
# with an older timestamp, must rebuild the indexed package. file(GENERATE)
# changes this file only when membership changes, preserving no-op builds.
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/package-inputs.txt"
    CONTENT "${package_files}\n")
add_custom_command(
    OUTPUT "${GROK_PACKAGE_DIRECTORY}/package.files.json"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${GROK_PACKAGE_DIRECTORY}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${GROK_PACKAGE_DIRECTORY}/adapter" "${GROK_PACKAGE_DIRECTORY}/helpers"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/package/assets" "${GROK_PACKAGE_DIRECTORY}/assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/package/capabilities" "${GROK_PACKAGE_DIRECTORY}/capabilities"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/package/provenance" "${GROK_PACKAGE_DIRECTORY}/provenance"
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_BINARY_DIR}/generated/logonomic-cli-addon.json" "${GROK_PACKAGE_DIRECTORY}/logonomic-cli-addon.json"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:grok_adapter>" "${GROK_PACKAGE_DIRECTORY}/adapter/grok_adapter${GROK_EXECUTABLE_SUFFIX}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:grok_helper>" "${GROK_PACKAGE_DIRECTORY}/helpers/grok_helper${GROK_EXECUTABLE_SUFFIX}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:grok_launch_bridge>" "${GROK_PACKAGE_DIRECTORY}/helpers/grok_launch_bridge${GROK_EXECUTABLE_SUFFIX}"
    COMMAND "$<TARGET_FILE:logonomic_cli_addon_index>" "${GROK_PACKAGE_DIRECTORY}"
    DEPENDS grok_adapter grok_helper grok_launch_bridge logonomic_cli_addon_index
        "${CMAKE_BINARY_DIR}/generated/logonomic-cli-addon.json" "${CMAKE_CURRENT_BINARY_DIR}/package-inputs.txt" ${package_files}
    VERBATIM)
add_custom_target(grok_package ALL DEPENDS "${GROK_PACKAGE_DIRECTORY}/package.files.json")

enable_testing()
foreach(test_name IN ITEMS grok_runtime_tests grok_acp_tests grok_probe_tests grok_usage_tests)
    add_executable(${test_name} "tests/${test_name}.cpp")
    target_link_libraries(${test_name} PRIVATE grok_provider)
    grok_target(${test_name})
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 120)
endforeach()
add_test(NAME grok_package_validation COMMAND logonomic_cli_addon_validate "${GROK_PACKAGE_DIRECTORY}" --platform "${GROK_PLATFORM}")
add_test(NAME grok_package_conformance COMMAND logonomic_cli_addon_conformance "${GROK_PACKAGE_DIRECTORY}")
add_executable(grok_usage_cache_tests tests/grok_usage_cache_tests.cpp)
target_link_libraries(grok_usage_cache_tests PRIVATE grok_provider)
grok_target(grok_usage_cache_tests)
add_dependencies(grok_usage_cache_tests grok_package)
add_test(NAME grok_usage_cache_tests
    COMMAND grok_usage_cache_tests "${GROK_PACKAGE_DIRECTORY}")
set_tests_properties(grok_usage_cache_tests PROPERTIES TIMEOUT 180 LABELS "usage;cache;packaged-adapter")
logonomic_cli_addon_install_package("${GROK_PACKAGE_DIRECTORY}" logonomic.cli.grok)

add_executable(grok_fake_vnmctl tests/grok_fake_vnmctl.cpp)
grok_target(grok_fake_vnmctl)
add_test(NAME grok_helper_gate_tests COMMAND "${CMAKE_COMMAND}"
    "-DHELPER=$<TARGET_FILE:grok_helper>" "-DVNMCTL=$<TARGET_FILE:grok_fake_vnmctl>"
    "-DWORK_DIRECTORY=${CMAKE_BINARY_DIR}/helper-gate-fixture"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/grok_helper_gate_tests.cmake")
set_tests_properties(grok_helper_gate_tests PROPERTIES TIMEOUT 15)

# Exercise this repository's actual producer, never a substitute packaging
# implementation. The isolated nested configure uses this same SDK source;
# it does not change the owned master dependency or edit an installed package.
option(LOGONOMIC_CLI_ADDON_RECIPE_TESTS
    "Register isolated package rebuild regressions (requires Python 3)" OFF)
if(LOGONOMIC_CLI_ADDON_RECIPE_TESTS)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    get_target_property(_membership_sdk_source logonomic_cli_addon_sdk::sdk SOURCE_DIR)
    add_test(NAME grok_package_membership
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_membership_tests.py"
            "--source=${CMAKE_CURRENT_SOURCE_DIR}" "--sdk=${_membership_sdk_source}"
            "--cmake=${CMAKE_COMMAND}" "--target=grok_package" "--package-id=logonomic.cli.grok"
            "--generator=${CMAKE_GENERATOR}" "--config=$<CONFIG>" "--build-type=${CMAKE_BUILD_TYPE}"
            "--generator-platform=${CMAKE_GENERATOR_PLATFORM}" "--toolset=${CMAKE_GENERATOR_TOOLSET}"
            "--toolchain=${CMAKE_TOOLCHAIN_FILE}" "--compiler=${CMAKE_CXX_COMPILER}"
            "--make-program=${CMAKE_MAKE_PROGRAM}" "--prefix-path=${CMAKE_PREFIX_PATH}"
            "--validator=$<TARGET_FILE:logonomic_cli_addon_validate>" "--platform=${GROK_PLATFORM}")
    set_tests_properties(grok_package_membership PROPERTIES
        TIMEOUT 1800 RESOURCE_LOCK cli_addon_recipe_rebuild LABELS "cli-addon;delivery;recipe;owning-path")
endif()
# Contract and recipe tests must not be confused with real native qualification.
get_directory_property(_provider_tests TESTS)
set_property(TEST ${_provider_tests} APPEND PROPERTY LABELS "cli-addon;provider-contract")
