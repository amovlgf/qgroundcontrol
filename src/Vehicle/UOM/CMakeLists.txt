if(TARGET QGCUomCore)
    return()
endif()

add_library(QGCUomCore STATIC UomTypes.cc UomProvider.cc MockUomProvider.cc UomSession.cc)
set_target_properties(QGCUomCore PROPERTIES AUTOMOC ON)
target_compile_features(QGCUomCore PUBLIC cxx_std_20)
target_link_libraries(QGCUomCore PUBLIC Qt6::Core Qt6::Network QGCTiming)
target_sources(
    QGCUomCore
    PUBLIC FILE_SET
           HEADERS
           FILES
           UomTypes.h
           UomProvider.h
           MockUomProvider.h
           UomSession.h
)
