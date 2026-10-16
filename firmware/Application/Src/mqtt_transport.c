/* 文件：mqtt_transport.c */
/* 功能：Wi-Fi MQTT RAW 发布，校验 MQTT 包预算并等待真正发布成功。 */
#include "mqtt_transport.h"
#include "network_manager.h"
#include "network_config.h"
#include "board_config.h"
#include "at_command.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
static char command[192];
/* Topic 只接受设备路径字符，禁止引号、换行与 AT 注入。 */
static bool ValidTopic(const char *topic)
{
  size_t i, length;
  if (topic==NULL) { return false; }
  length=strlen(topic);
  if (length==0U || length>128U) { return false; }
  for (i=0U;i<length;++i)
  {
    char c=topic[i];
    if (!((c>='a'&&c<='z') || (c>='A'&&c<='Z') ||
          (c>='0'&&c<='9') || c=='/' || c=='_' || c=='-')) { return false; }
  }
  return true;
}
/* 发布层不自行连接，4G 在该独立工程中禁用。 */
void MqttTransport_Init(void) {}
/* 原四字段与四个状态分片均采用 QoS0/retain1 RAW，绝不字符串转义。 */
MqttResult MqttTransport_PublishRaw(MqttLink link, const char *topic,
  const uint8_t *payload, uint16_t length)
{
  HAL_StatusTypeDef result;
  int n;
  if ((link!=MQTT_LINK_WIFI && link!=MQTT_LINK_4G) || !ValidTopic(topic) ||
      payload==NULL || length==0U || (size_t)length+strlen(topic)+8U>512U)
  { return MQTT_RESULT_INVALID_ARGUMENT; }
  if (link!=MQTT_LINK_WIFI || link!=NetworkManager_GetActiveLink())
  { return MQTT_RESULT_NOT_CONNECTED; }
  n=snprintf(command,sizeof(command),"AT+MQTTPUBRAW=0,\"%s\",%u,0,1",topic,(unsigned)length);
  if (n<=0 || (size_t)n>=sizeof(command)) { return MQTT_RESULT_INVALID_ARGUMENT; }
  result=AtCommand_PublishRaw(BOARD_WIFI_UART,command,payload,length,
    WATER_MQTT_PUBLISH_TIMEOUT_MS);
  if (result==HAL_OK) { return MQTT_RESULT_OK; }
  if (result!=HAL_BUSY) { NetworkManager_ReportPublishFailure(link); }
  return MQTT_RESULT_NOT_CONNECTED;
}
/* 保持原水质发布接口，内部全部走 RAW 流程。 */
MqttResult MqttTransport_Publish(MqttLink link, const char *topic,
  const uint8_t *payload, uint16_t length)
{ return MqttTransport_PublishRaw(link,topic,payload,length); }
