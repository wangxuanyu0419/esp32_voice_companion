# ESP32 Voice Companion

基于 Waveshare **ESP32-S3-Touch-AMOLED-1.8** 的 ClawChat 语音终端：触屏角色界面、语音输入、流式语音播放、文字广播和图片消息。

仓库：<https://github.com/wangxuanyu0419/esp32_voice_companion>

## 当前进度

截至 **2026-10-16**，已整理本地四层架构固件，包含自适应 VAD、半双工收放音、WebSocket PCM 流、HTTP 音频广播、远程图片、中文字体及音量迁移。开机直接进入聊天页；设置页支持网络、服务器、音量和 VAD 参数。TF 媒体代码保留，但默认启动流程未注册该应用。

本次使用 **ESP-IDF 5.5.2** 在原开发环境增量编译通过。线上服务及实物效果未在本次重新验收，详见 [当前状态](docs/DEVELOPMENT_STATUS.md) 和 [验证记录](docs/VALIDATION.md)。

## 快速开始

准备目标开发板（16 MB Flash / 8 MB OPI PSRAM）、USB 数据线和 ESP-IDF **v5.5.2**。在已激活的 IDF 终端运行：

```powershell
git clone https://github.com/wangxuanyu0419/esp32_voice_companion.git
cd esp32_voice_companion
Copy-Item main/system/wifi_networks.example.h main/system/wifi_networks.local.h
# 编辑本地 Wi-Fi 文件后再编译
idf.py set-target esp32s3
idf.py build
# 替换为实际端口；以下操作将烧录设备
idf.py -p COM4 flash monitor
```

首次烧录需包含 SPIFFS 字体和头像。服务端不包含在本仓库中；认证、配置和调试见 [开发指南](docs/DEVELOPMENT.md)。

## 文档

| 文档 | 用途 |
| --- | --- |
| [开发指南](docs/DEVELOPMENT.md) | 环境、配置、编译、烧录、串口、媒体转换 |
| [开发状态](docs/DEVELOPMENT_STATUS.md) | 已实现功能、限制和下一步 |
| [架构与协议](docs/ARCHITECTURE.md) | 当前入口、任务和服务端消息 |
| [验证与验收](docs/VALIDATION.md) | 本次证据和上板清单 |
| [N100 音频兼容](docs/AUDIO_BROADCAST.md) | fMP4 转封装与发送端集成 |
| [第三方资源](docs/THIRD_PARTY.md) | 组件、字体和许可证 |
| [历史文档](docs/history/) | 4 月原型记录；不作为当前操作指南 |

## 目录

```text
main/main.c           当前 app_main 入口
main/board/           开发板和外设
main/system/          NVS 配置、Wi-Fi、事件队列
main/framework/       音频、协议、显示和休眠
main/apps/            聊天、设置及保留的应用
components/           随仓库分发的组件
spiffs_data/          运行时字体和头像
tools/                串口、媒体和 N100 辅助工具
docs/                 当前文档和历史记录
```

以 `main/CMakeLists.txt` 的 SRCS 为构建清单。早期 `components/openclaw_bridge` 保留；当前应用协议在 `main/framework/protocol/ws_protocol.c`。
