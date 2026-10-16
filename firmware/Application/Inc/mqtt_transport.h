/* 文件：mqtt_transport.h */
/* 功能：根据当前活动链路发布水质 MQTT 消息。 */
/* 单一活动链路发布 MQTT；发布命令因模组而异。 */
#ifndef __MQTT_TRANSPORT_H__
#define __MQTT_TRANSPORT_H__
#include <stdint.h>
/* 通道标识只用于业务选择，枚举值本身不能证明该通道已经连接。 */
typedef enum { MQTT_LINK_NONE = 0, MQTT_LINK_WIFI, MQTT_LINK_4G } MqttLink;
/* NOT_CONFIGURED 保留兼容旧接口，未连接或 AT 失败返回 NOT_CONNECTED。 */
typedef enum { MQTT_RESULT_OK = 0, MQTT_RESULT_NOT_CONFIGURED,
               MQTT_RESULT_INVALID_ARGUMENT, MQTT_RESULT_NOT_CONNECTED } MqttResult;
/** 初始化发布适配层，不会自行连接服务器。 */
void MqttTransport_Init(void);
/**
 * @brief 在当前已确认的 Wi-Fi 或 4G 链路上发布 QoS 0、Retain 1 消息。
 * @param link 目标 Wi-Fi 或 4G 通道，不接受 MQTT_LINK_NONE。
 * @param topic 非空主题字符串；payload 为非空负载；payload_length 为负载字节数且大于 0。
 * @return 参数无效返回 INVALID_ARGUMENT；无活动链路或命令失败返回 NOT_CONNECTED。
 * @note OK 只说明模组已接受 QoS 0 发布命令，不代表服务器已永久存储。
 */
MqttResult MqttTransport_Publish(MqttLink link, const char *topic,
                               const uint8_t *payload, uint16_t payload_length);
/* 发布原始字节，限制 payload+topic+8<=512，并等待 +MQTTPUB:OK。 */
MqttResult MqttTransport_PublishRaw(MqttLink link, const char *topic,
                                 const uint8_t *payload, uint16_t payload_length);
#endif
