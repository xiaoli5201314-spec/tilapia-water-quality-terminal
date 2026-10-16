/* 文件：at_stream.h */
/* 功能：定义持续 AT 行/提示符与按长度 MQTT URC 解复用接口。 */
#ifndef AT_STREAM_H
#define AT_STREAM_H
#include "stm32f4xx_hal.h"
#include <stdint.h>
typedef void (*AtStreamLineCallback)(UART_HandleTypeDef *uart, const char *line);
typedef void (*AtStreamMessageCallback)(const char *topic, const uint8_t *payload, uint16_t length);
/* 初始化仅在上电执行，事务切换不得清空解析状态。 */
void AtStream_Init(void);
void AtStream_SetCallbacks(AtStreamLineCallback line, AtStreamMessageCallback message);
void AtStream_Feed(UART_HandleTypeDef *uart, uint8_t byte);
/* UART 溢出时拒绝当前残帧并重新寻找行边界，绝不执行不完整命令。 */
void AtStream_Fault(UART_HandleTypeDef *uart);
#endif
