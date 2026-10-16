/* 文件：remote_control.h */
/* 功能：统一远程命令、采集配置、机械计划与四分片状态快照接口。 */
#ifndef REMOTE_CONTROL_H
#define REMOTE_CONTROL_H
#include "control_config.h"
#include <stdint.h>
#include <stdbool.h>
#define CONTROL_STATUS_PARTS 4U
#define CONTROL_STATUS_CAPACITY 400U
void RemoteControl_Init(void);
/* AT 前台回调只入队；停止有独立优先槽，禁止回调发送 AT。 */
void RemoteControl_Receive(const char *topic, const uint8_t *payload, uint16_t length);
void RemoteControl_Poll(void);
void RemoteControl_PersistTask(void);
void RemoteControl_StatusTask(void);
void RemoteControl_SetConnected(bool connected);
void RemoteControl_EmergencyStop(void);
const char *RemoteControl_CommandTopic(void);
const char *RemoteControl_StatusTopic(void);
const ControlConfig *RemoteControl_Config(void);
bool RemoteControl_CollectionActive(void);
bool RemoteControl_AckRejected(void);
/* 捕获四片后正文冻结；每片最多399字节、同一 boot/seq，仅发布一次。 */
bool RemoteControl_CaptureStatus(uint32_t sequence);
const char *RemoteControl_StatusPart(uint32_t part);
#endif
