cmake_minimum_required(VERSION 3.5)

# 1. 项目基础配置（仅保留工程名）
project(libdatachannel_test_3_18 VERSION 0.1 LANGUAGES CXX)

# ===================== 源文件配置 =====================
# 仅保留你实际用到的源文件（删除注释、UWP资源等冗余内容）
set(PROJECT_SOURCES
    main.cpp
    utilities/log.h
    utilities/blocking_queue.h
    DataChannelClient.cpp
    DataChannelClient.h
    dispatchqueue.cpp
    dispatchqueue.hpp
    h264fileparser.cpp
    h264fileparser.hpp
    helpers.cpp
    helpers.hpp
    opusfileparser.cpp
    opusfileparser.hpp
    fileparser.cpp
    fileparser.hpp
    stream.cpp
    stream.hpp
    ArgParser.cpp
    ArgParser.hpp
)

# ===================== 可执行文件 =====================
add_executable(${PROJECT_NAME} ${PROJECT_SOURCES})

# ===================== 【关键】直接链接根目录导入的全局目标 =====================
target_link_libraries(${PROJECT_NAME} PRIVATE
    datachannel          # 根目录third_party/libdatachannel导入的目标
    spdlog::spdlog       # 根目录third_party/spdlog导入的目标
    Threads::Threads     # 线程库（根目录或这里find_package都可以，这里保留更稳妥）
)