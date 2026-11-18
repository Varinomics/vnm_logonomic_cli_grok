if(NOT DEFINED HELPER OR NOT DEFINED CONFIGURATION OR NOT DEFINED VNMCTL OR NOT DEFINED WORK_DIRECTORY)
    message(FATAL_ERROR "Missing executable fixture paths")
endif()
file(MAKE_DIRECTORY "${WORK_DIRECTORY}")
get_filename_component(vnmctl_name "${VNMCTL}" NAME)
set(host_directory "${WORK_DIRECTORY}/host executable with spaces")
file(MAKE_DIRECTORY "${host_directory}")
file(COPY_FILE "${VNMCTL}" "${host_directory}/${vnmctl_name}" ONLY_IF_DIFFERENT)
set(VNMCTL "${host_directory}/${vnmctl_name}")
set(input "${WORK_DIRECTORY}/pretool.json")
file(WRITE "${input}" "{\"hookEventName\":\"pre_tool_use\",\"hook_event_name\":\"PreToolUse\",\"sessionId\":\"root-session\"}")
get_filename_component(program_directory "${HELPER}" DIRECTORY)
get_filename_component(package "${program_directory}" DIRECTORY)
set(helpers "${package}/helpers")
execute_process(COMMAND "${CONFIGURATION}" --print-hook-configuration
    OUTPUT_VARIABLE configuration COMMAND_ERROR_IS_FATAL ANY)
string(JSON hook GET "${configuration}" hooks PreToolUse 0 hooks 0 command)
if(WIN32)
    set(hook_shell cmd)
    set(hook_arguments /d /s /c)
else()
    set(hook_shell sh)
    set(hook_arguments -c)
endif()
set(receipts "${WORK_DIRECTORY}/helper-receipts.jsonl")
file(WRITE "${receipts}" "")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "LOGONOMIC_VNMCTL_PATH=${VNMCTL}"
        "LOGONOMIC_SESSION_CAPABILITY=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "LOGONOMIC_PROVIDER_PACKAGE_ID=logonomic.cli.grok"
        "LOGONOMIC_PROVIDER_GENERATION_DIGEST=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
        "LOGONOMIC_PROVIDER_GENERATION_NONCE=cccccccccccccccccccccccccccccccc"
        "LOGONOMIC_PROVIDER_HELPERS_DIR=${helpers}"
        "GROK_HELPER_TEST_RECEIPTS=${receipts}"
        "${hook_shell}" ${hook_arguments} "${hook}"
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
        "GROK_HELPER_TEST_RECEIPTS=${receipts}"
        "${HELPER}"
    INPUT_FILE "${input}" OUTPUT_VARIABLE output ERROR_VARIABLE errors RESULT_VARIABLE status TIMEOUT 10)
if(NOT status STREQUAL "0" OR NOT output STREQUAL "")
    message(FATAL_ERROR "A child tool hook requested root title context")
endif()

# Native camelCase names and PascalCase aliases must reach the classifier with
# the original timestamp and transcript pointer; delivery identities are separate.
set(transcript "${WORK_DIRECTORY}/native-session/updates.jsonl")
file(MAKE_DIRECTORY "${WORK_DIRECTORY}/native-session")
file(WRITE "${transcript}"
    "{\"timestamp\":1792923070,\"method\":\"session/update\",\"params\":{\"sessionId\":\"native-session\",\"_meta\":{\"eventId\":\"native-event-1\",\"totalTokens\":7000},\"update\":{\"sessionUpdate\":\"agent_message_chunk\"}}}\n")
foreach(event IN ITEMS "post_compact;PostCompact" "post_tool_use;PostToolUse" "user_prompt_submit;UserPromptSubmit")
    list(GET event 0 wire_name)
    list(GET event 1 alias_name)
    file(WRITE "${input}"
        "{\"hookEventName\":\"${wire_name}\",\"hook_event_name\":\"${alias_name}\",\"sessionId\":\"native-session\",\"source\":\"manual\",\"timestamp\":\"2026-10-25T12:11:10.123456700+00:00\",\"transcriptPath\":\"${transcript}\"}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "LOGONOMIC_VNMCTL_PATH=${VNMCTL}"
            "LOGONOMIC_SESSION_CAPABILITY=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "LOGONOMIC_PROVIDER_PACKAGE_ID=logonomic.cli.grok"
            "LOGONOMIC_PROVIDER_GENERATION_DIGEST=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
            "LOGONOMIC_PROVIDER_GENERATION_NONCE=cccccccccccccccccccccccccccccccc"
            "LOGONOMIC_PROVIDER_HELPERS_DIR=${helpers}"
            "GROK_HELPER_TEST_RECEIPTS=${receipts}"
            "${HELPER}"
        INPUT_FILE "${input}" OUTPUT_VARIABLE output ERROR_VARIABLE errors RESULT_VARIABLE status TIMEOUT 10)
    if(NOT status STREQUAL "0" OR NOT output STREQUAL "")
        message(FATAL_ERROR "Native ${alias_name} context evidence did not reach its classifier: ${status}: ${errors}")
    endif()
endforeach()

file(WRITE "${input}"
    "{\"hookEventName\":\"session_start\",\"hook_event_name\":\"SessionStart\",\"sessionId\":\"native-session\",\"source\":\"load\",\"cwd\":\"${WORK_DIRECTORY}/native-session/nested folder\",\"workspaceRoot\":\"${WORK_DIRECTORY}\",\"transcriptPath\":\"${transcript}\"}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "LOGONOMIC_VNMCTL_PATH=${VNMCTL}"
        "LOGONOMIC_SESSION_CAPABILITY=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "LOGONOMIC_PROVIDER_PACKAGE_ID=logonomic.cli.grok"
        "LOGONOMIC_PROVIDER_GENERATION_DIGEST=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
        "LOGONOMIC_PROVIDER_GENERATION_NONCE=cccccccccccccccccccccccccccccccc"
        "LOGONOMIC_PROVIDER_HELPERS_DIR=${helpers}"
        "GROK_HELPER_TEST_RECEIPTS=${receipts}"
        "${HELPER}"
    INPUT_FILE "${input}" OUTPUT_VARIABLE output ERROR_VARIABLE errors RESULT_VARIABLE status TIMEOUT 10)
if(NOT status STREQUAL "0" OR NOT output STREQUAL "")
    message(FATAL_ERROR "Native SessionStart lost its exact default directory: ${status}: ${errors}")
endif()

# Validate the actual package once, then enforce every original classifier
# assertion on the exact deliveries received within the native helper budget.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "LOGONOMIC_PROVIDER_HELPERS_DIR=${helpers}"
        "${VNMCTL}" --verify-deliveries "${receipts}"
    OUTPUT_VARIABLE output ERROR_VARIABLE errors RESULT_VARIABLE status TIMEOUT 60)
if(NOT status STREQUAL "0")
    message(FATAL_ERROR "Native helper evidence did not survive packaged classification: ${status}: ${errors}")
endif()
