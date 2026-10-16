/* 文件：uart_channel.h */
/* 功能：三路 UART 的中断接收环形缓冲和阻塞发送接口。 */
/* 三路串口的中断接收缓冲，禁止与 HAL 阻塞接收混用。 */
#ifndef UART_CHANNEL_H
#define UART_CHANNEL_H
#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>
/**
 * @brief 为三路已初始化 UART 启用单字节中断接收并清空软件缓冲。
 * @return 全部启动成功为 HAL_OK；任一路失败为 HAL_ERROR，并保留该路错误标记。
 * @note 在 CubeMX UART 初始化后调用一次；不可在收发事务进行中重新初始化。
 */
HAL_StatusTypeDef UartChannel_Init(void);
/** 在主循环检查被 HAL 错误中断终止的接收并尝试重新挂接，不清除错误标记。 */
void UartChannel_Task(void);
/** 丢弃 uart 的软件缓存并清除错误标记；未知串口忽略，不负责清除硬件故障。 */
void UartChannel_Clear(UART_HandleTypeDef *uart);
/**
 * @brief 由主循环从 uart 软件缓冲读取一个字节，不阻塞等待。
 * @param byte 非空输出指针；无数据或参数无效时保持原值。
 * @return 成功读取为 true，否则 false；调用者仍需另行检查 HasError。
 */
bool UartChannel_ReadByte(UART_HandleTypeDef *uart, uint8_t *byte);
/** 有溢出、HAL 接收错误或串口未注册时返回 true；错误保留至 Clear 或 Init。 */
bool UartChannel_HasError(UART_HandleTypeDef *uart);
/* 前台取走错误标记但保留全部缓存，流解析器会拒绝当前残帧。 */
bool UartChannel_TakeError(UART_HandleTypeDef *uart);
/**
 * @brief 在 uart 上阻塞发送一段数据，不停止后台中断接收，也不等待模块应答。
 * @param data 非空发送缓冲区；length 为大于 0 的字节数。
 * @param timeout HAL 发送超时，单位 ms，不接受 0 或 HAL_MAX_DELAY。
 * @return 参数无效返回 HAL_ERROR，否则原样返回 HAL_UART_Transmit 状态。
 * @note 只在主循环使用，不承担机械动作保护；上层必须限制阻塞调用时机。
 */
HAL_StatusTypeDef UartChannel_Write(UART_HandleTypeDef *uart, const uint8_t *data,
                                   uint16_t length, uint32_t timeout);
#endif
