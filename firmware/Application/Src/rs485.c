/* 文件：rs485.c */
/* 功能：实现 RS485 请求发送、应答接收、静默结束和查询间隔保护。 */
/* RS485 事务保留查询间隔，中断缓存避免发送结束后丢失快速应答。 */
#include "rs485.h"
#include "board_config.h"
#include "uart_channel.h"
#include "actuator.h"
#include "brush_motor.h"
#include "at_command.h"
#include <stddef.h>
static uint32_t last_finished;  /* 上次事务结束的毫秒时刻，包括失败与超时的事务。 */
static bool busy;               /* 主循环事务占用标记，不是多任务环境下的互斥锁。 */
/* 清除软件占用标记和查询间隔记录；USART3 必须先由 CubeMX 初始化。 */
void Rs485_Init(void)
{
  last_finished = HAL_GetTick() - BOARD_RS485_QUERY_GAP_MS;
  busy = false;
}

/* 发送一次 RS485 请求，接收至帧间静默或超时；此层不检查 Modbus CRC。 */
HAL_StatusTypeDef Rs485_Transfer(const uint8_t *request, uint16_t request_length,
  uint8_t *response, uint16_t capacity, uint16_t *response_length, uint32_t timeout)
{
  HAL_StatusTypeDef status;
  uint32_t started;
  uint32_t last_byte;
  uint8_t byte;
  uint16_t received = 0U;
  if (response_length != NULL) { *response_length = 0U; }
  if (request == NULL || request_length == 0U || request_length > 256U ||
      response == NULL || capacity < 5U || capacity > 256U || response_length == NULL ||
      timeout == 0U || timeout == HAL_MAX_DELAY) { return HAL_ERROR; }
  if (busy || Actuator_IsBusy() || BrushMotor_IsRunning()) { return HAL_BUSY; }
  busy = true;
  /* 调用间至少等待 50 ms；这是从机准备时间，不是额外使能 GPIO。 */
  while ((HAL_GetTick() - last_finished) < BOARD_RS485_QUERY_GAP_MS)
  { AtCommand_Poll(); HAL_Delay(1U); }
  if (Actuator_IsBusy() || BrushMotor_IsRunning()) { busy=false; return HAL_BUSY; }
  UartChannel_Task();
  UartChannel_Clear(BOARD_RS485_UART);
  started = HAL_GetTick();
  last_byte = started;
  status = UartChannel_Write(BOARD_RS485_UART, request, request_length, timeout);
  if (status == HAL_OK)
  {
    status = HAL_TIMEOUT;
    while ((HAL_GetTick() - started) < timeout)
    {
      UartChannel_Task();
      AtCommand_Poll();
      if (Actuator_IsBusy() || BrushMotor_IsRunning()) { status=HAL_BUSY; break; }
      if (UartChannel_HasError(BOARD_RS485_UART)) { status = HAL_ERROR; break; }
      if (UartChannel_ReadByte(BOARD_RS485_UART, &byte))
      {
        if (received >= capacity) { status = HAL_ERROR; break; }
        response[received++] = byte;
        last_byte = HAL_GetTick();
      }
      else if (received != 0U && (HAL_GetTick() - last_byte) >= BOARD_RS485_FRAME_GAP_MS)
      {
        /* 静默仅标记接收结束；即使残帧也交给上层验证长度、地址和 CRC。 */
        status = HAL_OK;
        break;
      }
      else { HAL_Delay(1U); }
    }
  }
  *response_length = received;
  last_finished = HAL_GetTick();
  busy = false;
  return status;
}
