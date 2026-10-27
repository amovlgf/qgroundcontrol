target_sources(${CMAKE_PROJECT_NAME} PRIVATE RidParameterManager.cc RidParameterManager.h RidTransportClient.cc
                                             RidTransportClient.h VehicleRidManager.cc VehicleRidManager.h
)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                                         ${CMAKE_SOURCE_DIR}/protocol/amov_device_bridge
)
