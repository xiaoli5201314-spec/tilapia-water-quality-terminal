"""功能：校验控制协议、保存水质记录，并以设备回执确认命令执行结果。"""
import copy
import json
import math
import re
import sqlite3
import threading
import time
import uuid
from datetime import datetime, timezone, timedelta


BEIJING = timezone(timedelta(hours=8))
FRESH_SECONDS = 20
# 实机四片发布约需 6 秒；只扩展组装窗口，实时状态仍按 20 秒判定失效。
STATUS_FRAME_WINDOW_SECONDS = 10
COMMAND_TTL = 30
WATER_FIELDS = ("ph", "doMgL", "waterTemp", "doSaturation")
STATE_GROUPS = (
    ("controlMode", "collection", "clock", "light", "uptimeMs"),
    ("brush", "actuator", "cleaning", "ack"),
    ("brushPlan", "actuatorPlan"),
    ("cleaningConfig",),
)
REJECTION_MESSAGES = {
    "busy": "设备忙，正在执行其他动作",
    "manual_required": "请先切换到手动模式",
    "auto_required": "请先切换到自动模式",
    "clock_not_ready": "设备尚未完成校时",
    "expired": "命令已过期",
    "future": "设备与电脑时钟不一致",
    "duplicate": "设备已处理此命令，未重复执行",
    "invalid": "设备拒绝了无效参数",
    "storage_error": "设备保存配置失败，未应用设置",
}


def now_text():
    return datetime.now(BEIJING).isoformat(timespec="seconds")


def valid_state(data):
    """只有字段齐全、类型及范围正确的运行快照才能开启控制或确认命令。"""
    def integer(value, low, high):
        return type(value) is int and low <= value <= high

    def group(name, rules):
        value = data.get(name)
        return isinstance(value, dict) and all(
            key in value and check(value[key]) for key, check in rules.items()
        )

    def plan(name, actions):
        return group(name, {
            "type": lambda x: x in ("off", "daily", "interval"),
            "start": lambda x: integer(x, 0, 1439),
            "interval": lambda x: integer(x, 60, 86400),
            "duration": lambda x: integer(x, 1, 30),
            "action": lambda x: x in actions,
        })

    if (data.get("controlMode") not in ("auto", "manual")
            or data.get("light") not in ("off", "red", "yellow", "green")
            or not integer(data.get("uptimeMs"), 0, 0xffffffff)):
        return False
    return all((
        group("collection", {
            "mode": lambda x: x in ("run", "pause", "schedule"),
            "active": lambda x: type(x) is bool,
            "start": lambda x: integer(x, 0, 1439), "end": lambda x: integer(x, 0, 1439),
            "sample": lambda x: integer(x, 2, 3600), "publish": lambda x: integer(x, 5, 86400),
        }),
        group("clock", {"synced": lambda x: type(x) is bool, "unix": lambda x: integer(x, 0, 0xffffffff)}),
        group("brush", {"running": lambda x: type(x) is bool, "remaining": lambda x: integer(x, 0, 30)}),
        group("actuator", {"state": lambda x: x in ("stopped", "extending", "retracting", "waiting"),
                           "remaining": lambda x: integer(x, 0, 30)}),
        group("cleaning", {"state": lambda x: x in ("idle", "extending", "brushing", "retracting", "settling", "fault"),
                           "nextIn": lambda x: integer(x, 0, 604800)}),
        group("ack", {"id": lambda x: isinstance(x, str) and (x == "" or bool(re.fullmatch(r"[a-f0-9]{12}", x))),
                      "result": lambda x: x in ("ok", "rejected"),
                      "message": lambda x: isinstance(x, str) and len(x) <= 120}),
        plan("brushPlan", ("start",)), plan("actuatorPlan", ("extend", "retract")),
        group("cleaningConfig", {"enabled": lambda x: type(x) is bool,
            "interval": lambda x: integer(x, 60, 604800),
            "extend": lambda x: integer(x, 1, 30), "brush": lambda x: integer(x, 1, 30),
            "retract": lambda x: integer(x, 1, 30), "settle": lambda x: integer(x, 0, 600)}),
    ))


class CommandError(ValueError):
    """参数错误或设备暂不可控，HTTP 层按 status 返回。"""
    def __init__(self, message, status=400):
        super().__init__(message)
        self.status = status


def validate_command(body):
    """严格验证平坦业务命令，禁止客户端注入编号、过期时间或未知字段。"""
    if not isinstance(body, dict) or not isinstance(body.get("op"), str):
        raise CommandError("请选择有效的操作")
    op = body["op"]
    keys = {"op"}

    def enum(name, options):
        keys.add(name)
        if body.get(name) not in options:
            raise CommandError(f"{name} 参数无效")

    def integer(name, low, high):
        keys.add(name)
        if type(body.get(name)) is not int or not low <= body[name] <= high:
            raise CommandError(f"{name} 必须为 {low} 至 {high} 的整数")

    if op == "sampling":
        enum("mode", ("run", "pause", "schedule"))
        integer("start", 0, 1439)
        integer("end", 0, 1439)
        integer("sample", 2, 3600)
        integer("publish", 5, 86400)
        if body["mode"] == "schedule" and body["start"] == body["end"]:
            raise CommandError("采集开始时间和结束时间不能相同")
    elif op == "mode":
        enum("mode", ("auto", "manual"))
    elif op in ("brush", "actuator"):
        enum("action", ("start", "stop") if op == "brush" else ("extend", "retract", "stop"))
        if body["action"] != "stop":
            integer("duration", 1, 30)
    elif op == "plan":
        enum("target", ("brush", "actuator"))
        enum("type", ("off", "daily", "interval"))
        integer("start", 0, 1439)
        integer("interval", 60, 86400)
        integer("duration", 1, 30)
        enum("action", ("start",) if body["target"] == "brush" else ("extend", "retract"))
    elif op == "cleaning_config":
        keys.add("enabled")
        if type(body.get("enabled")) is not bool:
            raise CommandError("自动清洗开关必须为布尔值")
        integer("interval", 60, 604800)
        for name in ("extend", "brush", "retract"):
            integer(name, 1, 30)
        integer("settle", 0, 600)
    elif op == "cleaning":
        enum("action", ("start", "stop"))
    elif op not in ("stop", "query"):
        raise CommandError("不支持该操作")
    if set(body) != keys:
        raise CommandError("命令缺少必要参数或包含未知字段")
    return dict(body)


class ControlStore:
    """所有连接状态和数据库操作共用锁，保留消息不能伪装实时设备确认。"""
    def __init__(self, db_path, device_id, monotonic=time.monotonic):
        if not re.fullmatch(r"[A-Fa-f0-9]{24}", device_id):
            raise ValueError("STM32 deviceId 必须为 24 位十六进制编号")
        self.device_id = device_id.upper()
        prefix = f"fish/device/{self.device_id}/water"
        self.latest_topic = prefix + "/latest"
        self.command_topic = prefix + "/cmd"
        self.status_topic = prefix + "/control/status"
        self.lock = threading.RLock()
        self.monotonic = monotonic
        self.db = sqlite3.connect(db_path, check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.db.executescript("""
          PRAGMA journal_mode=WAL;
          CREATE TABLE IF NOT EXISTS readings (
            id INTEGER PRIMARY KEY, received_at TEXT NOT NULL,
            ph REAL NOT NULL, do_mg_l REAL NOT NULL,
            water_temp REAL NOT NULL, do_saturation REAL NOT NULL
          );
          CREATE TABLE IF NOT EXISTS commands (
            id TEXT PRIMARY KEY, op TEXT NOT NULL, state TEXT NOT NULL,
            message TEXT NOT NULL, requested_at TEXT NOT NULL,
            updated_at TEXT NOT NULL, payload TEXT NOT NULL
          );
        """)
        self.db.execute(
            "UPDATE commands SET state='expired',message=?,updated_at=? WHERE state='pending'",
            ("上位机已重启，未重新发送旧命令", now_text()),
        )
        self.db.commit()
        self.mqtt = {"connected": False, "error": "", "broker": "127.0.0.1:1883"}
        self.state = None
        self.status_received_at = None
        self.live_at = None
        self.live_online = None
        self.pending = {}
        self.frames = {}
        self.last_frame = None
        self.connection_epoch = 0
        self.water = {"receivedAt": None, "retained": False, "values": None}
        row = self.db.execute("SELECT * FROM readings ORDER BY id DESC LIMIT 1").fetchone()
        if row:
            self.water = {
                "receivedAt": row["received_at"], "retained": False, "fromHistory": True,
                "values": dict(zip(WATER_FIELDS, (row["ph"], row["do_mg_l"], row["water_temp"], row["do_saturation"]))),
            }

    def close(self):
        with self.lock:
            self.db.close()

    def set_mqtt(self, connected, error=""):
        with self.lock:
            was_connected = self.mqtt["connected"]
            self.mqtt.update(connected=bool(connected), error=str(error)[:300])
            if not connected or not was_connected:
                self.connection_epoch += 1
                self.live_at = None
                self.live_online = None
                self.frames.clear()
                self.last_frame = None
            if not connected:
                for command_id in list(self.pending):
                    self._finish(command_id, "error", "连接中断，执行结果未知；未自动重发")

    def _finish(self, command_id, state, message):
        self.db.execute(
            "UPDATE commands SET state=?,message=?,updated_at=? WHERE id=? AND state='pending'",
            (state, message, now_text(), command_id),
        )
        self.pending.pop(command_id, None)
        self.db.commit()

    def _expire(self):
        for command_id, meta in list(self.pending.items()):
            if self.monotonic() >= meta["deadline"]:
                self._finish(command_id, "expired", "设备未在 30 秒内确认，结果未知；未自动重发")

    def _available(self):
        return bool(
            self.mqtt["connected"] and self.live_online is True and self.live_at is not None
            and self.monotonic() - self.live_at <= FRESH_SECONDS
        )

    def _assemble(self, data, retained):
        """模块 MQTT 缓冲较小：仅合并同启动、同序号的四片完整实时状态。"""
        if "part" not in data and "parts" not in data:
            return data if data.get("online") is False else None
        if retained or data.get("online") is not True:
            return None
        part, count, seq = data.get("part"), data.get("parts"), data.get("seq")
        if (type(part) is not int or type(count) is not int or count != 4
                or not 0 <= part < 4 or type(seq) is not int or not 0 <= seq <= 0xffffffff
                or not isinstance(data.get("bootId"), str)):
            return None
        key = (data["bootId"], seq)
        if self.last_frame and self.last_frame[0] == key[0]:
            delta = (seq - self.last_frame[1]) & 0xffffffff
            if delta == 0 or delta >= 0x80000000:
                return None
        required = STATE_GROUPS[part]
        if not all(field in data for field in required):
            return None
        now = self.monotonic()
        self.frames = {
            frame_key: frame for frame_key, frame in self.frames.items()
            if now - frame["started"] <= STATUS_FRAME_WINDOW_SECONDS and frame_key[0] == key[0]
        }
        if key not in self.frames and len(self.frames) >= 2:
            self.frames.pop(next(iter(self.frames)))
        frame = self.frames.setdefault(key, {"started": now, "parts": {}})
        frame["parts"][part] = {field: data[field] for field in required}
        if len(frame["parts"]) != 4:
            return None
        merged = {field: data[field] for field in ("v", "bootId", "online")}
        merged.update(deviceId=self.device_id, seq=seq)
        for number in range(4):
            merged.update(frame["parts"][number])
        self.frames.pop(key, None)
        return merged

    def receive(self, topic, raw, retained):
        """只接受本设备的数据/状态，对超大报文、非JSON和非有限值直接忽略。"""
        if not isinstance(raw, (bytes, bytearray)) or len(raw) > 8192:
            return
        try:
            data = json.loads(raw.decode("utf-8"))
        except (ValueError, UnicodeError, RecursionError):
            return
        if not isinstance(data, dict):
            return
        with self.lock:
            self._expire()
            if topic == self.latest_topic:
                try:
                    if any(type(data.get(key)) not in (int, float) for key in WATER_FIELDS):
                        return
                    values = {key: float(data[key]) for key in WATER_FIELDS}
                except (OverflowError, ValueError):
                    return
                if any(not math.isfinite(value) for value in values.values()):
                    return
                received = now_text()
                self.water = {"receivedAt": received, "retained": bool(retained), "values": values}
                if not retained:
                    self.db.execute(
                        "INSERT INTO readings(received_at,ph,do_mg_l,water_temp,do_saturation) VALUES(?,?,?,?,?)",
                        (received, *(values[key] for key in WATER_FIELDS)),
                    )
                    self.db.commit()
                return
            if topic != self.status_topic or type(data.get("v")) is not int or data["v"] != 1:
                return
            if data.get("deviceId", self.device_id) != self.device_id:
                return
            if type(data.get("online")) is not bool:
                return
            if data["online"] and (not isinstance(data.get("bootId"), str) or not data["bootId"]):
                return
            data = self._assemble(data, retained)
            if data is None:
                return
            if data["online"] and not valid_state(data):
                return
            if data["online"]:
                self.last_frame = (data["bootId"], data["seq"])
                self.frames.clear()
            if retained and self.live_at is not None:
                return
            self.status_received_at = now_text()
            if data["online"]:
                self.state = data
            if not retained:
                self.live_at = self.monotonic()
                self.live_online = data["online"]
                boot_id = data.get("bootId")
                for command_id, meta in list(self.pending.items()):
                    if data["online"] and boot_id != meta["bootId"]:
                        self._finish(command_id, "error", "设备已重启，原命令执行结果未知")
                ack = data.get("ack") if data["online"] else None
                if isinstance(ack, dict) and ack.get("id") in self.pending:
                    command_id = ack["id"]
                    if boot_id == self.pending[command_id]["bootId"]:
                        result = ack.get("result")
                        if result in ("ok", "rejected"):
                            reason = str(ack.get("message", ""))[:120]
                            message = "设备已确认" if result == "ok" else REJECTION_MESSAGES.get(reason, f"设备拒绝：{reason or '参数或状态不满足'}")
                            self._finish(command_id, "confirmed" if result == "ok" else "rejected", message)

    def prepare_command(self, body):
        valid = validate_command(body)
        with self.lock:
            self._expire()
            if not self._available():
                raise CommandError("尚未收到设备实时控制状态，请先烧录配套控制固件并等待连接", 409)
            op = valid["op"]
            is_stop = op == "stop" or valid.get("action") == "stop" or op == "query"
            clock = self.state.get("clock", {})
            if not is_stop and not (isinstance(clock, dict) and clock.get("synced") is True):
                raise CommandError("设备尚未校时，请等待时钟同步后再设置或启动", 409)
            if op in ("brush", "actuator") and valid.get("action") != "stop":
                if self.state.get("controlMode") != "manual":
                    raise CommandError("请先切换到手动模式，再操作毛刷或推杆", 409)
            # 停止可越过其他待确认命令；配置一次一个，避免不同回执互相覆盖。
            if self.pending and not is_stop:
                raise CommandError("已有命令等待设备确认，请稍后再操作", 409)
            if is_stop and op != "query":
                # 尚未发布的旧动作不可晚于停止发出；已发出的结果仍等待设备回执。
                for old_id, meta in list(self.pending.items()):
                    if not meta["dispatched"]:
                        self._finish(old_id, "error", "后续停止取消了尚未发送的命令")
            command_id = uuid.uuid4().hex[:12]
            wire = {"v": 1, "id": command_id, **valid, "issued": int(time.time()), "ttl": COMMAND_TTL}
            wire_json = json.dumps(wire, separators=(",", ":"))
            if len(wire_json.encode()) > 384:
                raise CommandError("命令长度超过设备限制")
            timestamp = now_text()
            self.db.execute(
                "INSERT INTO commands VALUES(?,?,?,?,?,?,?)",
                (command_id, op, "pending", "等待设备确认", timestamp, timestamp, wire_json),
            )
            self.db.commit()
            self.pending[command_id] = {
                "deadline": self.monotonic() + COMMAND_TTL, "bootId": self.state["bootId"],
                "epoch": self.connection_epoch, "dispatched": False, "wire": wire,
            }
            return self._command_row(command_id), wire

    def dispatch_prepared(self, wire, send):
        """发布前在同一锁内重查期限、连接、启动编号及停止取消结果，不重发。"""
        with self.lock:
            self._expire()
            command_id = wire.get("id")
            meta = self.pending.get(command_id)
            if (meta is None or meta["dispatched"] or meta["wire"] != wire
                    or meta["epoch"] != self.connection_epoch or not self._available()
                    or meta["bootId"] != self.state["bootId"]):
                if meta is not None:
                    self._finish(command_id, "error", "发送前状态已变化，命令已取消")
                raise CommandError("命令已取消或过期，未重新发送", 409)
            meta["dispatched"] = True
            try:
                send(wire)
            except Exception:
                self._finish(command_id, "error", "发送失败，未自动重试；执行结果未知")
                raise
            return self._command_row(command_id)

    def dispatch_command(self, body, send):
        """串行化准备和发布，两个 HTTP 请求不能把启动重排到停止之后。"""
        with self.lock:
            _, wire = self.prepare_command(body)
            return self.dispatch_prepared(wire, send)

    def fail_command(self, command_id, message):
        with self.lock:
            self._finish(command_id, "error", message)

    def _command_row(self, command_id):
        row = self.db.execute("SELECT * FROM commands WHERE id=?", (command_id,)).fetchone()
        return {"id": row["id"], "op": row["op"], "state": row["state"], "message": row["message"],
                "requestedAt": row["requested_at"], "updatedAt": row["updated_at"]}

    def command_result(self, command_id):
        """独立查询命令结果，最近记录的展示数量不会丢失旧请求的确认。"""
        if not isinstance(command_id, str) or not re.fullmatch(r"[a-f0-9]{12}", command_id):
            raise CommandError("未找到该命令", 404)
        with self.lock:
            self._expire()
            if self.db.execute("SELECT id FROM commands WHERE id=?", (command_id,)).fetchone() is None:
                raise CommandError("未找到该命令", 404)
            return self._command_row(command_id)
    def snapshot(self):
        with self.lock:
            self._expire()
            available = self._available()
            online = available if self.live_online is not None else None
            commands = [
                self._command_row(row["id"]) for row in
                self.db.execute("SELECT id FROM commands ORDER BY rowid DESC LIMIT 30").fetchall()
            ]
            return {
                "ok": True, "serverTime": now_text(), "mqtt": dict(self.mqtt),
                "device": {"id": self.device_id, "online": online, "controlAvailable": available,
                           "statusReceivedAt": self.status_received_at, "state": copy.deepcopy(self.state)},
                "water": copy.deepcopy(self.water), "commands": commands,
                "topics": {"latest": self.latest_topic, "command": self.command_topic,
                           "status": self.status_topic},
            }

    def history(self, limit=200):
        with self.lock:
            rows = self.db.execute(
                "SELECT * FROM readings ORDER BY id DESC LIMIT ?", (min(max(int(limit), 1), 10000),)
            ).fetchall()
            return [{"receivedAt": row["received_at"], "retained": False, "ph": row["ph"],
                     "doMgL": row["do_mg_l"], "waterTemp": row["water_temp"],
                     "doSaturation": row["do_saturation"]} for row in rows]
