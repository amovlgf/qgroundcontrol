if(TARGET QGCUomReportingCore)
    return()
endif()

find_package(OpenSSL 1.1.1 REQUIRED COMPONENTS Crypto)
add_library(QGCUomReportingCore STATIC UomReportProtocol.cc UomReportClient.cc)
set_target_properties(QGCUomReportingCore PROPERTIES AUTOMOC ON)
target_compile_features(QGCUomReportingCore PUBLIC cxx_std_20)
target_include_directories(QGCUomReportingCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(
    QGCUomReportingCore
    PUBLIC Qt6::Core Qt6::Network
    PRIVATE OpenSSL::Crypto
)
target_sources(QGCUomReportingCore PUBLIC FILE_SET HEADERS FILES UomReportProtocol.h UomReportClient.h)
