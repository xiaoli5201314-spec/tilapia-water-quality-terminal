/* 文件：water_reporter.h */
/* 功能：传感器上电预热、周期采集和 MQTT 上报调度接口。 */
/* 两只探头的采样及四字段 MQTT 上报调度。 */
#ifndef WATER_REPORTER_H
#define WATER_REPORTER_H
/** 两路供电并开始预热计时，不启动清洗或执行器。 */
void WaterReporter_Init(void);
/** 探头稳定后定时读取四项数据，全部成功才允许发布。 */
void WaterReporter_Task(void);
/* 采集暂停或机械占用时仅清除缓存/恢复标记，不读传感器、不发送 AT。 */
void WaterReporter_Invalidate(void);
#endif
