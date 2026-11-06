include_guard(GLOBAL)

# Emits non-sensitive manufacturer settings; credentials are resolved from Secret Service at runtime.
function(qgc_generate_b950_profile profile_file output_file)
    if(NOT ARGC EQUAL 2
       OR "${profile_file}" STREQUAL ""
       OR "${output_file}" STREQUAL ""
    )
        message(FATAL_ERROR "qgc_generate_b950_profile: profile and output paths are required")
    endif()
    if(NOT EXISTS "${profile_file}" OR IS_DIRECTORY "${profile_file}")
        message(FATAL_ERROR "B950 customer builds require a manufacturer profile file")
    endif()

    file(READ "${profile_file}" _profile)
    string(JSON _type ERROR_VARIABLE _error TYPE "${_profile}")
    if(_error OR NOT _type STREQUAL "OBJECT")
        message(FATAL_ERROR "B950 manufacturer profile must be a JSON object")
    endif()
    string(JSON _count LENGTH "${_profile}")
    if(NOT _count EQUAL 6)
        message(FATAL_ERROR "B950 manufacturer profile requires exactly six configuration fields")
    endif()

    foreach(_field IN ITEMS environment producerId credentialReference verifyUrl reportUrl cancelUrl)
        string(
            JSON
            _type
            ERROR_VARIABLE
            _error
            TYPE
            "${_profile}"
            "${_field}"
        )
        if(_error OR NOT _type STREQUAL "STRING")
            message(FATAL_ERROR "B950 manufacturer profile requires string field ${_field}")
        endif()
        string(JSON _value GET "${_profile}" "${_field}")
        set(_profile_${_field} "${_value}")
    endforeach()

    if(NOT _profile_environment STREQUAL "official")
        message(FATAL_ERROR "B950 manufacturer profile requires the official environment")
    endif()
    string(REGEX REPLACE "[0-9a-fA-F]" "0" _producer_shape "${_profile_producerId}")
    if(NOT _producer_shape STREQUAL "00000000-0000-0000-0000-000000000000")
        message(FATAL_ERROR "B950 manufacturer profile has an invalid producerId")
    endif()
    string(LENGTH "${_profile_credentialReference}" _length)
    if(_length GREATER 160 OR NOT _profile_credentialReference MATCHES "^uom-registration/key/[a-zA-Z0-9_.-]+$")
        message(FATAL_ERROR "B950 manufacturer profile has an invalid credentialReference")
    endif()
    foreach(_field IN ITEMS verifyUrl reportUrl cancelUrl)
        if(NOT "${_profile_${_field}}" MATCHES "^https://uom[.]caac[.]gov[.]cn(:443)?/[a-zA-Z0-9_./%-]+$")
            message(FATAL_ERROR "B950 manufacturer profile has an invalid ${_field}")
        endif()
    endforeach()

    # Never pass credentials as compiler definitions or include them in this generated header.
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/B950ProfileData.h.in" "${output_file}" @ONLY FILE_PERMISSIONS
                   OWNER_READ OWNER_WRITE
    )
endfunction()
