/* 文件：test_main.c */
/* 模块：MQTT 发布链路定位（最小命令版，不依赖串口，用灯的闪法报错） */
/*
 * 背景：上一版诊断消息发不出去，怀疑是 AT+MQTTPUB 命令太长
 *       （ESP-AT 规定单条 AT 命令总长 <= 256 字节，而诊断 JSON 有 300+ 字节）。
 *
 * 本版策略：把命令压到最短，分阶段验证，每一步都用【灯的闪法】告诉你结果，
 *           不需要 USB-TTL，上电看灯就知道卡在哪一步。
 *
 * ── 三个阶段，每次上电推进一个（掉电重启就进下一阶段）──
 * 不行——掉电不能保存状态。所以本版一次把三个阶段全做完：
 *
 *   阶段 A：连热点 + 连 MQTT
 *   阶段 B：用【最短】命令发一条
 *            主题 fish/device/<UID>/t
 *             如果 UID 主题失败，再试主题 "t"（无斜杠，最短）
 *   阶段 C：循环，每隔 5 秒发一条心跳（各主题轮着发）
 *
 * ── 灯语（红灯闪几下 = 哪一步的结果；闪完停 2 秒再来一轮）──
 *
 *   红灯 闪 1 下 = AT 不通
 *   红灯 闪 2 下 = 连不上热点
 *   红灯 闪 3 下 = MQTT 连接失败
 *   红灯 闪 4 下 = MQTT 连上了，但发长主题失败
 *   红灯 闪 5 下 = MQTT 连上了，但发短主题也失败
 *   红灯 闪 6 下 = 全部成功（每次成功发布后，黄灯常亮 1 秒）
 *
 *   红灯 常亮不动 = 死在第 1~3 步（网络）
 *   红灯 慢闪 + 黄灯常亮 1 秒 = 成功在发
 *
 * ── 服务器上应该看到什么 ──
 *   主题（两个都会收到，只要命令能发出去）：
 *     t
 *     fish/device/<UID>/t
 *   载荷（26 字节，非常短）：
 *     {"src":"stm32","n":1}
 */

#include "main.h"
#include "gpio.h"
#include "usart.h"
#include <string.h>
#include <stdio.h>

#define WIFI_SSID      "YOUR_WIFI_SSID"
#define WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"

#define MQTT_HOST      "127.0.0.1"
#define MQTT_PORT      1883U
#define MQTT_USER      "YOUR_MQTT_USER"
#define MQTT_PASSWORD  "YOUR_MQTT_PASSWORD"

#define SEND_SECONDS   5U

#define LED_RUN_PORT   GPIOE
#define LED_RUN_PIN    GPIO_PIN_2
#define LED_ERR_PORT   GPIOE
#define LED_ERR_PIN    GPIO_PIN_3

static void LedRun(int on)
{ HAL_GPIO_WritePin(LED_RUN_PORT, LED_RUN_PIN, on ? GPIO_PIN_RESET : GPIO_PIN_SET); }
static void LedErr(int on)
{ HAL_GPIO_WritePin(LED_ERR_PORT, LED_ERR_PIN, on ? GPIO_PIN_RESET : GPIO_PIN_SET); }

/* 红灯闪 n 下（n=1..6），然后停 2 秒；用于不接串口时报错定位 */
static void BlinkCode(uint8_t n)
{
  uint8_t i;
  for (;;)
  {
    LedErr(0); LedRun(0);
    HAL_Delay(1200U);
    for (i = 0U; i < n; ++i)
    {
      LedErr(1); HAL_Delay(180U);
      LedErr(0); HAL_Delay(180U);
    }
  }
}

/* ---------------- 串口接收 ---------------- */
#define RX_CAP 512U
static char     wifi_buf[RX_CAP];
static volatile uint16_t wifi_head;
static uint8_t  wifi_byte;

static void WifiRxClear(void)
{ __disable_irq(); wifi_head = 0U; wifi_buf[0] = '\0'; __enable_irq(); }
static uint16_t WifiRxLen(void)
{ uint16_t n; __disable_irq(); n = wifi_head; __enable_irq(); return n; }

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    if (wifi_head < (RX_CAP - 1U))
    { wifi_buf[wifi_head] = (char)wifi_byte; wifi_head++; wifi_buf[wifi_head] = '\0'; }
    (void)HAL_UART_Receive_IT(&huart1, &wifi_byte, 1U);
  }
}
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1) { (void)HAL_UART_Receive_IT(&huart1, &wifi_byte, 1U); }
}

static int EspWaitFor(const char *expect, uint32_t timeout_ms)
{
  uint32_t start = HAL_GetTick();
  char needle[48];
  while ((HAL_GetTick() - start) < timeout_ms)
  {
    if (WifiRxLen() > 0U)
    {
      const char *p = expect;
      while (*p != '\0')
      {
        uint16_t i = 0U;
        while (*p != '\0' && *p != '|' && i < (sizeof(needle) - 1U)) { needle[i++] = *p++; }
        needle[i] = '\0';
        if (i > 0U && strstr(wifi_buf, needle) != NULL) { return 1; }
        if (*p == '|') { p++; }
      }
    }
    HAL_Delay(5U);
  }
  return 0;
}

static int EspCmd(const char *cmd, const char *expect, uint32_t timeout_ms)
{
  WifiRxClear();
  (void)HAL_UART_Transmit(&huart1, (uint8_t *)cmd, (uint16_t)strlen(cmd), 2000U);
  (void)HAL_UART_Transmit(&huart1, (uint8_t *)"\r\n", 2U, 200U);
  return EspWaitFor(expect, timeout_ms);
}

/* 发布一条；返回 1 = 模组回了 OK
 *
 * ★ 关键：JSON 载荷里的双引号必须转义成 \" 再放进 AT 命令，
 *   否则 AT 解析器会在第一个内嵌引号处断开，直接返回 ERROR。
 *   例如 {"src":"stm32"} 必须变成 {\"src\":\"stm32\"}。
 *   乐鑫 ESP-AT 手册：【消息中内嵌的双引号请用 \22 表达，
 *   如果是 MCU 发消息，可能需要用 \\22 来表达】——即 AT 层面写 \"。
 */
static int EspPub(const char *topic, const char *payload)
{
  char cmd[320];
  size_t used = 0U;
  const char *p;

  /* 前缀：AT+MQTTPUB=0,"<topic>"," */
  used = (size_t)snprintf(cmd, sizeof(cmd), "AT+MQTTPUB=0,\"%s\",\"", topic);
  if (used == 0U || used >= sizeof(cmd)) { return -1; }

  /* 逐字符追加载荷，把 " 转义成 \" */
  for (p = payload; *p != '\0'; ++p)
  {
    if (*p == '"')
    {
      if (used + 3U >= sizeof(cmd)) { return -1; }
      cmd[used++] = '\\';
      cmd[used++] = '"';
    }
    else
    {
      if (used + 2U >= sizeof(cmd)) { return -1; }
      cmd[used++] = *p;
    }
  }
  cmd[used] = '\0';

  /* 后缀：",0,1 */
  if (used + 6U >= sizeof(cmd)) { return -1; }
  (void)snprintf(cmd + used, sizeof(cmd) - used, "\",0,1");

  LedRun(1);
  {
    int r = EspCmd(cmd, "OK|ERROR|FAIL", 5000U);
    LedRun(0);
    if (r && strstr(wifi_buf, "OK") != NULL) { return 1; }
    return 0;
  }
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
  osc.PLL.PLLM = 8U; osc.PLL.PLLN = 336U; osc.PLL.PLLP = RCC_PLLP_DIV2; osc.PLL.PLLQ = 7U;
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
  __disable_irq();
  for (;;)
  {
    HAL_GPIO_WritePin(LED_RUN_PORT, LED_RUN_PIN | LED_ERR_PIN, GPIO_PIN_RESET);
    HAL_Delay(100U);
    HAL_GPIO_WritePin(LED_RUN_PORT, LED_RUN_PIN | LED_ERR_PIN, GPIO_PIN_SET);
    HAL_Delay(100U);
  }
}

int main(void)
{
  uint8_t i;
  char cmd[256];
  char topic_long[96];
  char payload[48];
  int ok;
  uint32_t n_sent = 0U;
  uint32_t t_next;

  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_USART1_UART_Init();   /* WiFi */
  /* 不初始化 USART2/USART3：本版不需要串口 */

  (void)HAL_UART_Receive_IT(&huart1, &wifi_byte, 1U);

  /* 开机两灯同闪 2 次 */
  for (i = 0U; i < 2U; ++i)
  { LedRun(1); LedErr(1); HAL_Delay(200U); LedRun(0); LedErr(0); HAL_Delay(200U); }

  /* ---------- 阶段 A：AT ---------- */
  HAL_Delay(1500U);
  ok = 0;
  for (i = 0U; i < 3U && ok == 0; ++i)
  { ok = EspCmd("AT", "OK", 3000U); if (!ok) { HAL_Delay(500U); } }
  if (!ok) { BlinkCode(1U); }              /* 红灯闪 1 下 = AT 不通 */

  (void)EspCmd("ATE0", "OK", 1500U);
  if (!EspCmd("AT+CWMODE=1", "OK", 3000U)) { BlinkCode(1U); }

  /* ---------- 阶段 B：连热点 ---------- */
  (void)snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", WIFI_SSID, WIFI_PASSWORD);
  if (!EspCmd(cmd, "OK|FAIL|ERROR", 30000U) || strstr(wifi_buf, "OK") == NULL)
  { BlinkCode(2U); }                       /* 红灯闪 2 下 = 连不上热点 */

  /* ---------- 阶段 C：连 MQTT ---------- */
  (void)snprintf(cmd, sizeof(cmd), "AT+MQTTUSERCFG=0,1,\"fw-%08lX\",\"%s\",\"%s\",0,0,\"\"",
    (unsigned long)HAL_GetUIDw0(), MQTT_USER, MQTT_PASSWORD);
  if (!EspCmd(cmd, "OK|ERROR", 3000U) || strstr(wifi_buf, "OK") == NULL)
  { BlinkCode(3U); }

  (void)snprintf(cmd, sizeof(cmd), "AT+MQTTCONN=0,\"%s\",%u,0", MQTT_HOST, (unsigned)MQTT_PORT);
  if (!EspCmd(cmd, "OK|ERROR|FAIL", 15000U) || strstr(wifi_buf, "OK") == NULL)
  { BlinkCode(3U); }                       /* 红灯闪 3 下 = MQTT 连接失败 */

  (void)EspCmd("AT+MQTTCONN?", "OK", 3000U);
  if (strstr(wifi_buf, ",4,") == NULL && strstr(wifi_buf, ",5,") == NULL &&
      strstr(wifi_buf, ",6,") == NULL)
  { BlinkCode(3U); }

  /* ---------- 阶段 D：先试短主题 ---------- */
  if (EspPub("t", "{\"src\":\"stm32\"}") != 1)
  { BlinkCode(5U); }                       /* 红灯闪 5 下 = 连短主题都发不出去 */

  /* ---------- 阶段 E：再试长主题 ---------- */
  (void)snprintf(topic_long, sizeof(topic_long), "fish/device/%08lX%08lX%08lX/t",
    (unsigned long)HAL_GetUIDw0(), (unsigned long)HAL_GetUIDw1(), (unsigned long)HAL_GetUIDw2());
  if (EspPub(topic_long, "{\"src\":\"stm32\"}") != 1)
  { BlinkCode(4U); }                       /* 红灯闪 4 下 = 长主题发不出去 */

  /* ---------- 阶段 F：两个主题都通了，循环发心跳 ---------- */
  t_next = HAL_GetTick();
  for (;;)
  {
    /* 红灯慢闪做心跳 */
    LedErr((((HAL_GetTick() / 1000U) % 4U) == 0U) ? 1 : 0);

    if ((int32_t)(HAL_GetTick() - t_next) < 0) { HAL_Delay(20U); continue; }
    t_next = HAL_GetTick() + (SEND_SECONDS * 1000U);
    n_sent++;

    (void)snprintf(payload, sizeof(payload), "{\"src\":\"stm32\",\"n\":%lu}",
      (unsigned long)n_sent);
    (void)EspPub("t", payload);
    (void)EspPub(topic_long, payload);
  }
}
