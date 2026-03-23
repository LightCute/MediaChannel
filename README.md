```markdown
# MediaChannel

基于 C++ 和 libdatachannel 的 P2P 音视频通话项目，支持实时数据传输与媒体流渲染。

---

## 功能特性

- ✅ **P2P 数据通道**：基于 libdatachannel 实现低延迟 WebRTC 数据传输
- ✅ **音视频流处理**：支持 H.264 视频和 Opus 音频解析与传输
- ✅ **媒体渲染**：集成 GStreamer 实现音视频播放
- ✅ **模块化设计**：三个独立测试工程，覆盖不同场景
- ✅ **现代 CMake 构建**：清晰的依赖管理与分层结构

---

## 系统依赖

### 必需依赖
| 依赖 | 版本/说明 | 安装要求 |
|------|-----------|----------|
| **CMake** | ≥ 3.5 | `sudo apt install cmake` |
| **C++ 编译器** | 支持 C++17 | `sudo apt install build-essential` |
| **Git LFS** | 大文件管理工具 | `sudo apt install git-lfs` |
| **GStreamer** | 媒体播放框架 | 请自行安装对应平台的开发库 |
| **OpenSSL** | 加密依赖库 | `sudo apt install libssl-dev` |

### 可选依赖
| 依赖 | 说明 |
|------|------|
| **Qt** | GUI 框架（部分测试工程可选依赖），请自行安装对应版本开发库 |

---

## 快速开始

### 1. 克隆项目代码
```bash
git clone git@github.com:LightCute/MediaChannel.git
cd MediaChannel
```

### 2. 初始化子模块
**重要说明**：项目核心依赖 `libdatachannel` 和 `spdlog` 均以 Git 子模块形式托管，必须执行以下命令完成初始化，否则无法正常编译：
```bash
# 初始化并递归更新所有子模块
git submodule update --init --recursive
```

### 3. Git LFS 测试文件拉取
测试音视频文件通过 Git LFS 托管，可按需选择拉取方式：

#### 方式1：拉取全部 LFS 测试文件
```bash
git lfs pull
```

#### 方式2：拉取指定的 LFS 文件/目录
```bash
# 拉取单个指定文件
git lfs pull --include "samples/h264/sample-0.h264"

# 拉取指定目录下的所有文件
git lfs pull --include "samples/h264/*"

# 同时拉取多个指定路径
git lfs pull --include "samples/h264/*,samples/*.mp3"
```

### 4. 项目编译
```bash
# 创建构建目录并进入
mkdir -p build && cd build

# CMake 工程配置
cmake ..

# 执行编译（-j 后可填写CPU核心数以加速编译）
make -j$(nproc)
```

### 5. 运行测试工程
编译完成后，所有可执行文件均输出至 `build/bin/` 目录：
```bash
cd build/bin

# 运行媒体接收端测试工程
./libdatachannel_test_3_20_recv

# 运行媒体发送端测试工程
./libdatachannel_test_3_20_send

# 运行基础功能测试工程
./libdatachannel_test_3_18
```

---

## 项目结构
```
MediaChannel/
├── CMakeLists.txt              # 根目录 CMake 工程配置
├── .gitattributes              # Git LFS 追踪规则配置
├── .gitignore                  # Git 忽略文件规则
├── samples/                    # 测试媒体文件（Git LFS 托管）
│   ├── h264/                   # H.264 格式视频样本集
│   ├── generate_h264.py        # H.264 测试文件生成脚本
│   └── generate_opus.py        # Opus 音频测试文件生成脚本
├── third_party/                # 第三方依赖目录
│   ├── libdatachannel/         # WebRTC 数据通道库（Git Submodule）
│   └── spdlog/                 # 高性能日志库（Git Submodule）
├── libdatachannel_test_3_18/   # 基础功能测试工程
├── libdatachannel_test_3_20_recv/ # 媒体流接收端测试工程
└── libdatachannel_test_3_20_send/ # 媒体流发送端测试工程
```

---

## 核心技术栈
| 分类 | 技术选型 |
|------|----------|
| 开发语言 | C++17 |
| 构建系统 | CMake |
| WebRTC 核心库 | libdatachannel（Git Submodule） |
| 日志组件 | spdlog（Git Submodule） |
| 媒体处理框架 | GStreamer |
| 大文件管理 | Git LFS |
| 版本控制 | Git |

---

## 许可证说明
本项目遵循所使用开源组件的许可证协议：
- libdatachannel：[MPL-2.0 许可证](https://github.com/paullouisageneau/libdatachannel/blob/master/LICENSE)
- spdlog：[MIT 许可证](https://github.com/gabime/spdlog/blob/v1.x/LICENSE)

---

## 项目说明
本项目为学习与研究用途开发，基于东莞理工学院相关课程实践完成。
```