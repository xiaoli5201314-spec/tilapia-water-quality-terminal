"""功能：验证审查发现的发布顺序、异常报文、完整状态和命令结果保留。"""
import json
import tempfile
import unittest
from pathlib import Path
from app import create_app
from control_core import CommandError, ControlStore


class RegressionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.tick = 100.0
        self.store = ControlStore(Path(self.temp.name) / "test.db", "000000000000000000000001",
                                  monotonic=lambda: self.tick)
        self.addCleanup(self.store.close)
        self.seq = 0
        self.sent = []
        self.store.set_mqtt(True)
        self.frame()

        class FakeTransport:
            def send(inner, wire):
                self.sent.append(wire)
        self.app = create_app(self.store, FakeTransport())

    def frame(self, **overrides):
        self.seq += 1
        state = {
            "v": 1, "online": True, "deviceId": self.store.device_id, "bootId": "12345678",
            "controlMode": "manual", "collection": {"mode": "run", "active": True,
                "start": 480, "end": 1080, "sample": 5, "publish": 3600},
            "clock": {"synced": True, "unix": 1791770000}, "light": "green", "uptimeMs": 1000,
            "brush": {"running": False, "remaining": 0},
            "actuator": {"state": "stopped", "remaining": 0},
            "cleaning": {"state": "idle", "nextIn": 600},
            "ack": {"id": "", "result": "ok", "message": ""},
            "brushPlan": {"type": "off", "start": 480, "interval": 21600, "duration": 10, "action": "start"},
            "actuatorPlan": {"type": "off", "start": 480, "interval": 21600, "duration": 12, "action": "extend"},
            "cleaningConfig": {"enabled": True, "interval": 21600, "extend": 12, "brush": 10, "retract": 12, "settle": 30},
            **overrides,
        }
        for part, fields in enumerate((
            ("controlMode", "collection", "clock", "light", "uptimeMs"),
            ("brush", "actuator", "cleaning", "ack"), ("brushPlan", "actuatorPlan"), ("cleaningConfig",)
        )):
            body = {key: state[key] for key in ("v", "online", "deviceId", "bootId")}
            body.update(seq=self.seq, part=part, parts=4, **{key: state[key] for key in fields})
            self.store.receive(self.store.status_topic, json.dumps(body).encode(), False)

    def test_delayed_start_cannot_follow_stop(self):
        command, wire = self.store.prepare_command({"op": "brush", "action": "start", "duration": 2})
        self.store.dispatch_command({"op": "stop"}, self.sent.append)
        with self.assertRaises(CommandError):
            self.store.dispatch_prepared(wire, self.sent.append)
        self.assertEqual([wire["op"] for wire in self.sent], ["stop"])
        self.assertEqual(self.store.command_result(command["id"])["state"], "error")

    def test_disconnected_and_expired_prepared_commands_never_send(self):
        _, wire = self.store.prepare_command({"op": "brush", "action": "start", "duration": 2})
        self.store.set_mqtt(False)
        self.store.set_mqtt(True)
        self.frame()
        with self.assertRaises(CommandError):
            self.store.dispatch_prepared(wire, self.sent.append)
        _, wire = self.store.prepare_command({"op": "brush", "action": "start", "duration": 2})
        self.tick += 31
        self.frame()
        with self.assertRaises(CommandError):
            self.store.dispatch_prepared(wire, self.sent.append)
        self.assertEqual(self.sent, [])

    def test_bad_ack_and_huge_number_do_not_interrupt_receiver(self):
        from mqtt_service import MqttService
        from types import SimpleNamespace
        service = object.__new__(MqttService)
        service.store = self.store
        self.frame(ack={"id": [], "result": "ok", "message": ""})
        service._on_message(None, None, SimpleNamespace(topic=self.store.latest_topic, retain=False,
            payload=('{"ph":' + "9" * 400 + ',"doMgL":1,"waterTemp":2,"doSaturation":3}').encode()))
        service._on_message(None, None, SimpleNamespace(topic=self.store.latest_topic, retain=False,
            payload=b'{"ph":0,"doMgL":0,"waterTemp":0,"doSaturation":0}'))
        self.assertEqual(self.store.history()[0]["ph"], 0)
        self.frame()
        self.assertTrue(self.store.snapshot()["device"]["controlAvailable"])

    def test_header_only_and_empty_groups_cannot_enable_or_confirm(self):
        _, wire = self.store.prepare_command({"op": "stop"})
        self.store.receive(self.store.status_topic, json.dumps({
            "v": 1, "online": True, "bootId": "12345678",
            "ack": {"id": wire["id"], "result": "ok", "message": ""}
        }).encode(), False)
        self.assertEqual(self.store.snapshot()["commands"][0]["state"], "pending")
        self.store.set_mqtt(False)
        self.store.set_mqtt(True)
        self.frame(collection={})
        self.assertFalse(self.store.snapshot()["device"]["controlAvailable"])
        self.frame(brush={"running": "false", "remaining": 0})
        self.assertFalse(self.store.snapshot()["device"]["controlAvailable"])
        self.frame()
        self.assertTrue(self.store.snapshot()["device"]["controlAvailable"])

    def test_old_result_is_available_after_many_queries(self):
        command, _ = self.store.prepare_command({"op": "mode", "mode": "manual"})
        for _ in range(31):
            self.store.prepare_command({"op": "query"})
        self.frame(ack={"id": command["id"], "result": "ok", "message": ""})
        response = self.app.test_client().get("/api/commands/" + command["id"])
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json["command"]["state"], "confirmed")

    def test_large_json_has_json_413_response(self):
        response = self.app.test_client().post("/api/command", json={"op": "stop", "padding": "x" * 2100})
        self.assertEqual(response.status_code, 413)
        self.assertFalse(response.json["ok"])
        self.assertEqual(self.sent, [])

    def test_rejected_high_sequence_does_not_block_valid_frames(self):
        self.tick += 21
        self.seq = 999999
        self.frame(collection={})
        self.assertFalse(self.store.snapshot()["device"]["controlAvailable"])
        self.seq = 1
        self.frame()
        self.assertTrue(self.store.snapshot()["device"]["controlAvailable"])
        self.assertEqual(self.store.snapshot()["device"]["state"]["seq"], 2)

    def test_offline_will_cannot_confirm_pending_command(self):
        command, _ = self.store.prepare_command({"op": "stop"})
        offline = {"v": 1, "online": False, "bootId": "12345678",
                   "ack": {"id": command["id"], "result": "ok", "message": ""}}
        self.store.receive(self.store.status_topic, json.dumps(offline).encode(), False)
        self.assertEqual(self.store.command_result(command["id"])["state"], "pending")
        self.assertFalse(self.store.snapshot()["device"]["controlAvailable"])


if __name__ == "__main__":
    unittest.main()
