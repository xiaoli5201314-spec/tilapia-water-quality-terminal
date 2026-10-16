// 功能：固定 DOM 水质工作台，独立草稿、设备回执、定时滚轮与安全控制门禁。
(function (root) {
  "use strict";

  const operationNames = {
    sampling: "采集设置", mode: "工作模式", brush: "毛刷控制",
    actuator: "推杆控制", plan: "机械计划", cleaning_config: "清洗配置",
    cleaning: "清洗控制", stop: "全部停止", query: "查询设备"
  };
  const commandStates = {
    pending: ["等待设备确认", "warn"], confirmed: ["设备已确认", "good"],
    rejected: ["设备拒绝", "bad"], expired: ["确认超时", "bad"],
    error: ["执行结果未知", "bad"]
  };
  // 现场推杆方向对调：固件 extend 对应缩回，retract 对应伸出；与 HTML 的按钮和选项保持一致。
  const actuatorDirections = { extend: "缩回", retract: "伸出" };
  const actuatorStateNames = {
    stopped: "已停止", extending: `${actuatorDirections.extend}中`,
    retracting: `${actuatorDirections.retract}中`, waiting: "换向等待",
    extend: `${actuatorDirections.extend}中`, retract: `${actuatorDirections.retract}中`
  };
  const cleaningNames = {
    idle: "空闲", extending: actuatorStateNames.extending, brushing: "刷洗中",
    retracting: actuatorStateNames.retracting, settling: "静置中", paused: "已暂停",
    stopped: "已停止", waiting: "等待清洗", fault: "清洗故障", error: "清洗错误"
  };
  const pad = value => String(value).padStart(2, "0");
  const timeString = value => Number.isInteger(value) && value >= 0 && value < 1440
    ? `${pad(Math.floor(value / 60))}:${pad(value % 60)}` : "";
  const timeMinutes = value => {
    const match = /^([01]?\d|2[0-3]):([0-5]\d)$/.exec(String(value).trim());
    if (!match) throw new Error("时间须为 00:00–23:59，例如 08:30");
    return Number(match[1]) * 60 + Number(match[2]);
  };
  const formatNumber = (value, digits) => typeof value === "number" && Number.isFinite(value)
    ? value.toFixed(digits) : "--";
  const dateFormatter = new Intl.DateTimeFormat("zh-CN", {
    timeZone: "Asia/Shanghai", year: "numeric", month: "2-digit", day: "2-digit",
    hour: "2-digit", minute: "2-digit", second: "2-digit", hourCycle: "h23"
  });
  const dateString = value => {
    if (!value) return "--";
    const date = new Date(value);
    return Number.isNaN(date.getTime()) ? "--" : dateFormatter.format(date);
  };
  const secondsString = value => typeof value === "number" && Number.isFinite(value) && value >= 0
    ? `${Math.floor(value)} 秒` : "--";

  function createApp(options) {
    const document = options.document;
    const fetch = options.fetch;
    const now = options.now || Date.now;
    const get = id => document.getElementById(id);
    const all = selector => Array.from(document.querySelectorAll(selector));
    const text = (id, value) => {
      const element = get(id);
      const next = String(value);
      if (element && element.textContent !== next) element.textContent = next;
    };
    const badge = (id, value, tone = "neutral") => {
      text(id, value);
      get(id).dataset.tone = tone;
    };
    const globalFeedback = (state, message) => {
      get("global-feedback").dataset.state = state;
      text("global-feedback", message);
    };

    let snapshot = null;
    let lastSuccess = 0;
    let hasSuccess = false;
    let serverReachable = false;
    let refreshRequest = null;
    let historyRequest = null;
    let historyLoadedAt = 0;
    let commandSignature = "";
    let pollStarted = false;
    let latestRequestSequence = 0;
    const localCommands = new Map();
    const ownedCommands = new Map();
    const forms = new Map();

    const definitions = [
      ["sampling-form", "collection", "sampling"],
      ["mode-form", "controlMode", "mode"],
      ["brush-plan-form", "brushPlan", "plan"],
      ["actuator-plan-form", "actuatorPlan", "plan"],
      ["cleaning-form", "cleaningConfig", "cleaning_config"],
      ["brush-action-form", null, "brush"],
      ["actuator-action-form", null, "actuator"],
      ["cleaning-action-form", null, "cleaning"]
    ];
    for (const [id, configKey, op] of definitions) {
      const element = get(id);
      const fields = new Map(Array.from(element.querySelectorAll("[name]")).map(field => [field.name, field]));
      for (const field of fields.values()) {
        if (field.type === "checkbox") field.checked = field.hasAttribute("checked");
      }
      const form = {
        id, element, fields, configKey, op, initialized: false,
        dirty: false, revision: 0, pending: null, posting: 0, requestOrder: 0,
        feedback: element.querySelector("[data-feedback]")
      };
      forms.set(id, form);
      const edit = event => {
        if (event.target.name && fields.has(event.target.name)) markEdited(form);
      };
      element.addEventListener("input", edit);
      element.addEventListener("change", edit);
      element.addEventListener("submit", event => {
        event.preventDefault();
        if (configKey) applyForm(form);
      });
    }

    function formFeedback(form, state, message) {
      form.feedback.dataset.state = state;
      if (form.feedback.textContent !== message) form.feedback.textContent = message;
    }

    function markEdited(form) {
      form.dirty = true;
      form.revision++;
      if (form.pending) formFeedback(form, "pending", "等待设备确认 · 新草稿未应用");
      else if (form.posting) formFeedback(form, "posting", "正在发送 · 新草稿未应用");
      else formFeedback(form, "dirty", "未应用");
      updateControls();
    }

    function writeField(field, value) {
      if (field.type === "checkbox") field.checked = value === true;
      else if (field.tagName === "SELECT") {
        const option = Array.from(field.querySelectorAll("option")).find(item => item.value === String(value));
        if (option) option.selected = true;
      } else field.value = String(value);
    }

    function applyDeviceConfig(form, state) {
      const config = state && state[form.configKey];
      if (config === undefined || config === null) return false;
      const values = form.configKey === "controlMode" ? { mode: config } : config;
      if (typeof values !== "object") return false;
      for (const [name, field] of form.fields) {
        if (values[name] === undefined || values[name] === null) continue;
        const value = name === "start" || name === "end" ? timeString(values[name]) : values[name];
        writeField(field, value);
      }
      form.initialized = true;
      return true;
    }

    function initializeForms(state) {
      if (!state || state.v !== 1) return;
      for (const form of forms.values()) {
        if (!form.configKey || form.initialized) continue;
        const config = state[form.configKey];
        if (config === undefined || config === null) continue;
        // 首次确认配置只填充未编辑表单；初始化机会不因后续状态刷新重复触发。
        form.initialized = true;
        if (!form.dirty && !form.pending && !form.posting && applyDeviceConfig(form, state)) {
          formFeedback(form, "confirmed", "已确认 · 设备当前配置");
        }
      }
    }

    function integer(form, name, min, max, label) {
      const value = String(form.fields.get(name).value).trim();
      if (!/^\d+$/.test(value)) throw new Error(`${label}须为 ${min}–${max} 的整数`);
      const result = Number(value);
      if (!Number.isSafeInteger(result) || result < min || result > max) {
        throw new Error(`${label}须为 ${min}–${max} 的整数`);
      }
      return result;
    }

    function choice(form, name, choices, label) {
      const value = form.fields.get(name).value;
      if (!choices.includes(value)) throw new Error(`请选择${label}`);
      return value;
    }

    function buildPayload(form) {
      if (form.op === "sampling") {
        const payload = {
          op: "sampling", mode: choice(form, "mode", ["run", "pause", "schedule"], "采集方式"),
          start: timeMinutes(form.fields.get("start").value),
          end: timeMinutes(form.fields.get("end").value),
          sample: integer(form, "sample", 2, 3600, "采样周期"),
          publish: integer(form, "publish", 5, 86400, "发布周期")
        };
        if (payload.mode === "schedule" && payload.start === payload.end) throw new Error("开始时间与结束时间不能相同");
        return payload;
      }
      if (form.op === "mode") return { op: "mode", mode: choice(form, "mode", ["manual", "auto"], "工作模式") };
      if (form.op === "plan") {
        const target = form.id === "brush-plan-form" ? "brush" : "actuator";
        return {
          op: "plan", target, type: choice(form, "type", ["off", "daily", "interval"], "计划方式"),
          start: timeMinutes(form.fields.get("start").value),
          interval: integer(form, "interval", 60, 86400, "计划间隔"),
          duration: integer(form, "duration", 1, 30, "运行时长"),
          action: target === "brush" ? "start" : choice(form, "action", ["extend", "retract"], "推杆动作")
        };
      }
      if (form.op === "cleaning_config") return {
        op: "cleaning_config", enabled: form.fields.get("enabled").checked === true,
        interval: integer(form, "interval", 60, 604800, "清洗间隔"),
        extend: integer(form, "extend", 1, 30, `${actuatorDirections.extend}时长`),
        brush: integer(form, "brush", 1, 30, "刷洗时长"),
        retract: integer(form, "retract", 1, 30, `${actuatorDirections.retract}时长`),
        settle: integer(form, "settle", 0, 600, "静置时间")
      };
      throw new Error("未知配置");
    }

    function canControl() {
      return serverReachable && hasSuccess && now() - lastSuccess < 20000
        && snapshot && snapshot.device && snapshot.device.controlAvailable === true;
    }

    function canStart(payload) {
      const state = snapshot && snapshot.device && snapshot.device.state;
      if (!state) return false;
      if (payload.op === "brush" || payload.op === "actuator") {
        return state.controlMode === "manual" && state.clock && state.clock.synced === true;
      }
      if (payload.op === "cleaning") {
        return state.controlMode === "auto" && state.clock && state.clock.synced === true
          && state.cleaning && state.cleaning.state === "idle"
          && state.brush && state.brush.running === false
          && state.actuator && state.actuator.state === "stopped";
      }
      return true;
    }

    function updateControls() {
      const available = canControl();
      const state = snapshot && snapshot.device && snapshot.device.state;
      for (const button of all("[data-send]")) {
        let allowed = available;
        const action = button.dataset.action || "";
        const isStop = action === "stop" || action.endsWith("-stop");
        const form = forms.get(button.closest("form")?.id);
        if (button.hasAttribute("data-manual")) {
          allowed = allowed && state?.controlMode === "manual" && state?.clock?.synced === true;
        }
        if (button.hasAttribute("data-auto")) allowed = allowed && canStart({ op: "cleaning" });
        if (!isStop && form && (form.pending || form.posting)) allowed = false;
        button.disabled = !allowed;
        button.title = allowed ? "" : !available ? "等待设备新鲜控制状态"
          : form && (form.pending || form.posting) ? "等待当前命令结果"
          : button.hasAttribute("data-manual") ? "需手动模式且设备已校时" : "需自动模式、已校时且设备空闲";
      }
    }

    async function requestJSON(url, init = {}) {
      const controller = new AbortController();
      const timer = setTimeout(() => controller.abort(), 8000);
      try {
        const response = await fetch(url, { ...init, signal: controller.signal, cache: "no-store" });
        const data = await response.json();
        if (!response.ok || data.ok !== true) throw new Error(data.message || `服务请求失败 (${response.status || "未知"})`);
        return data;
      } finally { clearTimeout(timer); }
    }

    async function applyForm(form) {
      if (form.posting || form.pending) return;
      try { await sendCommand(buildPayload(form), form); }
      catch (error) { formFeedback(form, "failed", error.message); }
    }

    async function sendCommand(payload, form = null) {
      const isStop = payload.op === "stop" || payload.action === "stop";
      if (!canControl()) {
        const message = "等待设备新鲜控制状态；草稿已保留";
        if (form) formFeedback(form, "failed", message);
        globalFeedback("failed", message);
        return;
      }
      if (!isStop && !canStart(payload)) {
        const message = payload.op === "cleaning" ? "需自动模式、已校时且设备空闲" : "需手动模式且设备已校时";
        if (form) formFeedback(form, "failed", message);
        globalFeedback("failed", message);
        return;
      }
      if (!isStop && form && (form.posting || form.pending)) return;
      const sequence = ++latestRequestSequence;
      const order = form ? ++form.requestOrder : 0;
      const sentRevision = form?.revision;
      if (form) {
        form.posting++;
        formFeedback(form, "posting", "正在发送 · 尚未确认");
      }
      globalFeedback("posting", `${operationNames[payload.op]}：正在发送`);
      updateControls();
      try {
        const data = await requestJSON("/api/command", {
          method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(payload)
        });
        const command = data.command;
        if (!command || typeof command.id !== "string" || !commandStates[command.state]) {
          throw new Error("服务返回缺少有效命令结果，请查看命令记录");
        }
        localCommands.set(command.id, command);
        ownedCommands.set(command.id, { form, order, sequence, sentRevision, resolved: false, payload });
        if (form && form.requestOrder === order) {
          form.pending = { id: command.id, revision: sentRevision };
          formFeedback(form, "pending", `${command.message || "等待设备确认"} · ${command.id}`);
        }
        if (sequence === latestRequestSequence) {
          globalFeedback("pending", `${operationNames[payload.op]}：${command.message || "等待设备确认"} · ${command.id}`);
        }
        reconcileCommands(mergedCommands());
        renderCommands(mergedCommands());
      } catch (error) {
        const detail = error.name === "AbortError" ? "请求超时，执行结果未知，请查看命令记录"
          : error instanceof TypeError ? "网络请求未完成，执行结果未知，请查看命令记录" : error.message;
        if (form && form.requestOrder === order) formFeedback(form, "failed", detail);
        if (sequence === latestRequestSequence) globalFeedback("failed", detail);
      } finally {
        if (form) form.posting--;
        updateControls();
      }
    }

    function mergedCommands() {
      const merged = new Map(localCommands);
      for (const command of snapshot?.commands || []) {
        if (command && command.id && commandStates[command.state]) merged.set(command.id, command);
      }
      return Array.from(merged.values()).sort((a, b) => {
        const difference = Date.parse(b.requestedAt || "") - Date.parse(a.requestedAt || "");
        return Number.isFinite(difference) && difference ? difference : String(b.id).localeCompare(String(a.id));
      });
    }

    function reconcileCommands(commands) {
      for (const command of commands) {
        const own = ownedCommands.get(command.id);
        if (!own || own.resolved || command.state === "pending") continue;
        own.resolved = true;
        localCommands.set(command.id, command);
        const form = own.form;
        const confirmed = command.state === "confirmed";
        const fallback = commandStates[command.state]?.[0] || "命令失败";
        const message = command.message || fallback;
        // 只接受服务端验证后的同 ID 命令结果，不直接相信保留状态中的 ack。
        if (form && form.pending?.id === command.id) {
          form.pending = null;
          if (confirmed) {
            if (form.revision === own.sentRevision) {
              form.dirty = false;
              if (form.configKey) applyDeviceConfig(form, snapshot?.device?.state);
              formFeedback(form, "confirmed", `已确认 · ${message}`);
            } else formFeedback(form, "dirty", "上一命令已确认 · 新草稿未应用");
          } else formFeedback(form, "failed", `${fallback} · ${message}`);
        }
        if (own.sequence === latestRequestSequence) {
          globalFeedback(confirmed ? "confirmed" : "failed", `${operationNames[command.op] || "命令"}：${message} · ${command.id}`);
        }
      }
    }

    function renderCommands(commands) {
      const visible = commands.slice(0, 12);
      const signature = JSON.stringify(visible);
      if (commandSignature === signature) return;
      commandSignature = signature;
      const body = get("command-body");
      const rows = visible.map(command => {
        const row = document.createElement("tr");
        const time = document.createElement("td");
        time.textContent = dateString(command.requestedAt);
        const operation = document.createElement("td");
        operation.append(document.createTextNode(operationNames[command.op] || command.op || "命令"));
        const id = document.createElement("span");
        id.className = "command-id";
        id.textContent = command.id;
        operation.append(id);
        const state = document.createElement("td");
        const stateBadge = document.createElement("span");
        stateBadge.className = "badge";
        const description = commandStates[command.state] || ["未知", "neutral"];
        stateBadge.textContent = description[0];
        stateBadge.dataset.tone = description[1];
        state.append(stateBadge);
        const message = document.createElement("td");
        message.textContent = command.message || description[0];
        row.append(time, operation, state, message);
        return row;
      });
      if (!rows.length) rows.push(emptyRow(4, "暂无命令记录"));
      body.replaceChildren(...rows);
    }

    function emptyRow(columns, message) {
      const row = document.createElement("tr");
      row.className = "empty-row";
      const cell = document.createElement("td");
      cell.colSpan = columns;
      cell.textContent = message;
      row.append(cell);
      return row;
    }

    function renderState() {
      const device = snapshot?.device || {};
      const state = device.state;
      const mqtt = snapshot?.mqtt;
      text("device-id", device.id || "等待设备标识");
      badge("mqtt-status", mqtt?.connected ? "MQTT 已连接" : "MQTT 未连接", mqtt?.connected ? "good" : "neutral");
      if (device.online === false) {
        text("device-status", "设备离线");
        get("device-dot").dataset.tone = "bad";
      } else if (canControl()) {
        text("device-status", "设备在线 · 控制就绪");
        get("device-dot").dataset.tone = "good";
      } else {
        text("device-status", state ? "控制状态未就绪" : "等待设备状态");
        get("device-dot").dataset.tone = "warn";
      }
      const waitingFirmware = !state && mqtt?.connected && snapshot?.water?.receivedAt && !snapshot.water.fromHistory;
      text("connection-detail", canControl()
        ? `控制状态更新于 ${dateString(device.statusReceivedAt)}`
        : device.online === false ? "设备已离线，等待重新连接后的新鲜控制状态"
        : waitingFirmware ? "水质数据已收到；等待升级固件的控制状态，草稿可编辑"
        : mqtt?.connected ? "等待设备新鲜控制状态，草稿可编辑"
        : `MQTT 未连接${mqtt?.error ? ` · ${mqtt.error}` : ""}，草稿可编辑`);
      const water = snapshot?.water;
      const values = water?.values;
      for (const [name, digits] of [["ph", 2], ["doMgL", 2], ["waterTemp", 1], ["doSaturation", 1]]) {
        text(`metric-${name}`, formatNumber(values?.[name], digits));
      }
      text("water-updated", water?.receivedAt
        ? `${water.fromHistory ? "历史缓存 · " : water.retained ? "保留数据 · " : "实时数据 · "}${dateString(water.receivedAt)}` : "尚未收到水质数据");
      text("server-updated", snapshot?.serverTime ? `服务时间 ${dateString(snapshot.serverTime)}` : "服务时间未知");
      const mode = state?.controlMode;
      text("mode-current", mode === "auto" ? "自动" : mode === "manual" ? "手动" : "未知");
      text("clock-state", state?.clock?.synced === true ? "已校时" : state?.clock?.synced === false ? "未校时" : "校时未知");
      const collection = state?.collection;
      const samplingNames = { run: "持续", pause: "暂停", schedule: "定时" };
      badge("collection-state", collection ? `${samplingNames[collection.mode] || "未知"} · ${collection.active === true ? "采集中" : collection.active === false ? "未采集" : "状态未知"}` : "采集状态未知", collection?.active === true ? "good" : "neutral");
      text("collection-applied", collection
        ? `设备：采样 ${secondsString(collection.sample)} / 发布 ${secondsString(collection.publish)}` : "设备设置未知");
      badge("brush-state", state?.brush?.running === true ? "运行中" : state?.brush?.running === false ? "已停止" : "未知", state?.brush?.running === true ? "good" : "neutral");
      text("brush-remaining", secondsString(state?.brush?.remaining));
      badge("actuator-state", actuatorStateNames[state?.actuator?.state] || "未知", state?.actuator?.state && state.actuator.state !== "stopped" ? "good" : "neutral");
      text("actuator-remaining", secondsString(state?.actuator?.remaining));
      const cleaningState = state?.cleaning?.state;
      const cleaningTone = ["fault", "error"].includes(cleaningState) ? "bad"
        : ["extending", "brushing", "retracting", "settling"].includes(cleaningState) ? "good" : "neutral";
      badge("cleaning-state", cleaningNames[cleaningState] || (cleaningState ? `设备：${cleaningState}` : "清洗状态未知"), cleaningTone);
      text("cleaning-next", secondsString(state?.cleaning?.nextIn));
      const lamp = state?.light;
      for (const color of ["red", "yellow", "green"]) get(`light-${color}`).dataset.lit = String(lamp === color);
      const lampNames = { red: "红灯亮", yellow: "黄灯亮", green: "绿灯亮", off: "全部熄灭" };
      text("light-label", lampNames[lamp] || "灯态未知");
      for (const step of all("[data-cleaning-step]")) step.dataset.active = String(state?.cleaning?.state === step.dataset.cleaningStep);
      for (const target of ["brush", "actuator"]) {
        const plan = state?.[`${target}Plan`];
        const action = target === "actuator" ? (actuatorDirections[plan?.action] || "未知") : "刷洗";
        text(`${target}-plan-applied`, !plan ? "设备计划未知" : plan.type === "off" ? "设备：计划已关闭"
          : plan.type === "daily" ? `设备：每天 ${timeString(plan.start)} · ${action} ${secondsString(plan.duration)}`
          : `设备：间隔 ${secondsString(plan.interval)} · ${action} ${secondsString(plan.duration)}`);
      }
      text("cleaning-applied", state?.cleaningConfig
        ? `设备：${state.cleaningConfig.enabled ? "已启用" : "已关闭"} · 间隔 ${secondsString(state.cleaningConfig.interval)}` : "设备配置未知");
      initializeForms(state);
      const commands = mergedCommands();
      reconcileCommands(commands);
      renderCommands(commands);
      updateControls();
    }

    async function refresh() {
      if (refreshRequest) return refreshRequest;
      refreshRequest = (async () => {
        try {
          snapshot = await requestJSON("/api/status");
          // 超出最近记录展示范围的旧请求仍按 ID 查询，防止表单永远停在等待。
          const visibleIds = new Set((snapshot.commands || []).map(command => command.id));
          for (const [id, owned] of ownedCommands) {
            if (owned.resolved || visibleIds.has(id)) continue;
            const result = await requestJSON(`/api/commands/${encodeURIComponent(id)}`);
            if (result.command?.id === id) localCommands.set(id, result.command);
          }
          serverReachable = true;
          hasSuccess = true;
          lastSuccess = now();
          renderState();
          if (!ownedCommands.size) globalFeedback("uninitialized", canControl()
            ? "控制就绪 · 命令需设备确认"
            : "等待设备控制状态；编辑草稿不会发送命令");
        } catch (error) {
          serverReachable = false;
          text("device-status", "服务连接中断");
          text("connection-detail", "状态读取失败，显示上次数据；草稿已保留");
          get("device-dot").dataset.tone = "bad";
          globalFeedback("failed", `状态读取失败：${error.name === "AbortError" ? "请求超时" : error.message}`);
          updateControls();
        } finally { refreshRequest = null; }
      })();
      return refreshRequest;
    }

    async function loadHistory() {
      if (historyRequest) return historyRequest;
      get("refresh-history").disabled = true;
      historyRequest = (async () => {
        try {
          const data = await requestJSON("/api/history?limit=200");
          if (!Array.isArray(data.rows)) throw new Error("历史返回格式错误");
          const rows = data.rows.map(record => {
            const row = document.createElement("tr");
            for (const value of [
              dateString(record.receivedAt), formatNumber(record.ph, 2),
              formatNumber(record.doMgL, 2), formatNumber(record.waterTemp, 1),
              formatNumber(record.doSaturation, 1), record.fromHistory ? "历史缓存" : record.retained ? "保留消息" : "实时消息"
            ]) {
              const cell = document.createElement("td");
              cell.textContent = value;
              row.append(cell);
            }
            return row;
          });
          get("history-body").replaceChildren(...(rows.length ? rows : [emptyRow(6, "暂无水质历史")]));
          text("history-meta", `最近 ${data.rows.length} 条 · ${dateString(new Date(now()).toISOString())}`);
          text("history-feedback", "");
          historyLoadedAt = now();
        } catch (error) {
          text("history-feedback", `历史加载失败：${error.message}`);
        } finally {
          historyRequest = null;
          get("refresh-history").disabled = false;
        }
      })();
      return historyRequest;
    }

    const modal = get("time-modal");
    const hourInput = get("time-hour");
    const minuteInput = get("time-minute");
    let timeTarget = null;
    let modalOpener = null;
    const wheels = [];
    function selectWheel(wheel, value, scroll) {
      for (const item of wheel.items) item.setAttribute("aria-selected", String(Number(item.dataset.value) === value));
      wheel.element.setAttribute("aria-activedescendant", `${wheel.element.id}-${value}`);
      if (scroll) wheel.element.scrollTop = value * 40;
    }
    function syncWheel(wheel) {
      const value = Number(wheel.input.value);
      if (wheel.input.value !== "" && Number.isInteger(value) && value >= 0 && value < wheel.count) selectWheel(wheel, value, true);
    }
    for (const [id, count, input] of [["hour-wheel", 24, hourInput], ["minute-wheel", 60, minuteInput]]) {
      const element = get(id);
      const container = element.querySelector(".wheel-options");
      const items = [];
      const wheel = { element, count, input, items };
      for (let value = 0; value < count; value++) {
        const item = document.createElement("button");
        item.type = "button";
        item.className = "wheel-option";
        item.id = `${id}-${value}`;
        item.dataset.value = value;
        item.setAttribute("role", "option");
        item.setAttribute("tabindex", "-1");
        item.setAttribute("aria-selected", "false");
        item.textContent = pad(value);
        item.addEventListener("click", () => {
          input.value = String(value);
          selectWheel(wheel, value, true);
          text("time-error", "");
        });
        items.push(item);
        container.append(item);
      }
      element.addEventListener("scroll", () => {
        if (modal.hidden) return;
        const value = Math.max(0, Math.min(count - 1, Math.round(element.scrollTop / 40)));
        input.value = String(value);
        selectWheel(wheel, value, false);
      });
      element.addEventListener("keydown", event => {
        if (!["ArrowUp", "ArrowDown", "Home", "End"].includes(event.key)) return;
        event.preventDefault();
        const value = event.key === "Home" ? 0 : event.key === "End" ? count - 1
          : Math.max(0, Math.min(count - 1, Number(input.value) + (event.key === "ArrowDown" ? 1 : -1)));
        input.value = String(value);
        selectWheel(wheel, value, true);
      });
      input.addEventListener("input", () => syncWheel(wheel));
      input.addEventListener("keydown", event => {
        if (event.key === "Enter") { event.preventDefault(); confirmTime(); }
      });
      wheels.push(wheel);
    }
    function closeTime() {
      if (typeof modal.close === "function" && modal.open) modal.close();
      modal.hidden = true;
      timeTarget = null;
      modalOpener?.focus();
    }
    function confirmTime() {
      if (!timeTarget) return;
      const hour = String(hourInput.value).trim();
      const minute = String(minuteInput.value).trim();
      if (!/^\d{1,2}$/.test(hour) || !/^\d{1,2}$/.test(minute) || Number(hour) > 23 || Number(minute) > 59) {
        text("time-error", "小时为 0–23，分钟为 0–59");
        return;
      }
      const value = `${pad(Number(hour))}:${pad(Number(minute))}`;
      if (timeTarget.value !== value) {
        timeTarget.value = value;
        const form = forms.get(timeTarget.closest("form").id);
        markEdited(form);
      }
      closeTime();
    }
    for (const opener of all("[data-time-for]")) opener.addEventListener("click", () => {
      timeTarget = get(opener.dataset.timeFor);
      modalOpener = opener;
      let minutes;
      try { minutes = timeMinutes(timeTarget.value); } catch { minutes = 480; }
      hourInput.value = String(Math.floor(minutes / 60));
      minuteInput.value = String(minutes % 60);
      text("time-title", opener.title.replace(/^选择/, ""));
      text("time-error", "");
      modal.hidden = false;
      if (typeof modal.showModal === "function" && !modal.open) modal.showModal();
      for (const wheel of wheels) syncWheel(wheel);
      hourInput.focus();
    });
    get("time-cancel").addEventListener("click", closeTime);
    get("time-confirm").addEventListener("click", confirmTime);
    modal.addEventListener("cancel", event => { event.preventDefault(); closeTime(); });
    modal.addEventListener("keydown", event => {
      if (event.key === "Escape") { event.preventDefault(); closeTime(); }
    });
    modal.addEventListener("click", event => { if (event.target === modal) closeTime(); });

    for (const button of all("[data-action]")) button.addEventListener("click", async () => {
      if (button.disabled) return;
      const action = button.dataset.action;
      const form = forms.get(button.closest("form")?.id);
      let payload;
      try {
        if (action === "stop" || action === "query") payload = { op: action };
        else {
          const [op, commandAction] = action.split("-");
          payload = { op, action: commandAction };
          if (op === "brush" && commandAction === "start" || op === "actuator" && commandAction !== "stop") {
            payload.duration = integer(form, "duration", 1, 30, "运行时长");
          }
        }
        await sendCommand(payload, form);
      } catch (error) {
        if (form) formFeedback(form, "failed", error.message);
        globalFeedback("failed", error.message);
      }
    });
    get("refresh-status").addEventListener("click", refresh);
    get("refresh-history").addEventListener("click", loadHistory);
    document.addEventListener("visibilitychange", () => {
      if (document.visibilityState === "visible") {
        serverReachable = false;
        updateControls();
        refresh();
      }
    });
    updateControls();

    function start() {
      if (pollStarted) return;
      pollStarted = true;
      refresh();
      loadHistory();
      const poll = async () => {
        updateControls();
        await refresh();
        if (now() - historyLoadedAt > 30000 && !document.hidden) loadHistory();
        setTimeout(poll, 2000);
      };
      setTimeout(poll, 2000);
    }
    return { start, refresh, loadHistory };
  }

  if (typeof module !== "undefined" && module.exports) module.exports = { createApp };
  else {
    const boot = () => createApp({ document: root.document, fetch: root.fetch.bind(root) }).start();
    if (root.document.readyState === "loading") root.document.addEventListener("DOMContentLoaded", boot, { once: true });
    else boot();
  }
})(typeof window !== "undefined" ? window : globalThis);
