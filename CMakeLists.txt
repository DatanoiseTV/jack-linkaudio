cmake_minimum_required(VERSION 3.10)
project(jack-linkaudio)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Ableton Link
include(extern/link/AbletonLinkConfig.cmake)
add_subdirectory(extern/link/extensions/abl_link)

# JACK
find_package(PkgConfig REQUIRED)
pkg_check_modules(JACK REQUIRED jack)

add_executable(jack-linkaudio
    src/main.cpp
    src/Bridge.cpp
)

target_include_directories(jack-linkaudio PRIVATE
    src
    extern/link/include
    extern/link/examples/linkaudio
)
target_compile_options(jack-linkaudio PRIVATE ${JACK_CFLAGS})
target_link_libraries(jack-linkaudio PRIVATE
    abl_link
    ${JACK_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)
target_link_options(jack-linkaudio PRIVATE ${JACK_LDFLAGS})

target_compile_definitions(jack-linkaudio PRIVATE
    LINK_AUDIO=1
)
