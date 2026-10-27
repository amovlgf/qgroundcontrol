target_sources(${CMAKE_PROJECT_NAME} PRIVATE VehicleRidManagerTest.cc VehicleRidManagerTest.h)
add_qgc_test(VehicleRidManagerTest LABELS Integration Vehicle TIMEOUT 120)
