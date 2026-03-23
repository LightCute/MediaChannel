cmake_minimum_required(VERSION 3.5) 
project(MediaChannel VERSION 1.0.0 LANGUAGES CXX)

# --------------------------
# 1. 全局配置（所有测试工程共用）
# --------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 【关键】禁用libdatachannel的examples、tests、benchmark，大幅加快编译速度
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(NO_BENCHMARK ON CACHE BOOL "" FORCE)

# --------------------------
# 2. 统一find_package系统级库
# --------------------------
find_package(Threads REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED
    gstreamer-1.0
    gstreamer-app-1.0
)
add_library(GStreamer INTERFACE IMPORTED GLOBAL)
target_include_directories(GStreamer INTERFACE ${GST_INCLUDE_DIRS})
target_link_libraries(GStreamer INTERFACE ${GST_LIBRARIES})
target_link_directories(GStreamer INTERFACE ${GST_LIBRARY_DIRS})

# --------------------------
# 3. 先添加根目录的third_party（导入全局目标）
# --------------------------
# ✅ 正确顺序1：先添加libdatachannel子模块（定义datachannel目标）
add_subdirectory(third_party/libdatachannel)

# ✅ 正确顺序2：再对已存在的datachannel目标添加json的include目录


target_include_directories(datachannel INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/libdatachannel/deps/json/include>
)

# 添加spdlog
add_subdirectory(third_party/spdlog)

# --------------------------
# 4. 再添加所有测试工程
# --------------------------
add_subdirectory(libdatachannel_test_3_18)
add_subdirectory(libdatachannel_test_3_20_recv)
add_subdirectory(libdatachannel_test_3_20_send)