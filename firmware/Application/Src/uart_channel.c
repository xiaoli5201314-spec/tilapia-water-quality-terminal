/* 文件：uart_channel.c */
/* 功能：实现三路 UART 的中断收字节、环形缓冲和错误锁存。 */
/* 单生产者中断、单消费者主循环；溢出必须报错，不能继续解析残帧。 */
#include "uart_channel.h"
#include "board_config.h"
#include <stddef.h>
#define UART_BUFFER_SIZE 2048U /* 保留一个空槽，容纳 AT 等待中的多条命令 URC。 */
typedef struct
{
  UART_HandleTypeDef *uart;                /* 对应 CubeMX 初始化的串口句柄。 */
  uint8_t rx_byte;                         /* HAL 单字节接收的固定存放位置。 */
  volatile uint16_t head;                  /* 中断推进写指针，主循环只读取。 */
  volatile uint16_t tail;                  /* 主循环推进读指针，中断只读取。 */
  volatile bool fault;                     /* 接收故障锁存，事务层据此拒绝残帧。 */
  volatile uint8_t data[UART_BUFFER_SIZE];  /* 中断生产、主循环消费的共享字节缓存。 */
} UartChannel;
static UartChannel channels[3];

/* 按句柄定位固定的三路串口，不自动注册未知串口。 */
static UartChannel *FindChannel(UART_HandleTypeDef *uart)
{
  uint8_t i;
  if (uart == NULL) { return NULL; }
  for (i = 0U; i < 3U; ++i)
  {
    if (channels[i].uart == uart) { return &channels[i]; }
  }
  return NULL;
}

/* 为三路已初始化 UART 启用单字节中断接收并清空软件缓冲。 */
HAL_StatusTypeDef UartChannel_Init(void)
{
  uint8_t i;
  HAL_StatusTypeDef status = HAL_OK;
  channels[0].uart = BOARD_WIFI_UART;
  channels[1].uart = BOARD_4G_UART;
  channels[2].uart = BOARD_RS485_UART;
  for (i = 0U; i < 3U; ++i)
  {
    channels[i].head = 0U;
    channels[i].tail = 0U;
    channels[i].fault = false;
    if (HAL_UART_Receive_IT(channels[i].uart, &channels[i].rx_byte, 1U) != HAL_OK)
    {
      channels[i].fault = true;
      status = HAL_ERROR;
    }
  }
  return status;
}

/* 在主循环检查被 HAL 错误中断终止的接收并尝试重新挂接，不清除错误标记。 */
void UartChannel_Task(void)
{
  uint8_t i;
  /* HAL 遇到溢出可能关闭接收，在主循环重新挂接，保留错误标记。 */
  for (i = 0U; i < 3U; ++i)
  {
    if (channels[i].uart != NULL &&
        channels[i].uart->RxState == HAL_UART_STATE_READY)
    {
      if (HAL_UART_Receive_IT(channels[i].uart, &channels[i].rx_byte, 1U) != HAL_OK)
      { channels[i].fault = true; }
    }
  }
}

/* 丢弃 uart 的软件缓存并清除错误标记；未知串口忽略，不负责清除硬件故障。 */
void UartChannel_Clear(UART_HandleTypeDef *uart)
{
  UartChannel *channel = FindChannel(uart);
  uint32_t mask;
  if (channel == NULL) { return; }
  /* 清空缓冲的短临界区保留调用前的中断开关状态。 */
  mask = __get_PRIMASK();
  __disable_irq();
  channel->tail = channel->head;
  channel->fault = false;
  __set_PRIMASK(mask);
}

/* 由主循环从 uart 软件缓冲读取一个字节，不阻塞等待。 */
bool UartChannel_ReadByte(UART_HandleTypeDef *uart, uint8_t *byte)
{
  UartChannel *channel = FindChannel(uart);
  if (channel == NULL || byte == NULL || channel->tail == channel->head) { return false; }
  *byte = channel->data[channel->tail];
  channel->tail = (uint16_t)((channel->tail + 1U) % UART_BUFFER_SIZE);
  return true;
}
/* 有溢出、HAL 接收错误或串口未注册时返回 true；错误保留至 Clear 或 Init。 */
bool UartChannel_HasError(UART_HandleTypeDef *uart)
{
  UartChannel *channel = FindChannel(uart);
  return channel == NULL || channel->fault;
}
/* 只取走错误锁存，不清除接收字节；上层拒绝当前帧后重同步。 */
bool UartChannel_TakeError(UART_HandleTypeDef *uart)
{
  UartChannel *channel=FindChannel(uart);
  uint32_t mask;
  bool error;
  if (channel==NULL) { return true; }
  mask=__get_PRIMASK(); __disable_irq();
  error=channel->fault; channel->fault=false;
  __set_PRIMASK(mask);
  return error;
}

/* 在 uart 上阻塞发送一段数据，不停止后台中断接收，也不等待模块应答。 */
HAL_StatusTypeDef UartChannel_Write(UART_HandleTypeDef *uart, const uint8_t *data,
                                   uint16_t length, uint32_t timeout)
{
  if (FindChannel(uart) == NULL || data == NULL || length == 0U ||
      timeout == 0U || timeout == HAL_MAX_DELAY) { return HAL_ERROR; }
  return HAL_UART_Transmit(uart, (uint8_t *)data, length, timeout);
}

/* 中断内只入队并挂接下一字节；不解析 AT、Modbus 或执行机械控制。 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *uart)
{
  UartChannel *channel = FindChannel(uart);
  uint16_t next;
  if (channel == NULL) { return; }
  next = (uint16_t)((channel->head + 1U) % UART_BUFFER_SIZE);
  /* 缓冲满时丢弃新字节并标错，不覆盖主循环尚未处理的旧数据。 */
  if (next == channel->tail) { channel->fault = true; }
  else
  {
    channel->data[channel->head] = channel->rx_byte;
    channel->head = next;
  }
  if (HAL_UART_Receive_IT(uart, &channel->rx_byte, 1U) != HAL_OK)
  { channel->fault = true; }
}

/* 中断内锁存错误，重新启用接收由 UartChannel_Task 处理。 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *uart)
{
  UartChannel *channel = FindChannel(uart);
  if (channel != NULL) { channel->fault = true; }
}
