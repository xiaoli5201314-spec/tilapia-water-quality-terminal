/* 文件：test_main.c */
/* 模块：两路继电器输出（PG13 = OUTPUT1 -> CN1, PG12 = OUTPUT2 -> CN2） */
/* 功能：单路轮流吸合 10 秒，供万用表测通断/电压。 */
/*
 * 硬件（原理图 SCH_继电器）：
 *   PG13/PG12 ─1k─ 各自光耦 EL357N LED 阴极，阳极接 3V3
 *   光耦集电极 ─→ R4 10k ─→ Q(HL2310A) 栅极；另有 10k 栅极→PGND
 *   R2 10k = 栅极上拉到 P24V
 *   Qx 漏极 → CN 的 2 脚；CN 的 1 脚 = P24V
 *   → 输出电压 24V、电流受 HL2310A 限制，适合驱动继电器线圈或小功率负载
 *
 * 有效电平：MCU 出【低】→ 光耦导通 → 栅极被抬到约 11~12V → HL2310A 导通 → CN 1-2 之间有 24V → 吸合
 *           MCU 出【高】→ 光耦不通 → 栅极被 10k 拉到 PGND → 截止 → CN 1-2 之间 0V → 释放
 *
 * 测量（两种都行）：
 *   ① 万用表直流电压档：黑笔 CN.2、红笔 CN.1，吸合时应读到【约 24V】，释放时【0V】
 *   ② 万用表通断档：先断电测量；接继电器线圈时，线圈两端会有线圈电阻（几十~几百Ω）
 *
 * 注意：板上【没有】继电器本体，原理图上 CN1/CN2 是 2 芯接线座，负载要外接。
 *
 * 时序（每档 10 秒，一直循环）：
 *   都释放 → 只吸合 OUTPUT1 → 都释放 → 只吸合 OUTPUT2 → 都释放 → ...
 *
 * 灯（RUN = PE2 = 黄灯，ERR = PE3 = 红灯）：
 *   黄灯闪 1 下 = OUTPUT1 吸合
 *   黄灯闪 2 下 = OUTPUT2 吸合
 *   黄灯灭      = 都释放
 *   红灯每 5 秒闪 = 程序在跑
 */

#include "main.h"
#include "gpio.h"
#include "usart.h"
#include <string.h>
#include <stdio.h>

/* ---------------- 引脚（与 main.h / .ioc 一致） ---------------- */
#define R1_PORT   GPIOG
#define R1_PIN    GPIO_PIN_13     /* OUTPUT1 -> CN1 */
#define R2_PORT   GPIOG
#define R2_PIN    GPIO_PIN_12     /* OUTPUT2 -> CN2 */

#define LED_RUN_PORT   GPIOE
#define LED_RUN_PIN    GPIO_PIN_2
#define LED_ERR_PORT   GPIOE
#define LED_ERR_PIN    GPIO_PIN_3

#define RELAY_ON_LEVEL    GPIO_PIN_RESET   /* 低电平吸合 */
#define RELAY_OFF_LEVEL   GPIO_PIN_SET     /* 高电平释放 */

#define HOLD_MS    10000U   /* 每档保持 10 秒 */

/* 1 = 通过 USART2(PD5/PD6) 打印调试日志；0 = 关闭 */
#define TEST_DEBUG_UART   1U

/* ---------------- 继电器控制 ---------------- */
static void AllOff(void)
{
  HAL_GPIO_WritePin(R1_PORT, R1_PIN, RELAY_OFF_LEVEL);
  HAL_GPIO_WritePin(R2_PORT, R2_PIN, RELAY_OFF_LEVEL);
}

/* 只让第 n 路吸合（1/2），其他释放；n=0 表示都释放 */
static void OnlyOne(int n)
{
  AllOff();
  if (n == 1) { HAL_GPIO_WritePin(R1_PORT, R1_PIN, RELAY_ON_LEVEL); }
  else if (n == 2) { HAL_GPIO_WritePin(R2_PORT, R2_PIN, RELAY_ON_LEVEL); }
}

/* ---------------- 调试日志 ---------------- */
#if (TEST_DEBUG_UART != 0U)
static void LogStr(const char *s)
{
  if (s == NULL) { return; }
  (void)HAL_UART_Transmit(&huart2, (uint8_t *)s, (uint16_t)strlen(s), 200U);
}
static void LogLine(const char *s) { LogStr(s); LogStr("\r\n"); }
static void LogCh(int n)
{
  char buf[176];
  int m;
  if (n == 0)
  {
    m = snprintf(buf, sizeof(buf),
      "[ALL OFF] PG13=%d PG12=%d  -> CN1 1-2 = 0V , CN2 1-2 = 0V\r\n",
      (int)HAL_GPIO_ReadPin(R1_PORT, R1_PIN),
      (int)HAL_GPIO_ReadPin(R2_PORT, R2_PIN));
  }
  else if (n == 1)
  {
    m = snprintf(buf, sizeof(buf),
      "[OUT1 ON] PG13=%d PG12=%d  -> CN1 1-2 should be ~24V (pin1 +)\r\n",
      (int)HAL_GPIO_ReadPin(R1_PORT, R1_PIN),
      (int)HAL_GPIO_ReadPin(R2_PORT, R2_PIN));
  }
  else
  {
    m = snprintf(buf, sizeof(buf),
      "[OUT2 ON] PG13=%d PG12=%d  -> CN2 1-2 should be ~24V (pin1 +)\r\n",
      (int)HAL_GPIO_ReadPin(R1_PORT, R1_PIN),
      (int)HAL_GPIO_ReadPin(R2_PORT, R2_PIN));
  }
  if (m > 0) { LogStr(buf); }
}
#else
static void LogStr(const char *s) { (void)s; }
static void LogLine(const char *s) { (void)s; }
static void LogCh(int n) { (void)n; }
#endif

/* 保持某档 HOLD_MS；run_blinks = 黄灯开始时闪几下（0 = 一直灭） */
static void HoldCh(int n, uint32_t total_ms, int run_blinks)
{
  uint32_t elapsed = 0U;
  OnlyOne(n);

  while (elapsed < total_ms)
  {
    HAL_GPIO_WritePin(LED_ERR_PORT, LED_ERR_PIN,
      ((elapsed % 5000U) < 100U) ? GPIO_PIN_RESET : GPIO_PIN_SET);

    if (elapsed < 1200U && run_blinks > 0)
    {
      uint32_t slot = elapsed / 300U;
      int on = ((slot % 2U) == 0U) && (slot < (uint32_t)(run_blinks * 2));
      HAL_GPIO_WritePin(LED_RUN_PORT, LED_RUN_PIN, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
    }
    else
    {
      HAL_GPIO_WritePin(LED_RUN_PORT, LED_RUN_PIN, GPIO_PIN_SET);
    }

    HAL_Delay(20U);
    elapsed += 20U;
  }
  HAL_GPIO_WritePin(LED_ERR_PORT, LED_ERR_PIN, GPIO_PIN_SET);
  HAL_GPIO_WritePin(LED_RUN_PORT, LED_RUN_PIN, GPIO_PIN_SET);
}

/* ---------------- 最小系统支撑 ---------------- */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef osc = {0};
  RCC_ClkInitTypeDef clk = {0};

  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  osc.HSEState = RCC_HSE_ON;
  osc.PLL.PLLState = RCC_PLL_ON;
  osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  osc.PLL.PLLM = 8U;
  osc.PLL.PLLN = 336U;
  osc.PLL.PLLP = RCC_PLLP_DIV2;
  osc.PLL.PLLQ = 7U;
  if (HAL_RCC_OscConfig(&osc) != HAL_OK) { Error_Handler(); }

  clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                  RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clk.APB1CLKDivider = RCC_HCLK_DIV4;
  clk.APB2CLKDivider = RCC_HCLK_DIV2;
  if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_5) != HAL_OK) { Error_Handler(); }
}

void Error_Handler(void)
{
  AllOff();              /* 出错先全部释放，安全第一 */
  __disable_irq();
  for (;;)
  {
    HAL_GPIO_WritePin(LED_RUN_PORT, LED_RUN_PIN | LED_ERR_PIN, GPIO_PIN_RESET);
    HAL_Delay(100U);
    HAL_GPIO_WritePin(LED_RUN_PORT, LED_RUN_PIN | LED_ERR_PIN, GPIO_PIN_SET);
    HAL_Delay(100U);
  }
}

/* ---------------- 主程序 ---------------- */
int main(void)
{
  uint8_t i;

  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_USART2_UART_Init();   /* 仅用于日志 */

  LogStr("\r\n\r\n=== XIAOLI RELAY (OUTPUT1/OUTPUT2) TEST ===\r\n");
  LogLine("CN1 = OUTPUT1 (PG13),  CN2 = OUTPUT2 (PG12); pin1 = P24V, pin2 = switch drain");
  LogLine("meter: black on CN.x pin2, red on CN.x pin1 -> ~24V when ON, 0V when OFF");
  LogLine("cycle: ALL OFF -> OUT1 ON -> ALL OFF -> OUT2 ON -> ... (10s each)");

  AllOff();

  for (i = 0U; i < 3U; ++i)
  {
    HAL_GPIO_WritePin(LED_RUN_PORT, LED_RUN_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LED_ERR_PORT, LED_ERR_PIN, GPIO_PIN_RESET);
    HAL_Delay(150U);
    HAL_GPIO_WritePin(LED_RUN_PORT, LED_RUN_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(LED_ERR_PORT, LED_ERR_PIN, GPIO_PIN_SET);
    HAL_Delay(150U);
  }

  LogLine("init done, cycling...");

  for (;;)
  {
    LogCh(0);
    HoldCh(0, HOLD_MS, 0);

    LogCh(1);
    HoldCh(1, HOLD_MS, 1);

    LogCh(0);
    HoldCh(0, HOLD_MS, 0);

    LogCh(2);
    HoldCh(2, HOLD_MS, 2);

    AllOff();
    LogLine("");
  }
}
