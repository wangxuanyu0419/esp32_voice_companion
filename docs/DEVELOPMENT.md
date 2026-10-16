# 开发指南

## 环境与本地配置

目标为 Waveshare ESP32-S3-Touch-AMOLED-1.8（16 MB Flash、8 MB OPI PSRAM）。锁文件和本次验证使用 **ESP-IDF 5.5.2**；组件清单宽泛的最低版本不代表已经验证所有旧版本。

Windows 在 ESP-IDF 终端执行命令；手工激活时先设置自己的 IDF_TOOLS_PATH，再加载 SDK 的 export.ps1。Linux/macOS 加载 export.sh。SDK、工具链、build、managed_components 和本机 sdkconfig 不提交。

```powershell
Copy-Item main/system/wifi_networks.example.h main/system/wifi_networks.local.h
```

本地文件每行一个 `{ "SSID", "PASSWORD" },`，是数组初始化内容。目标支持 2.4 GHz Wi-Fi。缺少本地文件时使用占位模板，能编译但不会连接真实网络。设置页选择列表中的网络，尚无完整触屏密码输入流程；NVS 网络索引不能替代源码网络列表。

当前应用从 NVS 读取 `app_config_t.cf_client_id`、`cf_client_secret`、`ws_auth_token`，分别用于 Access／Bearer 认证。通过本地配置工具或开发代码调用 config_save() 写入，当前 UI 不提供完整凭据配置。线上认证要求以实际服务配置为准。

旧桥接若单独使用，可复制 `components/openclaw_bridge/openclaw_secrets.example.h` 为同目录的 `openclaw_secrets.local.h`；它仅配置旧 CLAWCHAT_APP_TOKEN，不会自动写入当前应用 NVS。`*.local.h` 被忽略。

当前树已移除原有内嵌连接凭据。历史提交仍可能包含旧值；若仍有效需在对应服务中轮换。本次未改服务认证或重写 Git 历史。

## 构建与烧录

首次配置或切换目标才运行 set-target；它会重建配置。日常直接 build。所有命令从仓库根目录运行：

```text
idf.py set-target esp32s3
idf.py build
idf.py -p COM4 flash monitor
```

将 COM4 换成实际端口；macOS/Linux 使用实际 /dev/...。首次完整 flash 包含 SPIFFS，app-flash 不更新字体／头像。使用 IDF 生成的烧录参数，不沿用旧文档的手动偏移。

| 分区 | 偏移 | 大小 |
| --- | --- | --- |
| nvs | 0x10000 | 0x6000 |
| otadata | 0x16000 | 0x2000 |
| app0 | 0x20000 | 0x300000 |
| app1 | 0x320000 | 0x300000 |
| spiffs | 0x620000 | 0x9E0000 |

有双 OTA 分区不等于远程 OTA 升级功能已完成。sdkconfig 是本地生成文件，复现时需记录与 sdkconfig.defaults 的差异。

## 调试工具

```text
python -m pip install pyserial
python tools/serial_passive_capture.py
python tools/serial_cold_boot.py
```

两个脚本目前固定 COM4 / 115200，换端口需先编辑。passive 采集约 180 秒并设置 DTR/RTS 为 false；cold_boot 切换 RTS 并采集约 30 秒，会重启设备。不要与其他串口监视器同时使用；脚本导入即运行，不能作为库导入。

原有 tools/monitor.py 为网页串口监视器；启动或调整监听端口前须遵循本机共享 Cloudflare／端口管理说明并核对占用。本次没有启动该服务。

## TF 媒体准备

需要 FFmpeg 在 PATH。转换器默认输出 368×410、10 fps 的 VIDEO.MJPG 和单声道 16 kHz PCM16 AUDIO.WAV，居中裁剪并使用短文件夹名：

```text
python tools/mp4_to_mjpeg.py --help
python tools/mp4_to_mjpeg.py input.mp4 --out-dir VIDEO001
```

test_media/media/ 提供样本。当前 main.c 没有初始化／注册媒体应用，使用前需恢复入口并上板验收。

## 维护

以 main/CMakeLists.txt 为构建清单，新增资源时检查 SPIFFS 容量与授权。提交前检查 git diff --check 和忽略规则；不要纳入 SDK、构建产物、NVS 镜像、个人日志或凭据。功能变化同步更新 [开发状态](DEVELOPMENT_STATUS.md) 和 [验证记录](VALIDATION.md)。
