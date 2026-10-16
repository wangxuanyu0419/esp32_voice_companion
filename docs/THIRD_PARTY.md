# 第三方组件与资源

各组件保留自己的许可证，本页不为其统一重新授权。

| 资源 | 来源 | 许可证／说明 |
| --- | --- | --- |
| esp_audio_codec | Espressif esp-adf-libs，2.4.0；清单记录 b5600173ea001039d5c501b4bca3f96736d871c0 | [Modified MIT](../components/esp_audio_codec/LICENSE)，仅限配合 Espressif 产品使用；当前只含 ESP32-S3 两个预编译库 |
| Noto Sans CJK | Google / Adobe，来源／转换器版本见清单 | [SIL OFL 1.1](../third_party/licenses/NotoSansCJK-OFL-1.1.txt)、[文件哈希](../third_party/licenses/font_asset_manifest.json) |
| esp-websocket-client | components/espressif__esp-websocket-client | [Apache 2.0](../components/espressif__esp-websocket-client/LICENSE) |
| LVGL | components/lvgl__lvgl | 沿用项目现有组件与上游授权 |
| 显示／触摸／IO 扩展 | main/idf_component.yml、dependencies.lock | IDF Component Manager 获取，按上游许可使用 |

音频库 LICENSE 从本地原始组件包补齐，版本和上游路径在组件 idf_component.yml 中。更新库时应同步头文件、注册源码和版本并重新链接验证。

spiffs_data/font_common_manifest.json 为运行时清单，third_party/licenses/font_asset_manifest.json 记录两个 Noto 字库的字节数和 SHA-256。原有 PuHui / FontAwesome 和头像资产沿用历史来源；Noto 许可证不覆盖全部字体，进一步分发前仍需完善其他资产的来源记录。
