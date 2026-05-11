/*
 * openclaw_bridge.h — OpenClaw Bridge for ESP32-S3 (xiaozhi firmware integration)
 *
 * 调用方式:
 *   #include "openclaw_bridge.h"
 *   openclaw_bridge_start();
 *
 * 约束:
 *   - 仅使用 C (no C++ STL, no exceptions)
 *   - 不分配堆栈大对象，不在 ISR 里调用
 *   - 不修改 xiaozhi 已有初始化流程
 */

#ifndef _OPENCLAW_BRIDGE_H_
#define _OPENCLAW_BRIDGE_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 启动 OpenClaw Bridge.
 * 在 app_main 里调用，放在 xiaozhi 初始化之后。
 * 内部创建单独 FreeRTOS task，不阻塞.
 */
void openclaw_bridge_start(void);

/**
 * 主动断开连接并停止 bridge task.
 */
void openclaw_bridge_stop(void);

/**
 * 返回当前连接状态.
 * 0 = 未连接, 1 = 已连接, 2 = 连接中
 */
int openclaw_bridge_status(void);

#ifdef __cplusplus
}
#endif

#endif // _OPENCLAW_BRIDGE_H_
