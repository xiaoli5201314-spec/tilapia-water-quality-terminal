"""功能：验证新板命令校验、真实设备确认和实时状态，不连接现场设备。"""
import json
import tempfile
import unittest
from pathlib import Path


class ProtocolTests(unittest.TestCase):
    def test_motion_limits_and_strict_types(self):
        from control_core import validate_command, CommandError
        self.assertEqual(
            validate_command({"op": "brush", "action": "start", "duration": 10}),
            {"op": "brush", "action": "start", "duration": 10},
        )
        for body in [
            {"op": "brush", "action": "start", "duration": 31},
            {"op": "brush", "action": "start", "duration": True},
            {"op": "actuator", "action": "extend", "duration": 0},
            {"op": "mode", "mode": "anything"},
            {"op": "stop", "id": "injected"},
            {"op": "sampling", "mode": "schedule", "start": 480, "end": 480,
             "sample": 5, "publish": 60},
        ]:
            with self.subTest(body=body), self.assertRaises(CommandError):
                validate_command(body)

    def test_cross_midnight_and_disabled_plan(self):
        from control_core import validate_command
        body = {"op": "sampling", "mode": "schedule", "start": 1380, "end": 120,
                "sample": 5, "publish": 3600}
        self.assertEqual(validate_command(body), body)
        self.assertEqual(
            validate_command({"op": "plan", "target": "brush", "type": "off",
                              "start": 480, "interval": 21600, "duration": 10,
                              "action": "start"})["type"], "off"
        )


class StateTests(unittest.TestCase):
    def setUp(self):
        from control_core import ControlStore
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.monotonic = 100.0
        self.sequence = 0
        self.store = ControlStore(
            Path(self.temp.name) / "test.db",
            "000000000000000000000001",
            monotonic=lambda: self.monotonic,
        )
        self.addCleanup(self.store.close)

    def status_payload(self, **overrides):
        return json.dumps({
            "v": 1, "deviceId": self.store.device_id, "online": True,
            "bootId": "first", "clock": {"synced": True, "unix": 1791770000},
            "controlMode": "manual", "light": "green",
            "uptimeMs": 1000,
            "collection": {"mode": "run", "active": True, "start": 480, "end": 1080,
                           "sample": 5, "publish": 3600},
            "brush": {"running": False, "remaining": 0},
            "actuator": {"state": "stopped", "remaining": 0},
            "cleaning": {"state": "idle", "nextIn": 600},
            "ack": {"id": "", "result": "ok", "message": ""},
            "brushPlan": {"type": "off", "start": 480, "interval": 21600, "duration": 10, "action": "start"},
            "actuatorPlan": {"type": "off", "start": 480, "interval": 21600, "duration": 12, "action": "extend"},
            "cleaningConfig": {"enabled": True, "interval": 21600, "extend": 12,
                               "brush": 10, "retract": 12, "settle": 30},
            **overrides,
        }).encode()

    def receive_status(self, retained=False, **overrides):
        self.sequence += 1
        for part in range(4):
            self.store.receive(self.store.status_topic,
                               self.fragment(part, self.sequence, **overrides), retained)

    def connect_live(self):
        self.store.set_mqtt(True)
        self.receive_status()

    def test_retained_status_never_enables_commands(self):
        self.store.set_mqtt(True)
        self.receive_status(retained=True)
        status = self.store.snapshot()
        self.assertFalse(status["device"]["controlAvailable"])
        self.assertIsNone(status["device"]["online"])
        self.assertIsNone(status["device"]["state"])

    def test_live_state_expires_independently_of_hourly_reading(self):
        self.connect_live()
        self.assertTrue(self.store.snapshot()["device"]["controlAvailable"])
        self.monotonic += 21
        self.assertFalse(self.store.snapshot()["device"]["controlAvailable"])
        self.assertFalse(self.store.snapshot()["device"]["online"])

    def test_disconnect_and_reconnect_requires_new_live_state(self):
        self.connect_live()
        self.store.set_mqtt(False)
        self.store.set_mqtt(True)
        self.assertFalse(self.store.snapshot()["device"]["controlAvailable"])
        self.receive_status()
        self.assertTrue(self.store.snapshot()["device"]["controlAvailable"])

    def test_only_matching_live_ack_confirms(self):
        self.connect_live()
        command, wire = self.store.prepare_command({"op": "stop"})
        self.assertEqual(command["state"], "pending")
        self.assertEqual(wire["ttl"], 30)
        ack = {"id": command["id"], "result": "ok", "message": "stopped"}
        self.receive_status(retained=True, ack=ack)
        self.assertEqual(self.store.snapshot()["commands"][0]["state"], "pending")
        self.receive_status(bootId="second", ack=ack)
        self.assertNotEqual(self.store.snapshot()["commands"][0]["state"], "confirmed")

    def test_valid_ack_and_rejection(self):
        self.connect_live()
        command, _ = self.store.prepare_command({"op": "mode", "mode": "manual"})
        self.receive_status(ack={"id": command["id"], "result": "ok", "message": ""})
        self.assertEqual(self.store.snapshot()["commands"][0]["state"], "confirmed")
        command, _ = self.store.prepare_command({"op": "brush", "action": "start", "duration": 10})
        self.receive_status(ack={"id": command["id"], "result": "rejected", "message": "busy"})
        self.assertEqual(self.store.snapshot()["commands"][0]["state"], "rejected")
        self.assertIn("忙", self.store.snapshot()["commands"][0]["message"])

    def test_expired_command_is_not_confirmed_by_late_ack(self):
        self.connect_live()
        command, _ = self.store.prepare_command({"op": "stop"})
        self.monotonic += 31
        self.assertEqual(self.store.snapshot()["commands"][0]["state"], "expired")
        self.receive_status(ack={"id": command["id"], "result": "ok", "message": ""})
        self.assertEqual(self.store.snapshot()["commands"][0]["state"], "expired")

    def test_zero_is_valid_retained_is_not_new_history(self):
        payload = b'{"ph":0,"doMgL":0,"waterTemp":0,"doSaturation":0}'
        self.store.receive(self.store.latest_topic, payload, True)
        self.assertEqual(self.store.snapshot()["water"]["values"]["ph"], 0)
        self.assertEqual(len(self.store.history()), 0)
        self.store.receive(self.store.latest_topic, payload, False)
        self.assertEqual(len(self.store.history()), 1)

    def test_wrong_device_and_nonfinite_values_are_ignored(self):
        self.store.receive(self.store.status_topic, self.status_payload(deviceId="wrong"), False)
        self.assertFalse(self.store.snapshot()["device"]["controlAvailable"])
        self.store.receive(self.store.latest_topic, b'{"ph":NaN,"doMgL":1,"waterTemp":2,"doSaturation":3}', False)
        self.assertIsNone(self.store.snapshot()["water"]["values"])

    def fragment(self, part, seq=1, **overrides):
        from control_core import STATE_GROUPS
        data = json.loads(self.status_payload(**overrides))
        packet = {key: data[key] for key in ("v", "deviceId", "online", "bootId")}
        packet.update(part=part, parts=4, seq=seq, **{key: data[key] for key in STATE_GROUPS[part]})
        return json.dumps(packet).encode()

    def test_only_complete_same_frame_enables_control(self):
        self.store.set_mqtt(True)
        for part in (0, 1, 2):
            self.store.receive(self.store.status_topic, self.fragment(part), False)
        self.assertFalse(self.store.snapshot()["device"]["controlAvailable"])
        self.store.receive(self.store.status_topic, self.fragment(3), False)
        status = self.store.snapshot()
        self.assertTrue(status["device"]["controlAvailable"])
        self.assertIn("brushPlan", status["device"]["state"])
        self.assertIn("collection", status["device"]["state"])

    def test_retained_and_mismatched_fragments_cannot_complete_live_frame(self):
        self.store.set_mqtt(True)
        self.store.receive(self.store.status_topic, self.fragment(3), True)
        for part in (0, 1, 2):
            self.store.receive(self.store.status_topic, self.fragment(part, 2), False)
        self.assertFalse(self.store.snapshot()["device"]["controlAvailable"])
        self.store.receive(self.store.status_topic, self.fragment(3, 3), False)
        self.assertFalse(self.store.snapshot()["device"]["controlAvailable"])

    def test_live_frame_with_two_second_publish_spacing_enables_control(self):
        # 实机四片约每两秒到达，完整快照仍须同序号且四片齐全。
        self.store.set_mqtt(True)
        for part in range(4):
            self.monotonic = 100.0 + part * 2.0
            self.store.receive(self.store.status_topic, self.fragment(part), False)
            if part < 3:
                self.assertFalse(self.store.snapshot()["device"]["controlAvailable"])
        self.assertTrue(self.store.snapshot()["device"]["controlAvailable"])
        self.assertEqual(self.store.snapshot()["device"]["state"]["seq"], 1)

    def test_incomplete_frames_expire_and_duplicate_frame_cannot_refresh(self):
        self.store.set_mqtt(True)
        self.store.receive(self.store.status_topic, self.fragment(0), False)
        self.monotonic += 11
        for part in (1, 2, 3):
            self.store.receive(self.store.status_topic, self.fragment(part), False)
        self.assertFalse(self.store.snapshot()["device"]["controlAvailable"])
        for part in range(4):
            self.store.receive(self.store.status_topic, self.fragment(part, 2), False)
        self.assertTrue(self.store.snapshot()["device"]["controlAvailable"])
        self.monotonic += 21
        for part in range(4):
            self.store.receive(self.store.status_topic, self.fragment(part, 2), False)
        self.assertFalse(self.store.snapshot()["device"]["controlAvailable"])


class ApiTests(unittest.TestCase):
    def setUp(self):
        from app import create_app
        from control_core import ControlStore
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.store = ControlStore(Path(self.temp.name) / "test.db", "000000000000000000000001")
        self.addCleanup(self.store.close)
        self.sent = []

        class FakeTransport:
            def send(inner, wire):
                self.sent.append(wire)
        self.app = create_app(self.store, FakeTransport())
        self.client = self.app.test_client()

    def test_api_refuses_offline_mechanical_start(self):
        response = self.client.post("/api/command", json={"op": "brush", "action": "start", "duration": 10})
        self.assertEqual(response.status_code, 409)
        self.assertEqual(self.sent, [])

    def test_foreign_origin_cannot_control_local_device(self):
        response = self.client.post("/api/command", json={"op": "stop"},
                                    headers={"Origin": "https://unrelated.example"})
        self.assertEqual(response.status_code, 403)
        self.assertEqual(self.sent, [])

    def test_nonlocal_host_cannot_rebind_controller(self):
        response = self.client.post("/api/command", json={"op": "stop"},
                                    headers={"Host": "unrelated.example", "Origin": "http://unrelated.example"})
        self.assertEqual(response.status_code, 403)
        self.assertEqual(self.sent, [])

    def test_public_status_does_not_expose_password(self):
        response = self.client.get("/api/status")
        self.assertEqual(response.status_code, 200)
        self.assertNotIn("YOUR_MQTT_PASSWORD", response.get_data(as_text=True))
        self.assertNotIn("YOUR_WIFI_PASSWORD", response.get_data(as_text=True))

    def test_csv_is_excel_utf8_and_contains_actual_zero(self):
        self.store.receive(self.store.latest_topic, b'{"ph":0,"doMgL":0,"waterTemp":0,"doSaturation":0}', False)
        response = self.client.get("/api/export.csv")
        self.assertEqual(response.status_code, 200)
        self.assertTrue(response.data.startswith(b"\xef\xbb\xbf"))
        self.assertIn("水温".encode(), response.data)


class TransportTests(unittest.TestCase):
    def test_command_is_not_retained_and_is_qos_zero(self):
        from types import SimpleNamespace
        from mqtt_service import MqttService
        calls = []

        class Client:
            def is_connected(self):
                return True
            def publish(self, topic, payload, **options):
                calls.append((topic, json.loads(payload), options))
                return SimpleNamespace(rc=0)
        service = object.__new__(MqttService)
        service.client = Client()
        service.store = SimpleNamespace(command_topic="fish/device/test/water/cmd")
        service.send({"v": 1, "op": "stop", "id": "abc", "issued": 123, "ttl": 30})
        self.assertEqual(calls[0][2], {"qos": 0, "retain": False})

    def test_disconnected_transport_does_not_enqueue(self):
        from mqtt_service import MqttService

        class Client:
            def is_connected(self):
                return False
            def publish(self, *args, **kwargs):
                self.fail("Disconnected commands must not be published")
        service = object.__new__(MqttService)
        service.client = Client()
        with self.assertRaises(ConnectionError):
            service.send({"op": "stop"})


if __name__ == "__main__":
    unittest.main()
