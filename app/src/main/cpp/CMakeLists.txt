cmake_minimum_required(VERSION 3.22.1)

project(rlvm_android CXX)

# task.md：C++17 是硬性要求（libreallive / machine 均依赖）。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 目前只编译 JNI 桥接层。RLVM 上游源码的接入方式见 docs/DECISIONS.md D-006（T1.1 落地）。
add_library(rlvm SHARED
    native-bridge.cpp
)

target_compile_options(rlvm PRIVATE
    -Wall
    -Wextra
    -fvisibility=hidden
)

find_library(log_lib log)
find_library(android_lib android)

target_link_libraries(rlvm
    ${log_lib}
    ${android_lib}
)
