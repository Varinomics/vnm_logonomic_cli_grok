# An embedding build may already own the selected filesystem service.
set(GROK_FRAMEWORK_SOURCE "" CACHE PATH "Local vnm_framework source override")
if(NOT TARGET vnm_framework::vnm_filesystem_observation)
    if(GROK_FRAMEWORK_SOURCE)
        add_subdirectory("${GROK_FRAMEWORK_SOURCE}/cpp/filesystem_observation"
            "${CMAKE_CURRENT_BINARY_DIR}/filesystem_observation")
    else()
        find_package(vnm_filesystem_observation CONFIG QUIET)
        if(NOT TARGET vnm_framework::vnm_filesystem_observation)
            include(FetchContent)
            FetchContent_Declare(grok_framework
                GIT_REPOSITORY https://github.com/Varinomics/vnm_framework.git
                GIT_TAG master
                GIT_SHALLOW TRUE
                SOURCE_SUBDIR cpp/filesystem_observation)
            FetchContent_MakeAvailable(grok_framework)
        endif()
    endif()
endif()
find_package(Qt6 6.12 REQUIRED COMPONENTS Core)

# Retain actual selected sources for the isolated package recipe. Imported
# targets retain their selected package directory instead of inventing sources.
get_target_property(grok_observation_imported
    vnm_framework::vnm_filesystem_observation IMPORTED)
if(NOT grok_observation_imported)
    get_target_property(grok_observation_source
        vnm_framework::vnm_filesystem_observation SOURCE_DIR)
    get_filename_component(grok_framework_source "${grok_observation_source}/../.." ABSOLUTE)
endif()
get_target_property(grok_dispatch_imported vnm_qt_dispatch::vnm_qt_dispatch IMPORTED)
if(NOT grok_dispatch_imported)
    get_target_property(grok_dispatch_source vnm_qt_dispatch::vnm_qt_dispatch SOURCE_DIR)
endif()
include(FetchContent)
FetchContent_GetProperties(vnm_cmake SOURCE_DIR grok_vnm_cmake_source)
