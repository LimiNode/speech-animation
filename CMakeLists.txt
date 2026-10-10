cmake_minimum_required(VERSION 3.16)
project(speech_animation VERSION 0.1.0 LANGUAGES CXX)

find_package(Threads REQUIRED)

option(SPEECH_ANIMATION_BUILD_TESTS "Build speech-animation tests" ON)
option(SPEECH_ANIMATION_BUILD_BENCHMARKS "Build speech-animation benchmarks" OFF)

add_library(speech_animation
    core/src/analyzer.cpp
    core/src/predictive.cpp
)
add_library(speech_animation::speech_animation ALIAS speech_animation)

add_library(speech_animation_integration
    integration/src/integration.cpp
)
add_library(speech_animation::integration ALIAS speech_animation_integration)

target_compile_features(speech_animation PUBLIC cxx_std_17)
target_include_directories(speech_animation
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(speech_animation_integration PUBLIC cxx_std_17)
target_link_libraries(speech_animation_integration PUBLIC
    speech_animation::speech_animation Threads::Threads)
target_include_directories(speech_animation_integration
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/integration/include>
        $<INSTALL_INTERFACE:include>
)

if(MSVC)
    target_compile_options(speech_animation PRIVATE /W4 /permissive-)
    target_compile_options(speech_animation_integration PRIVATE /W4 /permissive-)
else()
    target_compile_options(speech_animation PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(speech_animation_integration PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(SPEECH_ANIMATION_BUILD_TESTS)
    enable_testing()
    add_executable(speech_animation_tests tests/analyzer_tests.cpp)
    target_link_libraries(speech_animation_tests PRIVATE speech_animation::speech_animation)
    add_test(NAME speech_animation_tests COMMAND speech_animation_tests)

    add_executable(speech_animation_predictive_tests tests/predictive_tests.cpp)
    target_link_libraries(speech_animation_predictive_tests PRIVATE speech_animation::speech_animation)
    add_test(NAME speech_animation_predictive_tests COMMAND speech_animation_predictive_tests)

    add_executable(speech_animation_integration_tests tests/integration_tests.cpp)
    target_link_libraries(speech_animation_integration_tests PRIVATE
        speech_animation::integration Threads::Threads)
    add_test(NAME speech_animation_integration_tests COMMAND speech_animation_integration_tests)
endif()

if(SPEECH_ANIMATION_BUILD_BENCHMARKS)
    add_executable(speech_animation_benchmark benchmarks/analyzer_benchmark.cpp)
    target_link_libraries(speech_animation_benchmark PRIVATE speech_animation::speech_animation)
endif()

include(GNUInstallDirs)
install(TARGETS speech_animation speech_animation_integration EXPORT speech_animation_targets)
install(DIRECTORY core/include/ integration/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT speech_animation_targets
    FILE speech_animation_targets.cmake
    NAMESPACE speech_animation::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/speech_animation
)
