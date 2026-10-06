cmake_minimum_required(VERSION 3.18)
project(cv710userspace)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")



add_subdirectory(src/liblgx)
add_subdirectory(src/cli)

include(version.cmake)

install(TARGETS cv710userspace DESTINATION bin)
install(DIRECTORY assets/ DESTINATION share/cv710userspace/assets)
