cmake_minimum_required(VERSION 3.20)
project(gpu_switcher VERSION 2.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)

add_library(gpu_switcher_common
    src/common.cpp
)
target_include_directories(gpu_switcher_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(gpu_switcher_common PUBLIC Qt6::Core Qt6::Network)

add_executable(gpu-switcher-helperd
    src/helperd_main.cpp
)
target_link_libraries(gpu-switcher-helperd PRIVATE gpu_switcher_common Qt6::Core Qt6::Network)

add_executable(gpu-switcher-gui
    src/gui_main.cpp
)
target_link_libraries(gpu-switcher-gui PRIVATE gpu_switcher_common Qt6::Core Qt6::Widgets Qt6::Network)

add_executable(gpu-switcher-ctl
    src/ctl_main.cpp
)
target_link_libraries(gpu-switcher-ctl PRIVATE gpu_switcher_common Qt6::Core Qt6::Network)
