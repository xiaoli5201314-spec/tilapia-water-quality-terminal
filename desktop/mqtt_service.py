"""功能：连接配置的 MQTT 服务器、订阅新板数据/状态，发送不保留且不重试的控制命令。"""
import json
import logging
import os
import sqlite3
import uuid
from paho.mqtt import client as mqtt


class MqttService:
    def __init__(self, store):
        self.store = store
        self.host = os.environ.get("WATER_MQTT_HOST", "127.0.0.1")
        self.port = int(os.environ.get("WATER_MQTT_PORT", "1883"))
        self.client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id="water-control-" + uuid.uuid4().hex[:12],
            clean_session=True,
        )
        self.client.username_pw_set(
            os.environ.get("WATER_MQTT_USER", "YOUR_MQTT_USER"),
            os.environ.get("WATER_MQTT_PASSWORD", "YOUR_MQTT_PASSWORD"),
        )
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message = self._on_message
        self.client.on_connect_fail = self._on_connect_fail
        self.client.reconnect_delay_set(min_delay=2, max_delay=15)
        # 最大待发送队列只作资源限制；业务层不会在断网时入队或重新发布。
        self.client.max_queued_messages_set(1)
        self.store.mqtt["broker"] = f"{self.host}:{self.port}"

    def _on_connect(self, client, userdata, flags, reason_code, properties):
        if reason_code.is_failure:
            self.store.set_mqtt(False, f"MQTT 连接失败：{reason_code}")
            return
        result, _ = client.subscribe(
            [(self.store.latest_topic, 0), (self.store.status_topic, 0)]
        )
        self.store.set_mqtt(result == mqtt.MQTT_ERR_SUCCESS,
                            "" if result == mqtt.MQTT_ERR_SUCCESS else "订阅失败")

    def _on_disconnect(self, client, userdata, flags, reason_code, properties):
        self.store.set_mqtt(False, "MQTT 连接中断，正在重连")

    def _on_connect_fail(self, client, userdata):
        self.store.set_mqtt(False, "无法连接服务器，正在重试")

    def _on_message(self, client, userdata, message):
        try:
            self.store.receive(message.topic, message.payload, message.retain)
        except (ValueError, TypeError, OverflowError, RecursionError, sqlite3.Error):
            # 畸形报文或暂时存储异常不能终止 Paho 接收与重连线程。
            logging.getLogger(__name__).warning("MQTT message could not be processed", exc_info=True)

    def start(self):
        self.client.connect_async(self.host, self.port, keepalive=15)
        self.client.loop_start()

    def stop(self):
        self.client.disconnect()
        self.client.loop_stop()

    def send(self, wire):
        if not self.client.is_connected():
            raise ConnectionError("MQTT 已断开，命令没有发送")
        # QoS 0、不保留、不调用重发；结果由设备控制状态的对应 ack 确认。
        info = self.client.publish(
            self.store.command_topic, json.dumps(wire, separators=(",", ":")),
            qos=0, retain=False,
        )
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            raise ConnectionError("命令发送失败，未自动重试")
