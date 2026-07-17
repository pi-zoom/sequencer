cmake_minimum_required(VERSION 3.22)

project(pi-zoom-seq VERSION 1.0)

# C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


# Dependencies
find_package(PkgConfig REQUIRED)

pkg_check_modules(JACK REQUIRED jack)
pkg_check_modules(LIBLO REQUIRED liblo)
pkg_check_modules(FLUIDSYNTH REQUIRED fluidsynth)

file(GLOB_RECURSE SOURCES src/*.c src/*.cpp)
file(GLOB_RECURSE HEADERS src/*.h src/*.hpp)

set(LIBS
        jack
        lo
        fluidsynth
)

add_executable(${PROJECT_NAME} ${SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE include/midifile)
target_link_libraries(${PROJECT_NAME} ${LIBS})
install(TARGETS ${PROJECT_NAME})
