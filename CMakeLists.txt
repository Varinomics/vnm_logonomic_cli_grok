cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0141 NEW)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
# A build that embeds this package owns its cache, its try_compile settings and
# the SDK (the SDK's docs/package_contract.md, "Embedded build").
if(NOT LOGONOMIC_CLI_ADDON_EMBEDDED)
    set(CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV OFF CACHE BOOL "Do not capture the configure environment" FORCE)
    list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV)
endif()
if(POLICY CMP0168)
    cmake_policy(SET CMP0168 NEW)
elseif(CMAKE_GENERATOR STREQUAL "FASTBuild")
    message(FATAL_ERROR "FASTBuild package builds require direct FetchContent population (CMP0168).")
endif()
project(logonomic_cli_grok VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(LOGONOMIC_CLI_ADDON_EMBEDDED)
    if(NOT TARGET logonomic_cli_addon_sdk::sdk)
        message(FATAL_ERROR "The embedding build must add the Logonomic CLI add-on SDK before this package.")
    endif()
else()
    set(LOGONOMIC_CLI_ADDON_SDK_SOURCE "" CACHE PATH "Local Logonomic CLI add-on SDK source override")
    set(LOGONOMIC_CLI_ADDON_SDK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    if(LOGONOMIC_CLI_ADDON_SDK_SOURCE)
        add_subdirectory("${LOGONOMIC_CLI_ADDON_SDK_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/sdk")
    else()
        include(FetchContent)
        FetchContent_Declare(logonomic_sdk_source
            GIT_REPOSITORY https://github.com/Varinomics/logonomic.git
            GIT_TAG master
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR sdk/logonomic_cli_addon_sdk)
        FetchContent_MakeAvailable(logonomic_sdk_source)
    endif()
endif()

include(cmake/grok_observation_dependency.cmake)
logonomic_cli_addon_add_filesystem_observer()

function(grok_target target)
    target_link_libraries(${target} PRIVATE logonomic_cli_addon_sdk::sdk)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
    if(WIN32)
        target_compile_definitions(${target} PRIVATE
            _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    endif()
endfunction()

add_library(grok_provider STATIC
    src/grok_accounts.cpp
    src/grok_acp.cpp
    src/grok_launcher.cpp
    src/grok_launcher_approval.cpp
    src/grok_configuration.cpp
    src/grok_context.cpp
    src/grok_lifecycle.cpp
    src/grok_observer.cpp
    src/grok_preparation.cpp
    src/grok_resume.cpp
    src/grok_probes.cpp
    src/grok_usage.cpp
    src/grok_usage_records.cpp)
target_sources(grok_provider PRIVATE
    src/grok_wake.cpp src/grok_wake_protocol.cpp src/grok_wake_identity.cpp
    src/grok_wake_transport_win.cpp src/grok_wake_transport_posix.cpp)
grok_target(grok_provider)
target_include_directories(grok_provider PUBLIC src)
target_link_libraries(grok_provider PUBLIC logonomic_cli_addon_sdk::sdk
    logonomic_cli_addon_sdk::filesystem_observer)
if(WIN32)
    target_link_libraries(grok_provider PRIVATE advapi32)
endif()

add_executable(grok_adapter src/adapter.cpp)
target_link_libraries(grok_adapter PRIVATE grok_provider)
grok_target(grok_adapter)
add_executable(grok_helper src/grok_helper.cpp)
target_link_libraries(grok_helper PRIVATE grok_provider)
grok_target(grok_helper)
logonomic_cli_addon_add_launch_bridge(grok_launch_bridge)
grok_target(grok_launch_bridge)
add_executable(grok_session_bridge src/grok_session_bridge.cpp)
target_link_libraries(grok_session_bridge PRIVATE grok_provider)
grok_target(grok_session_bridge)
if(MINGW)
    target_link_options(grok_session_bridge PRIVATE -municode)
endif()
add_executable(grok_quota_bridge
    src/grok_billing.cpp
    src/grok_billing_http.cpp
    src/grok_quota_bridge.cpp)
target_link_libraries(grok_quota_bridge PRIVATE grok_provider)
grok_target(grok_quota_bridge)
if(WIN32)
    target_link_libraries(grok_quota_bridge PRIVATE winhttp)
else()
    find_package(CURL REQUIRED)
    target_link_libraries(grok_quota_bridge PRIVATE CURL::libcurl)
endif()

logonomic_cli_addon_package_platform(GROK_PLATFORM)
set(GROK_EXECUTABLE_SUFFIX "${CMAKE_EXECUTABLE_SUFFIX}")
configure_file(package/logonomic-cli-addon.json.in generated/logonomic-cli-addon.json @ONLY)
set(GROK_QTBASE_SOURCE "" CACHE PATH "qtbase source matching the selected Qt runtime (notices)")
set(GROK_RUNTIME_NOTICES_DIRECTORY "" CACHE PATH
    "Copyright/license files for external runtimes, named <library filename>.txt")
set(GROK_LINUX_RUNTIME_MATERIAL "" CACHE FILEPATH
    "Exact prebuilt input provenance from the selected Qt/runtime acquisition")
logonomic_cli_addon_add_qt_package(grok_package logonomic.cli.grok GROK_PACKAGE_DIRECTORY
    ADAPTER grok_adapter
    HELPERS grok_helper grok_launch_bridge grok_session_bridge grok_quota_bridge
    QTBASE_SOURCE "${GROK_QTBASE_SOURCE}"
    RUNTIME_NOTICES "${GROK_RUNTIME_NOTICES_DIRECTORY}"
    LINUX_RUNTIME_MATERIAL "${GROK_LINUX_RUNTIME_MATERIAL}"
    OUT_RUNTIME_CHECK GROK_RUNTIME_CHECK
    OUT_QTBASE_SOURCE GROK_QTBASE_SOURCE
    INDEX_ARGUMENTS
        --third-party "assets/grok.svg" "notices/lobe_icons_mit.txt"
            "provenance/lobe_icons.json"
        --third-party "assets/grok-dark.svg" "notices/lobe_icons_mit.txt"
            "provenance/lobe_icons.json")

# The tests belong to the package's own build. A build that embeds the package
# stages it and runs tests of its own, so nothing below is added there.
if(LOGONOMIC_CLI_ADDON_EMBEDDED)
    return()
endif()
enable_testing()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
get_target_property(_grok_sdk_source logonomic_cli_addon_sdk::sdk SOURCE_DIR)
add_test(NAME grok_dependency_preparation_tests
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/.github/scripts/test_prepare_ci.py"
        --sdk "${_grok_sdk_source}")
add_test(NAME grok_package_runtime_closure COMMAND ${GROK_RUNTIME_CHECK})
set_tests_properties(grok_dependency_preparation_tests grok_package_runtime_closure PROPERTIES TIMEOUT 120)
add_executable(grok_observer_tests tests/grok_observer_tests.cpp)
target_link_libraries(grok_observer_tests PRIVATE grok_provider)
grok_target(grok_observer_tests)
add_test(NAME grok_observer_tests COMMAND grok_observer_tests)
set_tests_properties(grok_observer_tests PROPERTIES TIMEOUT 90)
foreach(test_name IN ITEMS
    grok_runtime_tests grok_initial_prompt_tests grok_acp_tests grok_probe_tests
    grok_usage_tests grok_wake_protocol_tests grok_wake_identity_tests grok_context_tests)
    add_executable(${test_name} "tests/${test_name}.cpp")
    target_link_libraries(${test_name} PRIVATE grok_provider)
    grok_target(${test_name})
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 120)
endforeach()
target_compile_definitions(grok_runtime_tests PRIVATE
    GROK_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
if(MINGW)
    target_link_options(grok_initial_prompt_tests PRIVATE -municode)
endif()
if(CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$")
    add_executable(grok_wake_transport_posix_tests tests/grok_wake_transport_posix_tests.cpp)
    target_link_libraries(grok_wake_transport_posix_tests PRIVATE grok_provider)
    grok_target(grok_wake_transport_posix_tests)
    add_dependencies(grok_wake_transport_posix_tests grok_session_bridge)
    add_test(NAME grok_wake_transport_posix_tests COMMAND grok_wake_transport_posix_tests
        --session-bridge "$<TARGET_FILE:grok_session_bridge>")
    set_tests_properties(grok_wake_transport_posix_tests PROPERTIES TIMEOUT 30)
endif()
if(WIN32)
    add_executable(grok_wake_transport_win_tests tests/grok_wake_transport_win_tests.cpp)
    target_link_libraries(grok_wake_transport_win_tests PRIVATE grok_provider)
    grok_target(grok_wake_transport_win_tests)
    add_dependencies(grok_wake_transport_win_tests grok_session_bridge)
    add_test(NAME grok_wake_transport_win_tests COMMAND grok_wake_transport_win_tests
        --session-bridge "$<TARGET_FILE:grok_session_bridge>")
    set_tests_properties(grok_wake_transport_win_tests PROPERTIES TIMEOUT 60)
endif()
add_executable(grok_wake_package_tests tests/grok_wake_package_tests.cpp)
target_link_libraries(grok_wake_package_tests PRIVATE grok_provider)
grok_target(grok_wake_package_tests)
add_dependencies(grok_wake_package_tests grok_package)
add_test(NAME grok_wake_package_tests COMMAND grok_wake_package_tests "${GROK_PACKAGE_DIRECTORY}")
set_tests_properties(grok_wake_package_tests PROPERTIES TIMEOUT 45)
# Link the production request loop to a scripted runtime without replacing its
# cancellation/deadline handling or starting a native provider process.
add_library(grok_wake_bridge_fixture OBJECT src/grok_session_bridge.cpp)
grok_target(grok_wake_bridge_fixture)
target_compile_definitions(grok_wake_bridge_fixture PRIVATE
    main=grok_fixture_entry wmain=grok_fixture_wentry)
add_executable(grok_wake_bridge_tests tests/grok_wake_bridge_tests.cpp)
target_link_libraries(grok_wake_bridge_tests PRIVATE grok_wake_bridge_fixture grok_provider)
grok_target(grok_wake_bridge_tests)
add_test(NAME grok_wake_bridge_tests COMMAND grok_wake_bridge_tests)
set_tests_properties(grok_wake_bridge_tests PROPERTIES TIMEOUT 45)
add_executable(grok_attention_bridge_tests tests/grok_attention_bridge_tests.cpp)
target_link_libraries(grok_attention_bridge_tests PRIVATE grok_wake_bridge_fixture grok_provider)
grok_target(grok_attention_bridge_tests)
add_dependencies(grok_attention_bridge_tests grok_package)
add_test(NAME grok_attention_bridge_tests COMMAND grok_attention_bridge_tests "${GROK_PACKAGE_DIRECTORY}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/pending_interactions.jsonl"
    "${CMAKE_CURRENT_BINARY_DIR}/grok-attention-receipt.jsonl")
set_tests_properties(grok_attention_bridge_tests PROPERTIES TIMEOUT 45)
# The billing tests build the helper's own sources against a scripted HTTPS
# transport; the packaged helper itself runs in grok_quota_bridge_tests.
add_executable(grok_billing_tests tests/grok_billing_tests.cpp src/grok_billing.cpp)
target_link_libraries(grok_billing_tests PRIVATE grok_provider)
grok_target(grok_billing_tests)
add_test(NAME grok_billing_tests COMMAND grok_billing_tests)
set_tests_properties(grok_billing_tests PROPERTIES TIMEOUT 120)
add_test(NAME grok_quota_bridge_tests COMMAND "${CMAKE_COMMAND}"
    "-DBRIDGE=$<TARGET_FILE:grok_quota_bridge>"
    "-DWORK_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/quota-bridge-fixture"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/grok_quota_bridge_tests.cmake")
set_tests_properties(grok_quota_bridge_tests PROPERTIES TIMEOUT 120)
add_executable(grok_lifecycle_tests tests/grok_lifecycle_tests.cpp)
target_link_libraries(grok_lifecycle_tests PRIVATE grok_provider)
grok_target(grok_lifecycle_tests)
add_dependencies(grok_lifecycle_tests grok_package)
add_test(NAME grok_lifecycle_tests COMMAND grok_lifecycle_tests "${GROK_PACKAGE_DIRECTORY}")
set_tests_properties(grok_lifecycle_tests PROPERTIES TIMEOUT 120)
add_executable(grok_resume_tests tests/grok_resume_tests.cpp)
target_link_libraries(grok_resume_tests PRIVATE grok_provider)
grok_target(grok_resume_tests)
add_dependencies(grok_resume_tests grok_package)
add_test(NAME grok_resume_tests COMMAND grok_resume_tests "${GROK_PACKAGE_DIRECTORY}")
set_tests_properties(grok_resume_tests PROPERTIES TIMEOUT 120)
add_test(NAME grok_package_validation COMMAND logonomic_cli_addon_validate "${GROK_PACKAGE_DIRECTORY}" --platform "${GROK_PLATFORM}")
add_test(NAME grok_package_conformance COMMAND logonomic_cli_addon_conformance "${GROK_PACKAGE_DIRECTORY}")
add_executable(grok_usage_cache_tests tests/grok_usage_cache_tests.cpp)
target_link_libraries(grok_usage_cache_tests PRIVATE
    grok_provider logonomic_cli_addon_sdk::test_support)
grok_target(grok_usage_cache_tests)
add_dependencies(grok_usage_cache_tests grok_package)
add_test(NAME grok_usage_cache_tests
    COMMAND grok_usage_cache_tests "${GROK_PACKAGE_DIRECTORY}")
set_tests_properties(grok_usage_cache_tests PROPERTIES TIMEOUT 180 LABELS "usage;cache;packaged-adapter")

add_executable(grok_fake_vnmctl tests/grok_fake_vnmctl.cpp)
grok_target(grok_fake_vnmctl)
add_dependencies(grok_fake_vnmctl grok_package)
add_test(NAME grok_helper_gate_tests COMMAND "${CMAKE_COMMAND}"
    "-DHELPER=${GROK_PACKAGE_DIRECTORY}/adapter/grok_helper${CMAKE_EXECUTABLE_SUFFIX}"
    "-DCONFIGURATION=$<TARGET_FILE:grok_runtime_tests>"
    "-DVNMCTL=$<TARGET_FILE:grok_fake_vnmctl>"
    "-DWORK_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/helper-gate-fixture"
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
    FetchContent_GetProperties(logonomic_cli_addon_qt_notices SOURCE_DIR _grok_qt_notices_source)
    add_test(NAME grok_package_membership
        COMMAND "${Python3_EXECUTABLE}" "${_membership_sdk_source}/cmake/package_membership_tests.py"
            "--source=${CMAKE_CURRENT_SOURCE_DIR}" "--sdk=${_membership_sdk_source}"
            "--cmake=${CMAKE_COMMAND}" "--target=grok_package" "--package-id=logonomic.cli.grok"
            "--generator=${CMAKE_GENERATOR}" "--config=$<CONFIG>" "--build-type=${CMAKE_BUILD_TYPE}"
            "--generator-platform=${CMAKE_GENERATOR_PLATFORM}" "--toolset=${CMAKE_GENERATOR_TOOLSET}"
            "--toolchain=${CMAKE_TOOLCHAIN_FILE}" "--compiler=${CMAKE_CXX_COMPILER}"
            "--make-program=${CMAKE_MAKE_PROGRAM}" "--prefix-path=${CMAKE_PREFIX_PATH}"
            "--cmake-definition=GROK_FRAMEWORK_SOURCE=${grok_framework_source}"
            "--cmake-definition=FETCHCONTENT_SOURCE_DIR_VNM_QT_DISPATCH=${grok_dispatch_source}"
            "--cmake-definition=FETCHCONTENT_SOURCE_DIR_VNM_CMAKE=${grok_vnm_cmake_source}"
            "--cmake-definition=vnm_filesystem_observation_DIR=${vnm_filesystem_observation_DIR}"
            "--cmake-definition=vnm_qt_dispatch_DIR=${vnm_qt_dispatch_DIR}"
            "--cmake-definition=GROK_QTBASE_SOURCE=${GROK_QTBASE_SOURCE}"
            "--cmake-definition=FETCHCONTENT_SOURCE_DIR_LOGONOMIC_CLI_ADDON_QT_NOTICES=${_grok_qt_notices_source}"
            "--cmake-definition=GROK_RUNTIME_NOTICES_DIRECTORY=${GROK_RUNTIME_NOTICES_DIRECTORY}"
            "--cmake-definition=GROK_LINUX_RUNTIME_MATERIAL=${GROK_LINUX_RUNTIME_MATERIAL}"
            "--cmake-definition=CMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}"
            "--cmake-definition=VNM_FILESYSTEM_OBSERVATION_BUILD_TESTS=OFF"
            "--cmake-definition=VNM_QT_DISPATCH_BUILD_TESTING=OFF"
            "--validator=$<TARGET_FILE:logonomic_cli_addon_validate>" "--platform=${GROK_PLATFORM}")
    set_tests_properties(grok_package_membership PROPERTIES
        TIMEOUT 1800 RESOURCE_LOCK cli_addon_recipe_rebuild LABELS "cli-addon;delivery;recipe;owning-path")
endif()
# Contract and recipe tests must not be confused with real native qualification.
get_directory_property(_provider_tests TESTS)
set_property(TEST ${_provider_tests} APPEND PROPERTY LABELS "cli-addon;provider-contract")
