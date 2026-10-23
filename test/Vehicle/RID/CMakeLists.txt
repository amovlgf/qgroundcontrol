cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(QGCRidTests LANGUAGES CXX)
    enable_testing()
    find_package(Qt6 6.8 REQUIRED COMPONENTS Core Network Test)
endif()

add_executable(QGCRidProviderTest
    RidActivationProviderTest.cc
    ../../../src/Vehicle/RID/RidActivationProvider.cc
    ../../../src/Vehicle/RID/RidActivationProvider.h
)
set_target_properties(QGCRidProviderTest PROPERTIES AUTOMOC ON)
target_compile_features(QGCRidProviderTest PRIVATE cxx_std_20)
target_include_directories(QGCRidProviderTest PRIVATE ../../../src/Vehicle/RID)
target_link_libraries(QGCRidProviderTest PRIVATE Qt6::Core Qt6::Network Qt6::Test)
add_test(NAME RidActivationProviderTest COMMAND QGCRidProviderTest)
set_tests_properties(RidActivationProviderTest PROPERTIES LABELS "Unit;Vehicle" TIMEOUT 30)

if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE VehicleRidManagerTest.cc VehicleRidManagerTest.h)
    add_qgc_test(VehicleRidManagerTest LABELS Integration Vehicle TIMEOUT 120)
endif()
