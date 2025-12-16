cmake_minimum_required(VERSION 3.16)
project(reed-tpse VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(reed STATIC
    src/protocol.cpp
    src/device.cpp
    src/adb.cpp
    src/media.cpp
    src/config.cpp
)

target_include_directories(reed PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_options(reed PRIVATE
    -Wall -Wextra -Wpedantic
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:Debug>:-g -O0>
)

add_executable(reed-tpse cli/main.cpp)
target_link_libraries(reed-tpse PRIVATE reed)

target_compile_options(reed-tpse PRIVATE
    -Wall -Wextra -Wpedantic
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:Debug>:-g -O0>
)

install(TARGETS reed-tpse RUNTIME DESTINATION bin)
install(TARGETS reed
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
install(DIRECTORY include/reed DESTINATION include)
install(FILES systemd/reed-tpse.service DESTINATION lib/systemd/user)
