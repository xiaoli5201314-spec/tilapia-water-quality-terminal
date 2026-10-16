"""功能：在独立本机测试服务验证持续刷新、时间选择、草稿保留和设备回执；不连接 MQTT。"""
import json
import tempfile
import threading
import unittest
from pathlib import Path

from playwright.sync_api import sync_playwright, expect
from werkzeug.serving import make_server
from app import create_app
from control_core import ControlStore


ROOT = Path(__file__).resolve().parents[1]


class BrowserTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.playwright = sync_playwright().start()
        cls.browser = cls.playwright.chromium.launch()

    @classmethod
    def tearDownClass(cls):
        cls.browser.close()
        cls.playwright.stop()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.store = ControlStore(Path(self.temp.name) / "browser.db", "000000000000000000000001")
        self.sequence = 0
        self.state = {
            "v": 1, "deviceId": self.store.device_id, "online": True, "bootId": "browser-test",
            "uptimeMs": 5000, "controlMode": "manual", "light": "green",
            "collection": {"mode": "run", "active": True, "start": 480, "end": 1080, "sample": 5, "publish": 3600},
            "brush": {"running": False, "remaining": 0},
            "actuator": {"state": "stopped", "remaining": 0},
            "cleaning": {"state": "idle", "nextIn": 600},
            "clock": {"synced": True, "unix": 1791770000},
            "brushPlan": {"type": "off", "start": 480, "interval": 21600, "duration": 10, "action": "start"},
            "actuatorPlan": {"type": "off", "start": 480, "interval": 21600, "duration": 12, "action": "extend"},
            "cleaningConfig": {"enabled": True, "interval": 21600, "extend": 12, "brush": 10, "retract": 12, "settle": 30},
            "ack": {"id": "", "result": "ok", "message": ""},
        }
        self.store.set_mqtt(True)
        self.publish_test_state()
        self.store.receive(self.store.latest_topic,
                           b'{"ph":7.65,"doMgL":6.04,"waterTemp":23.94,"doSaturation":72.44}', False)
        self.sent = []

        class NoMqttTransport:
            def send(inner, wire):
                self.sent.append(wire)
        self.server = make_server("127.0.0.1", 0, create_app(self.store, NoMqttTransport()), threaded=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.page = self.browser.new_page(viewport={"width": 1440, "height": 1000})
        self.errors = []
        self.page.on("pageerror", lambda error: self.errors.append(str(error)))
        self.url = f"http://127.0.0.1:{self.server.server_port}"
        self.page.goto(self.url)
        expect(self.page.locator("#metric-ph")).to_have_text("7.65", timeout=8000)

    def tearDown(self):
        self.page.close()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)
        self.store.close()
        self.temp.cleanup()

    def publish_test_state(self):
        # 使用固件的四片状态协议，浏览器确认链路同时覆盖后端组帧。
        self.sequence += 1
        groups = (
            ("controlMode", "collection", "clock", "light", "uptimeMs"),
            ("brush", "actuator", "cleaning", "ack"),
            ("brushPlan", "actuatorPlan"),
            ("cleaningConfig",),
        )
        for part, fields in enumerate(groups):
            packet = {key: self.state[key] for key in ("v", "deviceId", "online", "bootId")}
            packet.update(seq=self.sequence, part=part, parts=4)
            packet.update({key: self.state[key] for key in fields})
            self.store.receive(self.store.status_topic, json.dumps(packet).encode(), False)

    def test_select_identity_and_value_survive_updates_and_blur(self):
        selector = self.page.locator("#sampling-form select[name=mode]")
        self.page.evaluate("window.originalSelect = document.querySelector('#sampling-form select[name=mode]')")
        selector.select_option("schedule")
        self.page.locator("#sampling-form input[name=publish]").fill("60")
        self.page.locator("h1").click()
        self.state["uptimeMs"] = 8000
        self.publish_test_state()
        self.page.wait_for_timeout(3300)
        expect(selector).to_have_value("schedule")
        expect(self.page.locator("#sampling-form input[name=publish]")).to_have_value("60")
        self.assertTrue(self.page.evaluate("window.originalSelect === document.querySelector('#sampling-form select[name=mode]')"))
        self.assertEqual(self.errors, [])

    def test_time_modal_survives_refresh_and_confirmed_draft_survives_blur(self):
        self.page.locator("[data-time-for=sampling-start]").click()
        expect(self.page.locator("#time-modal")).to_be_visible()
        self.page.locator("#time-hour").fill("23")
        self.page.locator("#time-minute").fill("15")
        self.page.wait_for_timeout(3300)
        expect(self.page.locator("#time-modal")).to_be_visible()
        self.page.locator("#time-confirm").click()
        expect(self.page.locator("#sampling-start")).to_have_value("23:15")
        self.page.locator("h1").click()
        self.page.wait_for_timeout(1800)
        expect(self.page.locator("#sampling-start")).to_have_value("23:15")
        self.assertEqual(self.errors, [])

    def test_time_cancel_does_not_apply_and_wheel_can_select(self):
        self.page.locator("[data-time-for=sampling-start]").click()
        self.page.locator("#time-hour").fill("20")
        self.page.locator("#time-cancel").click()
        expect(self.page.locator("#sampling-start")).to_have_value("08:00")
        self.page.locator("[data-time-for=sampling-start]").click()
        self.page.locator("#hour-wheel").evaluate("(el) => el.scrollTop = 12 * 40")
        expect(self.page.locator("#time-hour")).to_have_value("12")
        self.page.locator("#minute-wheel").evaluate("(el) => el.scrollTop = 35 * 40")
        expect(self.page.locator("#time-minute")).to_have_value("35")
        self.page.locator("#time-confirm").click()
        expect(self.page.locator("#sampling-start")).to_have_value("12:35")
        self.assertEqual(self.sent, [])

    def test_new_draft_during_pending_command_survives_confirmation(self):
        publish = self.page.locator("#sampling-form input[name=publish]")
        publish.fill("60")
        self.page.locator("#sampling-form button[type=submit]").click()
        expect(self.page.locator("#sampling-form [data-feedback]")).to_contain_text("等待")
        publish.fill("90")
        self.state["collection"]["publish"] = 60
        self.state["ack"] = {"id": self.sent[0]["id"], "result": "ok", "message": ""}
        self.publish_test_state()
        expect(self.page.locator("#sampling-form [data-feedback]")).to_contain_text("新草稿未应用", timeout=8000)
        expect(publish).to_have_value("90")
        self.assertEqual(len(self.sent), 1)

    def test_send_is_pending_until_device_ack(self):
        expect(self.page.locator("#sampling-form button[type=submit]")).to_be_enabled()
        self.page.locator("#sampling-form input[name=publish]").fill("60")
        self.page.locator("#sampling-form button[type=submit]").click()
        expect(self.page.locator("#sampling-form [data-feedback]")).to_contain_text("等待")
        self.assertEqual(len(self.sent), 1)
        self.assertEqual(self.sent[0]["publish"], 60)
        self.state["collection"]["publish"] = 60
        self.state["ack"] = {"id": self.sent[0]["id"], "result": "ok", "message": ""}
        self.publish_test_state()
        expect(self.page.locator("#sampling-form [data-feedback]")).to_contain_text("确认", timeout=8000)
        self.assertEqual(self.errors, [])

    def test_desktop_and_mobile_no_page_overflow_and_lamp_readonly(self):
        artifacts = ROOT / "tests" / "artifacts"
        artifacts.mkdir(exist_ok=True)
        for width, height, name in [(1440, 1000, "desktop"), (390, 844, "mobile")]:
            self.page.set_viewport_size({"width": width, "height": height})
            self.page.wait_for_timeout(300)
            overflow = self.page.evaluate("document.documentElement.scrollWidth > innerWidth + 1")
            self.assertFalse(overflow, name)
            self.page.screenshot(path=str(artifacts / f"{name}.png"), full_page=True)
        self.assertEqual(self.page.locator(".lamp-panel button").count(), 0)
        self.assertEqual(self.page.locator("#light-green").get_attribute("data-lit"), "true")
        self.assertEqual(self.errors, [])

    def test_lost_http_connection_disables_controls_but_keeps_draft(self):
        self.page.locator("#sampling-form input[name=publish]").fill("90")
        self.page.route("**/api/status", lambda route: route.abort())
        self.page.locator("#refresh-status").click()
        expect(self.page.locator("#sampling-form button[type=submit]")).to_be_disabled(timeout=8000)
        expect(self.page.locator("#sampling-form input[name=publish]")).to_have_value("90")

    def test_actuator_visible_direction_matches_reversed_wire_command_and_state(self):
        form = self.page.locator("#actuator-action-form")
        form.locator("input[name=duration]").fill("2")
        for label, action, wire_state in [
            ("伸出", "retract", "retracting"), ("缩回", "extend", "extending")
        ]:
            form.get_by_role("button", name=label, exact=True).click()
            expect(form.locator("[data-feedback]")).to_have_attribute("data-state", "pending")
            self.assertEqual(self.sent[-1]["op"], "actuator")
            self.assertEqual(self.sent[-1]["action"], action)
            self.assertEqual(self.sent[-1]["duration"], 2)
            self.state["actuator"] = {"state": wire_state, "remaining": 2}
            self.state["ack"] = {"id": self.sent[-1]["id"], "result": "ok", "message": ""}
            self.publish_test_state()
            expect(form.locator("[data-feedback]")).to_have_attribute("data-state", "confirmed", timeout=8000)
            expect(self.page.locator("#actuator-state")).to_have_text(f"{label}中")
        self.assertEqual(self.errors, [])


if __name__ == "__main__":
    unittest.main()
