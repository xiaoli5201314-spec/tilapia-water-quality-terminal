/* 文件：application.h */
/* 功能：应用层初始化、主循环调度和紧急停止接口。 */
/* 主循环入口及紧急停止接口。 */
#ifndef __APPLICATION_H__
#define __APPLICATION_H__

/** 初始化串口、心跳、两路探头供电和 Wi-Fi 业务；串口失败则不打开探头。 */
void Application_Init(void);
/** 在主循环持续调用，维护动作保护、网络连接和水质采集发布。 */
void Application_Task(void);
/** 停止推杆和毛刷、释放继电器并亮红灯/ERR；按当前需求不切断传感器电源。 */
void Application_EmergencyStop(void);

#endif /* __APPLICATION_H__ */
