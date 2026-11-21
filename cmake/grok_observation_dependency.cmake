# An embedding build may already own the selected filesystem service.
set(GROK_OBSERVATION_SOURCE "" CACHE PATH "Local vnm_filesystem_observation source override")
if(NOT TARGET vnm_framework::vnm_filesystem_observation)
    if(GROK_OBSERVATION_SOURCE)
        add_subdirectory("${GROK_OBSERVATION_SOURCE}"
            "${CMAKE_CURRENT_BINARY_DIR}/filesystem_observation")
    else()
        find_package(vnm_filesystem_observation CONFIG QUIET)
        if(NOT TARGET vnm_framework::vnm_filesystem_observation)
            include(FetchContent)
            FetchContent_Declare(vnm_filesystem_observation
                GIT_REPOSITORY https://github.com/Varinomics/vnm_filesystem_observation.git
                GIT_TAG master
                GIT_SHALLOW TRUE)
            FetchContent_MakeAvailable(vnm_filesystem_observation)
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
    cmake_path(GET grok_observation_source PARENT_PATH grok_observation_source)
endif()
get_target_property(grok_dispatch_imported vnm_qt_dispatch::vnm_qt_dispatch IMPORTED)
if(NOT grok_dispatch_imported)
    get_target_property(grok_dispatch_source vnm_qt_dispatch::vnm_qt_dispatch SOURCE_DIR)
endif()
include(FetchContent)
FetchContent_GetProperties(vnm_cmake SOURCE_DIR grok_vnm_cmake_source)
