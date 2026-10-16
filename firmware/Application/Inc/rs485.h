/* 文件：rs485.h */
/* 功能：USART3 RS485 请求发送、应答接收和帧间隔控制接口。 */
/* TP8485E-SR 自动方向电路：只用 USART3，不配置额外 DE/RE。 */
#ifndef __RS485_H__
#define __RS485_H__
#include "stm32f4xx_hal.h"
#include <stdint.h>
/** 清除软件占用标记和查询间隔记录；USART3 必须先由 CubeMX 初始化。 */
void Rs485_Init(void);
/**
 * @brief 发送一次 RS485 请求，接收至帧间静默或超时；此层不检查 Modbus CRC。
 * @param request 非空请求字节数组；request_length 为字节数 1~256。
 * @param response 非空接收缓冲区；capacity 为总字节容量 5~256。
 * @param response_length 必须非空；返回已收到的字节数，失败时也可能非零。
 * @param timeout 发送与接收阶段的总超时，单位 ms，不含事务前的查询间隔等待。
 * @return HAL_OK 只表示收到帧并检测到静默，仍须由协议层校验；HAL_TIMEOUT 表示超时；
 *         HAL_BUSY 为事务占用或机械动作中；HAL_ERROR 为参数、串口或缓冲溢出错误。
 * @note 只能在主循环调用；机械动作期间不允许同步阻塞。自动方向由外部电路完成。
 */
HAL_StatusTypeDef Rs485_Transfer(const uint8_t *request, uint16_t request_length,
  uint8_t *response, uint16_t capacity, uint16_t *response_length, uint32_t timeout);
#endif
