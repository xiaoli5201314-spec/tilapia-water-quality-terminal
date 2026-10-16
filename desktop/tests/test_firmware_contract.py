"""功能：直接使用配套固件主机测试导出的状态报文验证 Python 组帧与容量契约。"""
import json
import unittest
from pathlib import Path
from control_core import ControlStore, valid_state


class FirmwareContractTests(unittest.TestCase):
    def test_actual_c_status_packets_match_backend(self):
        packets_path = Path(__file__).resolve().parents[2] / "firmware" / "tests" / "status_packets.jsonl"
        raw_packets = packets_path.read_text(encoding="utf-8").splitlines()
        packets = [json.loads(line) for line in raw_packets]
        self.assertEqual(len(packets), 4)
        store = ControlStore(":memory:", packets[0]["deviceId"])
        self.addCleanup(store.close)
        store.set_mqtt(True)
        for index, raw in enumerate(raw_packets):
            self.assertLess(len(raw.encode()), 400)
            self.assertLessEqual(len(raw.encode()) + len(store.status_topic.encode()) + 8, 512)
            store.receive(store.status_topic, raw.encode(), False)
            if index < 3:
                self.assertFalse(store.snapshot()["device"]["controlAvailable"])
        snapshot = store.snapshot()
        self.assertTrue(snapshot["device"]["controlAvailable"])
        self.assertTrue(valid_state(snapshot["device"]["state"]))
        self.assertEqual(snapshot["device"]["state"]["cleaningConfig"]["interval"], 604800)
        # 同一启动内序号由最大值回绕到 0，仍应按新帧接受。
        for packet in packets:
            packet["seq"] = 0
            if packet["part"] == 0:
                packet["uptimeMs"] = 5
            store.receive(store.status_topic, json.dumps(packet).encode(), False)
        self.assertEqual(store.snapshot()["device"]["state"]["seq"], 0)


if __name__ == "__main__":
    unittest.main()
