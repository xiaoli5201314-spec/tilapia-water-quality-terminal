/* 文件：test_main.c */
/* 模块：WiFi（ESP-12F / ESP8266 AT 固件） */
/* 功能：连手机热点 + 连 MQTT 服务器的最小验证。本文件夹只测 WiFi，不含其他模块。 */
/*
 * 灯语（低电平点亮）：
 *   开机            RUN/ERR 同时闪 3 次
 *   AT 通信成功      RUN 亮
 *   热点连接成功     RUN 亮 + ERR 闪 2 次
 *   全部成功         RUN 与 ERR 同时亮，之后 RUN 每 1 秒闪一下（表示还在跑）
 *   任一阶段失败     ERR 循环闪 N 次，N = 失败阶段号（1=AT, 2=热点, 3=MQTT）
 *
 * 灯对应关系：RUN = PE2 = 黄灯，ERR = PE3 = 红灯。
 *
 * 调试日志（可选）：USB-TTL 接 PD5(TX)/PD6(RX)/GND，电脑串口 115200 8N1。
 *   不需要日志时把 WIFI_TEST_DEBUG_UART 改成 0。
 */

#include "main.h"
#include "gpio.h"
#include "usart.h"
#include <string.h>
#include <stdio.h>

/* ==================== 现场参数 ==================== */
#define WIFI_SSID            "YOUR_WIFI_SSID"
#define WIFI_PASSWORD        "YOUR_WIFI_PASSWORD"

#define MQTT_HOST            "127.0.0.1"
#define MQTT_PORT            1883U
#define MQTT_USER            "YOUR_MQTT_USER"
#define MQTT_PASSWORD        "YOUR_MQTT_PASSWORD"

/* 1 = 通过 USART2(PD5/PD6) 输出调试日志；0 = 关闭 */
#define WIFI_TEST_DEBUG_UART 1U

/* ====================== 灯 ======================= */
#define LED_RUN_PORT   GPIOE
#define LED_RUN_PIN    GPIO_PIN_2
#define LED_ERR_PORT   GPIOE
#define LED_ERR_PIN    GPIO_PIN_3

static void LedRun(int on)
{
  HAL_GPIO_WritePin(LED_RUN_PORT, LED_RUN_PIN, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
}
static void LedErr(int on)
{
  HAL_GPIO_WritePin(LED_ERR_PORT, LED_ERR_PIN, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
}
static void LedBoth(int on) { LedRun(on); LedErr(on); }

/* ==================== 调试日志 ==================== */
#if (WIFI_TEST_DEBUG_UART != 0U)
static void LogStr(const char *s)
{
  if (s == NULL) { return; }
  (void)HAL_UART_Transmit(&huart2, (uint8_t *)s, (uint16_t)strlen(s), 200U);
}
static void LogLine(const char *s) { LogStr(s); LogStr("\r\n"); }
static void LogHex(const char *tag, const char *data, uint16_t length)
{
  char line[128];
  uint16_t i;
  int n;
  if (tag != NULL) { LogStr(tag); LogStr(" "); }
  for (i = 0U; i < length; ++i)
  {
    n = snprintf(line, sizeof(line), "%02X ", (unsigned char)data[i]);
    if (n <= 0) { break; }
    LogStr(line);
  }
  LogStr("\r\n");
}
#else
static void LogStr(const char *s) { (void)s; }
static void LogLine(const char *s) { (void)s; }
static void LogHex(const char *tag, const char *data, uint16_t length)
{ (void)tag; (void)data; (void)length; }
#endif

/* ================== ESP-12F 驱动 ================== */
#define RX_CAP 512U
static char rx_buf[RX_CAP];
static volatile uint16_t rx_head;
static uint8_t rx_byte;

static void EspRxClear(void)
{
  __disable_irq();
  rx_head = 0U;
  rx_buf[0] = '\0';
  __enable_irq();
}
static uint16_t EspRxLength(void)
{
  uint16_t n;
  __disable_irq();
  n = rx_head;
  __enable_irq();
  return n;
}
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    if (rx_head < (RX_CAP - 1U))
    {
      rx_buf[rx_head] = (char)rx_byte;
      rx_head++;
      rx_buf[rx_head] = '\0';
    }
    (void)HAL_UART_Receive_IT(&huart1, &rx_byte, 1U);
  }
}
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  { (void)HAL_UART_Receive_IT(&huart1, &rx_byte, 1U); }
}

static void EspSend(const char *cmd)
{
  (void)HAL_UART_Transmit(&huart1, (uint8_t *)cmd, (uint16_t)strlen(cmd), 1000U);
  (void)HAL_UART_Transmit(&huart1, (uint8_t *)"\r\n", 2U, 200U);
#if (WIFI_TEST_DEBUG_UART != 0U)
  LogStr(">> "); LogLine(cmd);
#endif
}

/* 等待候选文本出现。expect 可用 '|' 分隔多个候选，'\\r' / '\\n' 表示回车换行。 */
static int EspWaitFor(const char *expect, uint32_t timeout_ms)
{
  uint32_t start = HAL_GetTick();
  uint16_t reported = 0U;
  char needle[64];
  while ((HAL_GetTick() - start) < timeout_ms)
  {
    uint16_t len = EspRxLength();
#if (WIFI_TEST_DEBUG_UART != 0U)
    if (len > reported)
    {
      LogHex("<<", &rx_buf[reported], (uint16_t)(len - reported));
      reported = len;
    }
#endif
    if (len > 0U)
    {
      const char *p = expect;
      while (*p != '\0')
      {
        uint16_t i = 0U;
        while (*p != '\0' && *p != '|' && i < (sizeof(needle) - 1U))
        {
          if (p[0] == '\\' && p[1] == 'r') { needle[i++] = '\r'; p += 2; }
          else if (p[0] == '\\' && p[1] == 'n') { needle[i++] = '\n'; p += 2; }
          else { needle[i++] = *p++; }
        }
        needle[i] = '\0';
        if (i > 0U && strstr(rx_buf, needle) != NULL) { return 1; }
        if (*p == '|') { p++; }
      }
    }
    HAL_Delay(10U);
  }
  return 0;
}

static int EspCmd(const char *cmd, const char *expect, uint32_t timeout_ms)
{
  EspRxClear();
  EspSend(cmd);
  return EspWaitFor(expect, timeout_ms);
}

static void FatalBlink(uint8_t stage)
{
  LogStr("FATAL stage=");
  LogStr((stage == 1U) ? "1 (AT comm)" : ((stage == 2U) ? "2 (join AP)" : "3 (MQTT)"));
  LogStr("\r\n");
  for (;;)
  {
    uint8_t i;
    LedRun(0);
    for (i = 0U; i < stage; ++i) { LedErr(1); HAL_Delay(150U); LedErr(0); HAL_Delay(150U); }
    LedErr(0);
    HAL_Delay(1200U);
  }
}

/* ============== 最小系统支撑 ============== */
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
  osc.PLL.PLLM = 8U;      /* 8MHz / 8 = 1MHz   */
  osc.PLL.PLLN = 336U;    /* 1MHz * 336        */
  osc.PLL.PLLP = RCC_PLLP_DIV2;  /* 336 / 2 = 168MHz */
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
  __disable_irq();
  for (;;)
  {
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_2 | GPIO_PIN_3, GPIO_PIN_RESET);
    HAL_Delay(100U);
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_2 | GPIO_PIN_3, GPIO_PIN_SET);
    HAL_Delay(100U);
  }
}

/* ==================== 主程序 ==================== */
int main(void)
{
  uint8_t i;
  int ok;
  char cmd[256];
  int n;

  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();   /* 只用于调试日志 */

  LogStr("\r\n\r\n=== XIAOLI WIFI TEST ===\r\n");

  for (i = 0U; i < 3U; ++i) { LedBoth(1); HAL_Delay(150U); LedBoth(0); HAL_Delay(150U); }

  (void)HAL_UART_Receive_IT(&huart1, &rx_byte, 1U);
  HAL_Delay(1500U);   /* 等 ESP-12F 上电稳定 */

  /* ---------- 阶段 1：AT 通信 ---------- */
  LogLine("[1] AT handshake");
  ok = 0;
  for (i = 0U; i < 3U && ok == 0; ++i)
  {
    ok = EspCmd("AT", "OK", 3000U);
    if (ok == 0) { HAL_Delay(500U); }
  }
  if (ok == 0) { FatalBlink(1U); }
  LedRun(1);
  LogLine("[1] PASS");

  (void)EspCmd("ATE0", "OK", 1500U);
  (void)EspCmd("AT+GMR", "OK", 3000U);

  /* ---------- 阶段 2：连热点 ---------- */
  LogLine("[2] join AP");
  if (!EspCmd("AT+CWMODE=1", "OK", 3000U)) { FatalBlink(1U); }

  n = snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", WIFI_SSID, WIFI_PASSWORD);
  if (n <= 0 || (size_t)n >= sizeof(cmd)) { FatalBlink(2U); }
  if (!EspCmd(cmd, "OK|FAIL|ERROR", 30000U) || strstr(rx_buf, "OK") == NULL)
  {
    LogHex("<join fail", rx_buf, EspRxLength());
    FatalBlink(2U);
  }
  LedErr(1); HAL_Delay(120U); LedErr(0); HAL_Delay(120U);
  LedErr(1); HAL_Delay(120U); LedErr(0);
  LogLine("[2] PASS");

  /* ---------- 阶段 3：连 MQTT ---------- */
  LogLine("[3] mqtt connect");
  n = snprintf(cmd, sizeof(cmd), "AT+MQTTUSERCFG=0,1,\"fish-%08lX%08lX%08lX-w\",\"%s\",\"%s\",0,0,\"\"",
    (unsigned long)HAL_GetUIDw0(), (unsigned long)HAL_GetUIDw1(),
    (unsigned long)HAL_GetUIDw2(), MQTT_USER, MQTT_PASSWORD);
  if (n <= 0 || (size_t)n >= sizeof(cmd)) { FatalBlink(3U); }
  if (!EspCmd(cmd, "OK|ERROR", 3000U) || strstr(rx_buf, "OK") == NULL)
  {
    LogHex("<usercfg fail", rx_buf, EspRxLength());
    FatalBlink(3U);
  }

  n = snprintf(cmd, sizeof(cmd), "AT+MQTTCONN=0,\"%s\",%u,0", MQTT_HOST, (unsigned)MQTT_PORT);
  if (n <= 0 || (size_t)n >= sizeof(cmd)) { FatalBlink(3U); }
  if (!EspCmd(cmd, "OK|ERROR|FAIL", 15000U) || strstr(rx_buf, "OK") == NULL)
  {
    LogHex("<conn fail", rx_buf, EspRxLength());
    FatalBlink(3U);
  }
  /* 查连接状态：+MQTTCONN:0,<state>,...  state 4/5/6 表示已连接 */
  (void)EspCmd("AT+MQTTCONN?", "OK", 3000U);
  if (strstr(rx_buf, ",4,") == NULL && strstr(rx_buf, ",5,") == NULL &&
      strstr(rx_buf, ",6,") == NULL)
  {
    LogHex("<state fail", rx_buf, EspRxLength());
    FatalBlink(3U);
  }

  /* ---------- 成功 ---------- */
  LogLine("[3] PASS - ALL OK");
  LedRun(1);
  LedErr(1);
  for (;;)
  {
    HAL_Delay(1000U);
    LedRun(0); HAL_Delay(80U); LedRun(1);
  }
}
