/* 文件：network_manager.h */
/* 功能：Wi-Fi 优先、可选 4G 备用和单活动 MQTT 链路状态机接口。 */
/* 当前默认只使用 Wi-Fi；后续打开配置宏后才启用 4G 切换。 */
#ifndef __NETWORK_MANAGER_H__
#define __NETWORK_MANAGER_H__

#include "mqtt_transport.h"

/** 重置连接流程，初始选择 Wi-Fi；不把 RDY 当成联网成功。 */
void NetworkManager_Init(void);
/** 推进联网步骤，连接成功后定时检查状态并尝试切回 Wi-Fi。 */
void NetworkManager_Task(void);
/** 仅连接状态经过校验后返回 Wi-Fi 或 4G。 */
MqttLink NetworkManager_GetActiveLink(void);
/** 发布失败立即停用原链路并安排重新连接。 */
void NetworkManager_ReportPublishFailure(MqttLink link);
/* URC 前台回调仅标记断开与取消旧命令，不发送 AT。 */
void NetworkManager_NotifyDisconnected(void);

#endif /* __NETWORK_MANAGER_H__ */
