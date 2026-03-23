cmake_minimum_required(VERSION 3.5)

# 1. 项目基础配置（仅保留工程名）
project(libdatachannel_test_3_20_send VERSION 0.1 LANGUAGES CXX)

# =========================
# 源文件（仅保留你实际用到的，不要删）
# =========================
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

# =========================
# 可执行文件
# =========================
add_executable(${PROJECT_NAME} ${PROJECT_SOURCES})

# =========================
# 【关键】直接链接根目录导入的全局目标
# =========================
target_link_libraries(${PROJECT_NAME} PRIVATE
    datachannel          # 根目录libdatachannel导入的目标
    spdlog::spdlog       # 根目录spdlog导入的目标
    Threads::Threads     # 根目录find的线程库
)