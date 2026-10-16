"""功能：启动新 STM32 水质控制网页，提供状态、控制、历史记录和导出接口。"""
import csv
import io
import os
import socket
import threading
import webbrowser
from pathlib import Path
from urllib.parse import urlsplit

from flask import Flask, Response, jsonify, request, send_from_directory
from werkzeug.exceptions import RequestEntityTooLarge
from control_core import CommandError, ControlStore
from mqtt_service import MqttService


APP_DIR = Path(__file__).resolve().parent
DEFAULT_DEVICE = "000000000000000000000001"


def create_app(store, transport):
    app = Flask(__name__, static_folder=str(APP_DIR / "static"), static_url_path="/static")
    app.config["MAX_CONTENT_LENGTH"] = 2048

    @app.after_request
    def avoid_cached_status(response):
        response.headers["Cache-Control"] = "no-store"
        response.headers["X-Content-Type-Options"] = "nosniff"
        response.headers["Referrer-Policy"] = "same-origin"
        return response

    @app.get("/")
    def index():
        return send_from_directory(app.static_folder, "index.html")

    @app.get("/api/status")
    def status():
        return jsonify(store.snapshot())

    @app.get("/api/history")
    def history():
        try:
            limit = int(request.args.get("limit", "200"))
        except ValueError:
            return jsonify(ok=False, message="历史数量必须为整数"), 400
        return jsonify(ok=True, rows=store.history(limit))

    @app.get("/api/commands/<command_id>")
    def command_result(command_id):
        try:
            return jsonify(ok=True, command=store.command_result(command_id))
        except CommandError as exc:
            return jsonify(ok=False, message=str(exc)), exc.status

    @app.errorhandler(RequestEntityTooLarge)
    def oversized_body(error):
        return jsonify(ok=False, message="控制请求过大，请检查输入参数"), 413

    @app.post("/api/command")
    def command():
        if urlsplit("http://" + request.host).hostname not in ("127.0.0.1", "localhost", "::1"):
            return jsonify(ok=False, message="请通过本机地址访问控制网页"), 403
        origin = request.headers.get("Origin")
        if origin and urlsplit(origin).netloc != request.host:
            return jsonify(ok=False, message="该网页没有控制权限"), 403
        if not request.is_json:
            return jsonify(ok=False, message="控制请求必须使用 JSON"), 415
        try:
            entry = store.dispatch_command(request.get_json(silent=True), transport.send)
            return jsonify(ok=True, command=entry)
        except RequestEntityTooLarge:
            raise
        except CommandError as exc:
            return jsonify(ok=False, message=str(exc)), exc.status
        except Exception:
            return jsonify(ok=False, message="服务器连接异常，命令未得到确认"), 502

    @app.get("/api/export.csv")
    def export_csv():
        output = io.StringIO()
        writer = csv.writer(output)
        writer.writerow(["接收时间（北京时间）", "pH", "溶解氧(mg/L)", "水温(℃)", "溶氧饱和度(%)"])
        for row in reversed(store.history(10000)):
            writer.writerow([row["receivedAt"], row["ph"], row["doMgL"], row["waterTemp"], row["doSaturation"]])
        return Response(
            "\ufeff" + output.getvalue(), mimetype="text/csv; charset=utf-8",
            headers={"Content-Disposition": "attachment; filename=water_history.csv"},
        )

    return app


def available_port(preferred=5070):
    """从专用端口起查找空闲端口，不占用旧水质查看器。"""
    for port in range(preferred, preferred + 30):
        with socket.socket() as sock:
            try:
                sock.bind(("127.0.0.1", port))
                return port
            except OSError:
                continue
    raise OSError("没有可用的本机网页端口")


def main():
    device = os.environ.get("WATER_DEVICE_ID", DEFAULT_DEVICE)
    store = ControlStore(APP_DIR / "water_control.db", device)
    service = MqttService(store)
    app = create_app(store, service)
    port = available_port(int(os.environ.get("WATER_CONTROL_PORT", "5070")))
    url = f"http://127.0.0.1:{port}"
    print("STM32 Water Control")
    print(f"Browser: {url}", flush=True)
    service.start()
    if os.environ.get("WATER_NO_BROWSER") != "1":
        threading.Timer(1, lambda: webbrowser.open(url)).start()
    try:
        app.run(host="127.0.0.1", port=port, debug=False, use_reloader=False, threaded=True)
    finally:
        service.stop()
        store.close()


if __name__ == "__main__":
    main()
