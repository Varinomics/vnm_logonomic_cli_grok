# Runs the packaged quota helper as the host does: GROK_HOME names the selected
# profile, and one quota@2 result line is the whole of stdout. Each case stops
# before any request, so the run never reaches Grok's service.
if(NOT DEFINED BRIDGE OR NOT DEFINED WORK_DIRECTORY)
    message(FATAL_ERROR "Missing quota helper fixture paths")
endif()
file(REMOVE_RECURSE "${WORK_DIRECTORY}")
set(profile "${WORK_DIRECTORY}/profile")
file(MAKE_DIRECTORY "${profile}")

# The ambient environment of a developer machine may route Grok elsewhere.
set(clean_environment)
foreach(name IN ITEMS GROK_HOME GROK_AUTH GROK_AUTH_PATH GROK_CLI_CHAT_PROXY_BASE_URL
        GROK_PRODUCTION_CLI_CHAT_PROXY_BASE_URL GROK_CONFIG GROK_CONFIG_PATH GROK_OIDC_ISSUER
        GROK_OIDC_CLIENT_ID GROK_OAUTH2_ISSUER GROK_OAUTH2_CLIENT_ID GROK_LOCAL_AUTH
        GROK_FORCE_LOGIN_TEAM_ID GROK_AUTH_PROVIDER_COMMAND GROK_DEPLOYMENT_KEY GROK_MODELS_BASE_URL
        XAI_API_KEY)
    list(APPEND clean_environment "--unset=${name}")
endforeach()

function(run_bridge out_status out_output)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env ${clean_environment} ${ARGN} "${BRIDGE}"
        OUTPUT_VARIABLE output ERROR_VARIABLE errors RESULT_VARIABLE status TIMEOUT 30)
    set(${out_status} "${status}" PARENT_SCOPE)
    set(${out_output} "${output}" PARENT_SCOPE)
endfunction()

function(expect_state expected description)
    run_bridge(status output ${ARGN})
    if(NOT status STREQUAL "0")
        message(FATAL_ERROR "${description}: the helper exited with ${status}")
    endif()
    string(JSON complete ERROR_VARIABLE complete_error GET "${output}" complete)
    string(JSON state ERROR_VARIABLE state_error GET "${output}" observation state)
    string(JSON primary ERROR_VARIABLE primary_error GET "${output}" observation primary)
    # string(JSON) reports a present member with the error value NOTFOUND.
    if(complete_error OR state_error OR NOT complete STREQUAL "ON" OR NOT state STREQUAL "${expected}" OR
        primary_error STREQUAL "NOTFOUND")
        message(FATAL_ERROR "${description}: expected a complete ${expected} observation without "
            "figures, got: ${output}")
    endif()
endfunction()

run_bridge(status output)
if(status STREQUAL "0" OR NOT output STREQUAL "")
    message(FATAL_ERROR "Without a selected profile the helper must fail without a result")
endif()

expect_state(not_logged_in "A profile without a login" "GROK_HOME=${profile}")
expect_state(unavailable "A profile path that is not absolute" "GROK_HOME=relative/profile")
expect_state(unavailable "A session credential from the environment" "GROK_HOME=${profile}"
    "GROK_AUTH={\"key\":\"inline\"}")

file(WRITE "${profile}/auth.json"
    "{\"https://auth.x.ai::b1a00492-073a-47ea-816f-4c329264a828\":{\"key\":\"fixture-access-token\",\
\"auth_mode\":\"oidc\",\"create_time\":\"2020-01-01T00:00:00Z\",\"user_id\":\"fixture-user\",\
\"principal_type\":\"User\",\"principal_id\":\"fixture-principal\",\"refresh_token\":\"fixture-refresh-token\",\
\"expires_at\":\"2020-01-01T01:00:00Z\",\"oidc_issuer\":\"https://auth.x.ai\",\
\"oidc_client_id\":\"b1a00492-073a-47ea-816f-4c329264a828\"}}")
expect_state(error "An expired login" "GROK_HOME=${profile}")
file(REMOVE_RECURSE "${WORK_DIRECTORY}")
