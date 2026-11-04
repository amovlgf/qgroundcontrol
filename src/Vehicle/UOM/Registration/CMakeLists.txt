if(TARGET QGCUomRegistrationCore)
    return()
endif()

find_package(OpenSSL 1.1.1 REQUIRED COMPONENTS Crypto)

option(QGC_B950_CUSTOMER_BUILD "Use manufacturer-managed B950 UOM configuration" OFF)
set(QGC_B950_PROFILE_FILE
    ""
    CACHE FILEPATH "Private B950 manufacturer profile JSON file"
)
set(_qgc_b950_profile_header "${CMAKE_CURRENT_BINARY_DIR}/UomRegistrationProfileData.h")
if(QGC_B950_CUSTOMER_BUILD)
    include("${CMAKE_CURRENT_SOURCE_DIR}/../../../../cmake/modules/B950Profile.cmake")
    qgc_generate_b950_profile("${QGC_B950_PROFILE_FILE}" "${_qgc_b950_profile_header}")
    set_property(
        DIRECTORY
        APPEND
        PROPERTY CMAKE_CONFIGURE_DEPENDS "${QGC_B950_PROFILE_FILE}"
    )
else()
    file(REMOVE "${_qgc_b950_profile_header}")
endif()

add_library(
    QGCUomRegistrationCore STATIC
    UomRegistrationTypes.cc
    UomRegistrationCrypto.cc
    UomRegistrationStore.cc
    UomRegistrationClient.cc
    UomRegistrationCredentials.cc
    UomRegistrationWorkflow.cc
    UomRegistrationProfile.cc
)
set_target_properties(QGCUomRegistrationCore PROPERTIES AUTOMOC ON)
target_compile_features(QGCUomRegistrationCore PUBLIC cxx_std_20)
target_include_directories(QGCUomRegistrationCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(QGC_B950_CUSTOMER_BUILD)
    set_source_files_properties(UomRegistrationProfile.cc PROPERTIES COMPILE_DEFINITIONS QGC_B950_CUSTOMER_BUILD
                                                                     INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}"
    )
endif()
target_link_libraries(
    QGCUomRegistrationCore
    PUBLIC Qt6::Core Qt6::Network Qt6::Sql
    PRIVATE Qt6::Concurrent OpenSSL::Crypto
)
target_sources(
    QGCUomRegistrationCore
    PUBLIC FILE_SET
           HEADERS
           FILES
           UomRegistrationTypes.h
           UomRegistrationCrypto.h
           UomRegistrationStore.h
           UomRegistrationClient.h
           UomRegistrationCredentials.h
           UomRegistrationWorkflow.h
           UomRegistrationProfile.h
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(UOM_LIBSECRET QUIET IMPORTED_TARGET libsecret-1)
        if(TARGET PkgConfig::UOM_LIBSECRET)
            target_link_libraries(QGCUomRegistrationCore PRIVATE PkgConfig::UOM_LIBSECRET)
            target_compile_definitions(QGCUomRegistrationCore PRIVATE QGC_UOM_LIBSECRET)
        endif()
    endif()
endif()
