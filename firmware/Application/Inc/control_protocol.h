/* 文件：control_protocol.h */
/* 功能：定义 v1 平坦命令和严格类型/字段校验入口。 */
#ifndef CONTROL_PROTOCOL_H
#define CONTROL_PROTOCOL_H
#include "control_config.h"
#define CONTROL_COMMAND_MAX 384U
typedef enum {
  OP_SAMPLING, OP_MODE, OP_BRUSH, OP_ACTUATOR, OP_PLAN,
  OP_CLEANING_CONFIG, OP_CLEANING, OP_STOP, OP_QUERY
} ControlOp;
typedef struct {
  char id[13];
  ControlOp op;
  uint32_t issued, mode, action, duration, target;
  uint32_t start, end, sample, publish, type, interval;
  uint32_t enabled, extend, brush, retract, settle;
} ControlCommand;
/* 提取可回执的有效 ID；仅整帧语法和业务 schema 合法时返回 true。 */
bool ControlProtocol_Parse(const uint8_t *json, uint16_t length,
                          ControlCommand *command, char ack_id[13]);
#endif
