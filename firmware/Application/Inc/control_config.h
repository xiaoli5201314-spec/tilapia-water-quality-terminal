/* 文件：control_config.h */
/* 功能：定义可持久化配置、定长记录与校验接口，不保存动作运行状态。 */
#ifndef CONTROL_CONFIG_H
#define CONTROL_CONFIG_H
#include <stdbool.h>
#include <stdint.h>
#define CONTROL_CONFIG_RECORD_SIZE 128U
#define CONTROL_CONFIG_FLASH_BASE 0x080E0000UL
#define CONTROL_CONFIG_FLASH_SIZE 0x00020000UL
typedef enum { COLLECTION_RUN, COLLECTION_PAUSE, COLLECTION_SCHEDULE } CollectionMode;
typedef enum { PLAN_OFF, PLAN_DAILY, PLAN_INTERVAL } PlanType;
typedef enum { ACTION_STOP, ACTION_START, ACTION_EXTEND, ACTION_RETRACT } ControlAction;
typedef struct {
  uint32_t type, start, interval, duration, action;
} ControlPlan;
typedef struct {
  uint32_t enabled, interval, extend, brush, retract, settle;
} ControlCleaning;
typedef struct {
  uint32_t manual, collection_mode, start, end, sample, publish;
  ControlPlan brush_plan, actuator_plan;
  ControlCleaning cleaning;
} ControlConfig;
/* 生成默认配置，验证完整范围并编解码小端记录。 */
void ControlConfig_Default(ControlConfig *config);
bool ControlConfig_Valid(const ControlConfig *config);
bool ControlConfig_Encode(const ControlConfig *config, uint32_t sequence, uint8_t *record);
bool ControlConfig_Decode(const uint8_t *record, ControlConfig *config, uint32_t *sequence);
uint32_t ControlConfig_Crc(const uint8_t *data, uint32_t length);
/* 读取最新有效日志；仅配置实际变化且机械停止时追加新记录。 */
bool ControlConfig_Load(ControlConfig *config);
bool ControlConfig_Save(const ControlConfig *config);
#ifdef CONTROL_HOST_TEST
/* 主机测试专用存储复位，不编译进实机固件。 */
void ControlConfig_ResetStorage(void);
#endif
#endif
