# ============================================================================
# Vehicle Module
# Core vehicle state management and MAVLink communication
# ============================================================================

target_sources(
    ${CMAKE_PROJECT_NAME}
    PRIVATE Autotune.cpp
            Autotune.h
            FTPController.cc
            FTPController.h
            FTPManager.cc
            FTPManager.h
            InitialConnectStateMachine.cc
            InitialConnectStateMachine.h
            MavCommandQueue.cc
            MavCommandQueue.h
            MAVLinkLogManager.cc
            MAVLinkLogManager.h
            MessageIntervalManager.cc
            MessageIntervalManager.h
            MultiVehicleManager.cc
            MultiVehicleManager.h
            RemoteIDManager.cc
            RemoteIDManager.h
            RequestMessageCoordinator.cc
            RequestMessageCoordinator.h
            StandardModes.cc
            StandardModes.h
            TerrainProtocolHandler.cc
            TerrainProtocolHandler.h
            TerrainQueryCoordinator.cc
            TerrainQueryCoordinator.h
            TrajectoryPoints.cc
            TrajectoryPoints.h
            Vehicle.cc
            Vehicle.h
            VehicleLinkManager.cc
            VehicleLinkManager.h
            VehicleObjectAvoidance.cc
            VehicleObjectAvoidance.h
            VehicleReadiness.cc
            VehicleReadiness.h
            VehicleSigningController.cc
            VehicleSigningController.h
            VehicleSupports.cc
            VehicleSupports.h
            VehicleTypes.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(Actuators)
add_subdirectory(ComponentInformation)
add_subdirectory(FactGroups)
add_subdirectory(VehicleSetup)

option(QGC_UOM_BENCH_TEST "Enable localhost-only MOCK UOM provider for bench testing" OFF)
add_subdirectory(UOM)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE QGCUomCore)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE UOM/VehicleUomManager.cc UOM/VehicleUomManager.h)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/UOM)
if(QGC_UOM_BENCH_TEST)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE QGC_UOM_BENCH_TEST)
endif()

add_subdirectory(RID)

add_subdirectory(UOM/Registration)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE QGCUomRegistrationCore)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE UOM/Registration/VehicleUomRegistrationManager.cc
                                             UOM/Registration/VehicleUomRegistrationManager.h
)

add_subdirectory(UOM/Reporting)
target_sources(QGCUomReportingCore PRIVATE UOM/Reporting/UomReportStore.cc UOM/Reporting/UomReportStore.h)
target_link_libraries(QGCUomReportingCore PUBLIC Qt6::Sql)
add_library(QGCUomReportingService STATIC UOM/Reporting/UomReportingService.cc UOM/Reporting/UomReportingService.h)
set_target_properties(QGCUomReportingService PROPERTIES AUTOMOC ON)
target_link_libraries(QGCUomReportingService PUBLIC QGCUomReportingCore QGCUomRegistrationCore)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE QGCUomReportingService)
