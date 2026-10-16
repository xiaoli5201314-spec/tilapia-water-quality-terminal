/* 文件：at_command.h */
/* 功能：前台 AT 事务、持续 URC 解析和 GPIO 保护等待接口，禁止回调重入。 */
#ifndef AT_COMMAND_H
#define AT_COMMAND_H
#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>
void AtCommand_Init(void);
void AtCommand_Poll(void);
bool AtCommand_IsBusy(void);
HAL_StatusTypeDef AtCommand_ClassifyLine(const char *line);
HAL_StatusTypeDef AtCommand_Execute(UART_HandleTypeDef *uart, const char *command,
  char *response, uint16_t capacity, uint32_t timeout);
HAL_StatusTypeDef AtCommand_ExecuteUntil(UART_HandleTypeDef *uart, const char *command,
  const char *event, char *response, uint16_t capacity, uint32_t timeout);
/* 最近一次 RAW 事务记录留在 RAM，可在 Keil Watch 查看，不包含联网密码。 */
typedef enum {
  AT_RAW_IDLE, AT_RAW_PROMPT, AT_RAW_PAYLOAD, AT_RAW_ACK, AT_RAW_DONE
} AtRawStage;
typedef struct {
  AtRawStage stage;
  HAL_StatusTypeDef result;
  uint32_t elapsed_ms;
  uint16_t expected_bytes, sent_bytes;
  char last_line[96];
} AtRawTrace;
extern volatile AtRawTrace g_at_raw_trace;
/* 提示符和发布应答分别等待 timeout；正文不加 CRLF，保持精确字节长度。 */
HAL_StatusTypeDef AtCommand_PublishRaw(UART_HandleTypeDef *uart, const char *command,
  const uint8_t *payload, uint16_t length, uint32_t timeout);
#endif
