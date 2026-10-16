/* 文件：test_main.c */
/* 模块：三色报警灯（PB5=LED1_G, PB4=LED2_G, PB3=LED3_G -> CN2） */
/* 功能：单通道轮流点亮 10 秒，供万用表测 CN2 各路对地电压。 */
/*
 * 硬件（原理图 SCH_三色灯）：
 *   PB5/PB4/PB3 ─1k─ 各自的光耦 EL357N LED 阴极，阳极接 3V3
 *   光耦集电极 -> 10k -> Q(HL2310A) 栅极；另一个 10k 栅极->PGND
 *   Qx 源极 = PGND，漏极 = CN2 的第 2/3/4 脚
 *   CN2: 1 = P24V, 2 = LED1_G 通道, 3 = LED2_G 通道, 4 = LED3_G 通道, 5 = PGND
 *   → 外部灯必须是【共阳】接法：灯的正极接 P24V，负极分别接 2/3/4
 *
 * 有效电平：MCU 出【低】→ 光耦导通 → 栅极被抬到约 11~12V → HL2310A 导通 → 该路拉低 → 灯亮
 *           MCU 出【高】→ 光耦不通 → 栅极被 10k 拉到 PGND → 截止 → 该路悬空 → 灯灭
 *
 * ⚠️ 代码里的命名和原理图丝印对不上，务必先记下来：
 *   PB5 网络名 LED1_G，但原理图三色灯图页该路丝印是【蓝灯】
 *   PB4 网络名 LED2_G，丝印【黄灯】
 *   PB3 网络名 LED3_G，丝印【红灯】
 *   代码里写的是 ALARM_RED=PB5 / ALARM_YELLOW=PB4 / ALARM_GREEN=PB3
 *   → 请按实物告诉我 PB5 到底是哪一颗，我把命名统一过来。
 *
 * 时序（每档 10 秒，一直循环）：
 *   全灭 → 只亮 1 路 → 只亮 2 路 → 只亮 3 路 → 全灭 → ...
 *
 * 测量：万用表黑笔接 GND，红笔依次测 CN2 的 2/3/4 脚对地电压。
 *   某一路亮时该脚应被拉到【接近 0V】；不亮的那两路【悬空】（读数不定/接近 24V）。
 *   更直接的判据：看灯 + 看黄灯指示，不用万用表也能确认。
 *
 * 灯（RUN = PE2 = 黄灯，ERR = PE3 = 红灯）：
 *   黄灯闪 1 下 = 正在点第 1 路（PB5）
 *   黄灯闪 2 下 = 第 2 路（PB4）
 *   黄灯闪 3 下 = 第 3 路（PB3）
 *   黄灯灭      = 全灭
 *   红灯每 5 秒闪 = 程序在跑
 */

#include "main.h"
#include "gpio.h"
#include "usart.h"
#include <string.h>
#include <stdio.h>

/* ---------------- 引脚（与 main.h / .ioc 一致） ---------------- */
#define CH1_PORT   GPIOB
#define CH1_PIN    GPIO_PIN_5      /* LED1_G -> CN2.2 */
#define CH2_PORT   GPIOB
#define CH2_PIN    GPIO_PIN_4      /* LED2_G -> CN2.3 */
#define CH3_PORT   GPIOB
#define CH3_PIN    GPIO_PIN_3      /* LED3_G -> CN2.4 */

#define LED_RUN_PORT   GPIOE
#define LED_RUN_PIN    GPIO_PIN_2
#define LED_ERR_PORT   GPIOE
#define LED_ERR_PIN    GPIO_PIN_3

#define CH_ON_LEVEL    GPIO_PIN_RESET   /* 低电平点亮 */
#define CH_OFF_LEVEL   GPIO_PIN_SET     /* 高电平熄灭 */

#define HOLD_MS    10000U   /* 每档保持 10 秒 */

/* 1 = 通过 USART2(PD5/PD6) 打印调试日志；0 = 关闭 */
#define TEST_DEBUG_UART   1U

/* ---------------- 通道控制 ---------------- */
static void AllOff(void)
{
  HAL_GPIO_WritePin(CH1_PORT, CH1_PIN, CH_OFF_LEVEL);
  HAL_GPIO_WritePin(CH2_PORT, CH2_PIN, CH_OFF_LEVEL);
  HAL_GPIO_WritePin(CH3_PORT, CH3_PIN, CH_OFF_LEVEL);
}

/* 只点亮第 n 路（1/2/3），其他全灭；n=0 表示全灭 */
static void OnlyOne(int n)
{
  AllOff();
  if (n == 1) { HAL_GPIO_WritePin(CH1_PORT, CH1_PIN, CH_ON_LEVEL); }
  else if (n == 2) { HAL_GPIO_WritePin(CH2_PORT, CH2_PIN, CH_ON_LEVEL); }
  else if (n == 3) { HAL_GPIO_WritePin(CH3_PORT, CH3_PIN, CH_ON_LEVEL); }
}

/* ---------------- 调试日志 ---------------- */
#if (TEST_DEBUG_UART != 0U)
static void LogStr(const char *s)
{
  if (s == NULL) { return; }
  (void)HAL_UART_Transmit(&huart2, (uint8_t *)s, (uint16_t)strlen(s), 200U);
}
static void LogLine(const char *s) { LogStr(s); LogStr("\r\n"); }
static void LogChannel(int n)
{
  char buf[176];
  int n2;
  if (n == 0)
  {
    n2 = snprintf(buf, sizeof(buf),
      "[OFF  ] PB5=%d PB4=%d PB3=%d  -> all channels off\r\n",
      (int)HAL_GPIO_ReadPin(CH1_PORT, CH1_PIN),
      (int)HAL_GPIO_ReadPin(CH2_PORT, CH2_PIN),
      (int)HAL_GPIO_ReadPin(CH3_PORT, CH3_PIN));
  }
  else
  {
    n2 = snprintf(buf, sizeof(buf),
      "[CH%d  ] PB5=%d PB4=%d PB3=%d  -> CN2.%d should be pulled LOW (near 0V)\r\n",
      n,
      (int)HAL_GPIO_ReadPin(CH1_PORT, CH1_PIN),
      (int)HAL_GPIO_ReadPin(CH2_PORT, CH2_PIN),
      (int)HAL_GPIO_ReadPin(CH3_PORT, CH3_PIN),
      n + 1);
  }
  if (n2 > 0) { LogStr(buf); }
}
#else
static void LogStr(const char *s) { (void)s; }
static void LogLine(const char *s) { (void)s; }
static void LogChannel(int n) { (void)n; }
#endif

/* 保持某档 HOLD_MS；run_blinks = 黄灯在这一档开始时闪几下（0 = 黄灯灭） */
static void HoldChannel(int n, uint32_t total_ms, int run_blinks)
{
  uint32_t elapsed = 0U;
  OnlyOne(n);

  /* 档位开始的 1 秒先闪指示（闪 run_blinks 下），之后黄灯：点亮时灭、全灭时灭 */
  while (elapsed < total_ms)
  {
    HAL_GPIO_WritePin(LED_ERR_PORT, LED_ERR_PIN,
      ((elapsed % 5000U) < 100U) ? GPIO_PIN_RESET : GPIO_PIN_SET);

    if (elapsed < 1200U && run_blinks > 0)
    {
      uint32_t slot = elapsed / 300U;         /* 每 300ms 一个槽 */
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
  AllOff();              /* 出错先全灭 */
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

  LogStr("\r\n\r\n=== XIAOLI TRI-COLOR LIGHT TEST ===\r\n");
  LogLine("CN2: 1=P24V  2=PB5 channel  3=PB4 channel  4=PB3 channel  5=PGND");
  LogLine("external lamp must be COMMON ANODE (+24V), cathodes to pins 2/3/4");
  LogLine("cycle: OFF -> CH1 -> CH2 -> CH3 -> OFF ... (10s each)");

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
    LogChannel(0);
    HoldChannel(0, HOLD_MS, 0);

    LogChannel(1);
    HoldChannel(1, HOLD_MS, 1);

    LogChannel(2);
    HoldChannel(2, HOLD_MS, 2);

    LogChannel(3);
    HoldChannel(3, HOLD_MS, 3);

    AllOff();
    LogLine("");
  }
}
