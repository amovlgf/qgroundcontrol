cmake_minimum_required(VERSION 3.25)
include("${CMAKE_CURRENT_LIST_DIR}/../modules/B950Profile.cmake")

if(NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "WORK_DIR is required")
endif()

if(DEFINED TEST_CASE)
    set(_profile
        [=[{
        "environment": "official",
        "producerId": "00000000-0000-0000-0000-000000000001",
        "keyId": "00112233-4455-6677-8899-aabbccddeeff",
        "verifyUrl": "https://uom.caac.gov.cn/api/verify",
        "reportUrl": "https://uom.caac.gov.cn/api/report",
        "cancelUrl": "https://uom.caac.gov.cn/api/cancel"
    }]=]
    )
    if(TEST_CASE STREQUAL "malformed")
        set(_profile "{invalid-secret-marker")
    elseif(TEST_CASE STREQUAL "unknown-field")
        string(JSON _profile SET "${_profile}" unexpected "true")
    elseif(TEST_CASE STREQUAL "missing-field")
        string(JSON _profile REMOVE "${_profile}" keyId)
    elseif(TEST_CASE STREQUAL "environment")
        string(JSON _profile SET "${_profile}" environment "\"integration\"")
    elseif(TEST_CASE STREQUAL "producer")
        string(JSON _profile SET "${_profile}" producerId "\"invalid-secret-marker\"")
    elseif(TEST_CASE STREQUAL "key")
        string(JSON _profile SET "${_profile}" keyId "\"invalid-secret-marker\"")
    elseif(TEST_CASE STREQUAL "url")
        string(JSON _profile SET "${_profile}" verifyUrl "\"https://uom.caac.gov.cn.invalid/api/verify\"")
    elseif(TEST_CASE STREQUAL "url-query")
        string(JSON _profile SET "${_profile}" reportUrl "\"https://uom.caac.gov.cn/api/report?secret=1\"")
    elseif(TEST_CASE STREQUAL "wrong-type")
        string(JSON _profile SET "${_profile}" cancelUrl "42")
    endif()

    file(MAKE_DIRECTORY "${WORK_DIR}")
    if(NOT TEST_CASE STREQUAL "missing-file")
        file(WRITE "${WORK_DIR}/profile.json" "${_profile}")
    endif()
    qgc_generate_b950_profile("${WORK_DIR}/profile.json" "${WORK_DIR}/profile.h")
    file(READ "${WORK_DIR}/profile.h" _generated)
    if(NOT _generated MATCHES "KEY_ID\\[\\] = \"00112233445566778899aabbccddeeff\""
       OR NOT _generated MATCHES "https://uom.caac.gov.cn/api/verify"
    )
        message(FATAL_ERROR "Generated profile does not contain the validated configuration")
    endif()
    return()
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
foreach(
    _case IN
    ITEMS valid
          missing-file
          malformed
          unknown-field
          missing-field
          environment
          producer
          key
          url
          url-query
          wrong-type
)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DWORK_DIR=${WORK_DIR}/${_case}" "-DTEST_CASE=${_case}" -P
                "${CMAKE_CURRENT_LIST_FILE}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr
    )
    if(_case STREQUAL "valid")
        if(NOT _result EQUAL 0)
            message(FATAL_ERROR "Valid manufacturer profile was rejected: ${_stderr}")
        endif()
    elseif(_result EQUAL 0 OR NOT _stderr MATCHES "B950")
        message(FATAL_ERROR "Invalid manufacturer profile was not rejected: ${_case}")
    endif()
    if("${_stdout}${_stderr}" MATCHES "invalid-secret-marker|00112233|4455-6677")
        message(FATAL_ERROR "Manufacturer profile diagnostics exposed configuration contents")
    endif()
endforeach()
message(STATUS "B950 manufacturer profile validation passed")
