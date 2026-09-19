if(NOT DEFINED HELPER OR NOT DEFINED VNMCTL OR NOT DEFINED WORK_DIRECTORY)
    message(FATAL_ERROR "Missing executable fixture paths")
endif()
file(MAKE_DIRECTORY "${WORK_DIRECTORY}")
set(input "${WORK_DIRECTORY}/pretool.json")
file(WRITE "${input}" "{\"hookEventName\":\"pre_tool_use\",\"hook_event_name\":\"PreToolUse\",\"sessionId\":\"root-session\"}")
get_filename_component(helpers "${HELPER}" DIRECTORY)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "LOGONOMIC_VNMCTL_PATH=${VNMCTL}"
        "LOGONOMIC_SESSION_CAPABILITY=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "LOGONOMIC_PROVIDER_PACKAGE_ID=logonomic.cli.grok"
        "LOGONOMIC_PROVIDER_GENERATION_DIGEST=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
        "LOGONOMIC_PROVIDER_GENERATION_NONCE=cccccccccccccccccccccccccccccccc"
        "LOGONOMIC_PROVIDER_HELPERS_DIR=${helpers}"
        "${HELPER}"
    INPUT_FILE "${input}" OUTPUT_VARIABLE output ERROR_VARIABLE errors RESULT_VARIABLE status TIMEOUT 10)
# Grok's native command gate discards stdout on nonzero exit. Checking the
# parsed text alone would therefore miss a broken model-context delivery.
if(NOT status STREQUAL "0")
    message(FATAL_ERROR "Native gate would discard accepted context: ${status}: ${errors}")
endif()
string(JSON instruction ERROR_VARIABLE parse_error GET "${output}" hookSpecificOutput additionalContext)
if(parse_error OR NOT instruction STREQUAL "Set the title using the authenticated host instruction.")
    message(FATAL_ERROR "Authenticated model context was not preserved")
endif()

file(WRITE "${input}" "{\"hookEventName\":\"pre_tool_use\",\"hook_event_name\":\"PreToolUse\",\"sessionId\":\"child-session\",\"subagentType\":\"researcher\"}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "LOGONOMIC_VNMCTL_PATH=${VNMCTL}"
        "LOGONOMIC_SESSION_CAPABILITY=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "LOGONOMIC_PROVIDER_PACKAGE_ID=logonomic.cli.grok"
        "LOGONOMIC_PROVIDER_GENERATION_DIGEST=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
        "LOGONOMIC_PROVIDER_GENERATION_NONCE=cccccccccccccccccccccccccccccccc"
        "LOGONOMIC_PROVIDER_HELPERS_DIR=${helpers}"
        "${HELPER}"
    INPUT_FILE "${input}" OUTPUT_VARIABLE output ERROR_VARIABLE errors RESULT_VARIABLE status TIMEOUT 10)
if(NOT status STREQUAL "0" OR NOT output STREQUAL "")
    message(FATAL_ERROR "A child tool hook requested root title context")
endif()
