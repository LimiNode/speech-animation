cmake_minimum_required(VERSION 3.16)
project(speech_animation VERSION 0.1.0 LANGUAGES CXX)

option(SPEECH_ANIMATION_BUILD_TESTS "Build speech-animation tests" ON)

add_library(speech_animation
    core/src/analyzer.cpp
)
add_library(speech_animation::speech_animation ALIAS speech_animation)

target_compile_features(speech_animation PUBLIC cxx_std_17)
target_include_directories(speech_animation
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include>
        $<INSTALL_INTERFACE:include>
)

if(MSVC)
    target_compile_options(speech_animation PRIVATE /W4 /permissive-)
else()
    target_compile_options(speech_animation PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(SPEECH_ANIMATION_BUILD_TESTS)
    enable_testing()
    add_executable(speech_animation_tests tests/analyzer_tests.cpp)
    target_link_libraries(speech_animation_tests PRIVATE speech_animation::speech_animation)
    add_test(NAME speech_animation_tests COMMAND speech_animation_tests)
endif()

include(GNUInstallDirs)
install(TARGETS speech_animation EXPORT speech_animation_targets)
install(DIRECTORY core/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT speech_animation_targets
    FILE speech_animation_targets.cmake
    NAMESPACE speech_animation::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/speech_animation
)
