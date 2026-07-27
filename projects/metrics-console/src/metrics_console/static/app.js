/* Metrics Console client: time range + differential updates */

const $ = (sel) => document.querySelector(sel);

const TITLES = {
  overview: ["概览", "窗口计数 · 差分轮询 · Quack 只读"],
  api: ["API 延迟", "路径聚合与请求样本"],
  lifecycle: ["生命周期", "e2ed 启停与 daemon ticks"],
  components: ["组件", "component_ops 窗口事件"],
  explore: ["表浏览", "category 库只读采样"],
  sql: ["SQL", "只读查询 · ⌘/Ctrl+Enter 运行"],
};

const KPI_DEF = [
  ["API 请求", "orch_api", "blue", "api_requests"],
  ["Orch 事件", "orch_events", "indigo", "events"],
  ["组件操作", "orch_components", "purple", "component_ops"],
  ["场景步骤", "orch_scenarios", "pink", "scenario_steps"],
  ["生命周期", "e2ed_lifecycle", "green", "service_lifecycle"],
  ["Daemon ticks", "e2ed_ticks", "orange", "daemon_ticks"],
  ["e2ed 事件", "e2ed_events", "blue", "e2ed.events"],
  ["ops 事件", "ops_events", "indigo", "ops.events"],
];

const state = {
  view: "overview",
  range: "1h",
  live: true,
  cursor: null,
  feeds: {
    api: [],
    lifecycle: [],
    ticks: [],
    components: [],
  },
  tables: {},
  latency: [],
  paths: [],
  loading: false,
  abort: null,
  pollTimer: null,
  healthTimer: null,
  lastQueryMs: null,
  lastMode: null,
};

const FEED_LIMIT = 60;

async function api(path, opts = {}) {
  const ctrl = opts.signal;
  const res = await fetch(path, {
    headers: { "content-type": "application/json" },
    ...opts,
    signal: ctrl,
  });
  const data = await res.json().catch(() => ({}));
  if (!res.ok) {
    const detail = data.detail;
    const msg =
      typeof detail === "string"
        ? detail
        : Array.isArray(detail)
          ? detail.map((d) => d.msg || d).join("; ")
          : res.statusText || "request failed";
    throw new Error(msg);
  }
  return data;
}

function toast(msg, err) {
  const el = $("#toast");
  if (!el) return;
  el.hidden = false;
  el.textContent = msg;
  el.classList.toggle("err", !!err);
  el.classList.add("show");
  clearTimeout(el._t);
  el._t = setTimeout(() => {
    el.classList.remove("show");
  }, 2200);
}

function fmtNum(n) {
  if (n == null || typeof n === "object") return "—";
  if (typeof n === "number") {
    if (Number.isInteger(n)) return n.toLocaleString();
    if (Math.abs(n) >= 100) return n.toFixed(1);
    if (Math.abs(n) >= 10) return n.toFixed(2);
    return n.toFixed(3);
  }
  return String(n);
}

function fmtTs(v) {
  if (!v) return "—";
  try {
    const d = new Date(v);
    if (Number.isNaN(d.getTime())) return String(v);
    return d.toLocaleString(undefined, {
      month: "2-digit",
      day: "2-digit",
      hour: "2-digit",
      minute: "2-digit",
      second: "2-digit",
      hour12: false,
    });
  } catch {
    return String(v);
  }
}

function fmtAxisTime(v) {
  if (!v) return "";
  try {
    const d = new Date(v);
    if (Number.isNaN(d.getTime())) return String(v).slice(11, 16);
    return d.toLocaleTimeString(undefined, {
      hour: "2-digit",
      minute: "2-digit",
      hour12: false,
    });
  } catch {
    return String(v).slice(0, 16);
  }
}

function cell(v) {
  if (v == null) return "—";
  if (typeof v === "object") return JSON.stringify(v);
  return String(v);
}

/** HTTP status → short reason; prefer writer-provided error/attrs. */
const HTTP_REASON = {
  400: "Bad Request",
  401: "Unauthorized",
  403: "Forbidden",
  404: "Not Found",
  405: "Method Not Allowed",
  408: "Request Timeout",
  409: "Conflict",
  422: "Unprocessable",
  429: "Too Many Requests",
  500: "Internal Server Error",
  502: "Bad Gateway",
  503: "Service Unavailable",
  504: "Gateway Timeout",
};

function statusReason(row) {
  if (!row) return "";
  const err = row.error;
  if (err != null && String(err).trim()) return String(err).trim();
  let attrs = row.attrs;
  if (attrs) {
    try {
      if (typeof attrs === "string") attrs = JSON.parse(attrs);
      if (attrs && typeof attrs === "object") {
        const pick =
          attrs.error ||
          attrs.message ||
          attrs.detail ||
          attrs.reason ||
          attrs.msg;
        if (pick) return String(pick);
      }
    } catch {
      if (String(attrs).trim() && String(attrs) !== "null") return String(attrs);
    }
  }
  const st = Number(row.status);
  if (!Number.isFinite(st) || st < 400) return "";
  return HTTP_REASON[st] || `HTTP ${st}`;
}

function escapeHtml(s) {
  return String(s)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}

function escapeAttr(s) {
  return escapeHtml(s).replaceAll("'", "&#39;");
}

function rowKey(r, cols) {
  if (r.ts) return String(r.ts) + "|" + (r.path || r.service || r.action || "");
  return cols.map((c) => String(r[c] ?? "")).join("|");
}

function mergeFeed(oldRows, added, limit) {
  if (!Array.isArray(added) || !added.length) return { rows: oldRows || [], newKeys: new Set() };
  const cols = Object.keys(added[0] || {});
  const seen = new Set((oldRows || []).map((r) => rowKey(r, cols)));
  const newKeys = new Set();
  const prepend = [];
  // API returns newest first
  for (const r of added) {
    const k = rowKey(r, cols);
    if (seen.has(k)) continue;
    seen.add(k);
    newKeys.add(k);
    prepend.push(r);
  }
  const merged = prepend.concat(oldRows || []).slice(0, limit);
  return { rows: merged, newKeys };
}

function renderTable(el, rows, columns, opts = {}) {
  if (!el) return;
  if (!rows || rows.error) {
    el.innerHTML = `<div class="error" style="padding:12px">${escapeHtml(rows?.error || "no data")}</div>`;
    return;
  }
  if (!rows.length) {
    el.innerHTML = `<div class="muted" style="padding:12px">暂无数据</div>`;
    return;
  }
  const cols = columns || Object.keys(rows[0]);
  const newKeys = opts.newKeys || new Set();
  const thead = cols
    .map((c) => {
      const label = c === "reason" ? "原因" : c === "duration_ms" ? "ms" : c;
      return `<th>${escapeHtml(label)}</th>`;
    })
    .join("");
  const body = rows
    .map((r) => {
      const k = rowKey(r, cols);
      const isNew = newKeys.has(k);
      const tds = cols
        .map((c) => {
          let val = r[c];
          if (c === "ts" || c === "bucket" || String(c).endsWith("_at")) val = fmtTs(val);
          if (c === "duration_ms" && typeof val === "number") {
            return `<td title="${escapeAttr(String(val))}">${escapeHtml(fmtNum(val))}</td>`;
          }
          if (c === "ok" && typeof val === "boolean") {
            return `<td><span class="badge ${val ? "ok" : "err"}">${val ? "ok" : "fail"}</span></td>`;
          }
          if (c === "status") {
            const st = Number(val);
            const cls = Number.isFinite(st) && st >= 400 ? "err" : "ok";
            const reason = statusReason(r);
            if (reason && Number.isFinite(st) && st >= 400) {
              return `<td><div class="status-cell"><span class="badge ${cls}">${escapeHtml(String(val))}</span><span class="reason" title="${escapeAttr(reason)}">${escapeHtml(reason)}</span></div></td>`;
            }
            return `<td><span class="badge ${cls}">${escapeHtml(cell(val))}</span></td>`;
          }
          if (c === "reason") {
            const reason = statusReason(r) || val;
            if (!reason) {
              return `<td><span class="reason empty">—</span></td>`;
            }
            return `<td title="${escapeAttr(reason)}"><span class="reason">${escapeHtml(reason)}</span></td>`;
          }
          return `<td title="${escapeAttr(cell(val))}">${escapeHtml(cell(val))}</td>`;
        })
        .join("");
      return `<tr class="${isNew ? "is-new" : ""}" data-k="${escapeAttr(k)}">${tds}</tr>`;
    })
    .join("");
  el.innerHTML = `<table class="data"><thead><tr>${thead}</tr></thead><tbody>${body}</tbody></table>`;
  if (opts.flash) {
    el.classList.remove("flash");
    // reflow
    void el.offsetWidth;
    el.classList.add("flash");
  }
}

function renderKpis(tables) {
  const root = $("#kpiGrid");
  if (!root) return;
  root.innerHTML = KPI_DEF.map(([label, key, tone, hint]) => {
    const value = tables?.[key];
    return `
      <div class="card" data-tone="${tone}">
        <div class="label">${label}</div>
        <div class="value">${fmtNum(value)}</div>
        <div class="hint">${hint} · ${state.range}</div>
      </div>`;
  }).join("");
}

function renderKpiSkeleton() {
  const root = $("#kpiGrid");
  if (!root || root.children.length) return;
  root.innerHTML = KPI_DEF.map(
    ([label, , tone]) => `
    <div class="card skeleton" data-tone="${tone}">
      <div class="label">${label}</div>
      <div class="value">0000</div>
      <div class="hint">loading</div>
    </div>`
  ).join("");
}

function niceMax(raw) {
  if (!(raw > 0)) return 1;
  const exp = Math.pow(10, Math.floor(Math.log10(raw)));
  const n = raw / exp;
  const nice = n <= 1 ? 1 : n <= 2 ? 2 : n <= 5 ? 5 : 10;
  return nice * exp;
}

function renderChart(latency) {
  const svg = $("#latencyChart");
  const empty = $("#latencyEmpty");
  const cap = $("#latencyCaption");
  const chip = $("#latencyChip");
  if (!svg) return;

  const series = Array.isArray(latency) ? latency : [];
  if (!series.length || series.error) {
    svg.innerHTML = "";
    empty?.classList.remove("hidden");
    if (cap) cap.textContent = series.error || "avg / p95 · ms";
    if (chip) chip.textContent = "—";
    return;
  }
  empty?.classList.add("hidden");

  const W = 720;
  const H = 220;
  const pad = { t: 16, r: 16, b: 36, l: 48 };
  const plotW = W - pad.l - pad.r;
  const plotH = H - pad.t - pad.b;

  const avgs = series.map((r) => Number(r.avg_ms) || 0);
  const p95s = series.map((r) => Number(r.p95_ms) || 0);
  const yMax = niceMax(Math.max(...avgs, ...p95s, 0.001) * 1.08);
  const n = series.length;
  const xAt = (i) => {
    if (n <= 1) return pad.l + plotW / 2;
    return pad.l + (i / (n - 1)) * plotW;
  };
  const yAt = (v) => pad.t + (1 - Math.min(v, yMax) / yMax) * plotH;

  const poly = (vals) => {
    if (n === 1) {
      // single point: short horizontal tick so it doesn't look like a filled block
      const x0 = xAt(0) - 18;
      const x1 = xAt(0) + 18;
      const y = yAt(vals[0]);
      return `M${x0.toFixed(1)},${y.toFixed(1)} L${x1.toFixed(1)},${y.toFixed(1)}`;
    }
    return vals
      .map((v, i) => `${i === 0 ? "M" : "L"}${xAt(i).toFixed(1)},${yAt(v).toFixed(1)}`)
      .join(" ");
  };

  const areaPath =
    n <= 1
      ? ""
      : poly(avgs) +
        ` L${xAt(n - 1).toFixed(1)},${(pad.t + plotH).toFixed(1)}` +
        ` L${xAt(0).toFixed(1)},${(pad.t + plotH).toFixed(1)} Z`;

  // Y grid + labels (4 ticks)
  const yTicks = 4;
  let grid = "";
  let yLabels = "";
  for (let i = 0; i <= yTicks; i++) {
    const v = (yMax * i) / yTicks;
    const y = yAt(v);
    grid += `<line class="grid-line" x1="${pad.l}" y1="${y.toFixed(1)}" x2="${(pad.l + plotW).toFixed(1)}" y2="${y.toFixed(1)}"/>`;
    const label = v >= 100 ? v.toFixed(0) : v >= 10 ? v.toFixed(1) : v.toFixed(2);
    yLabels += `<text class="tick-label" x="${pad.l - 6}" y="${(y + 3).toFixed(1)}" text-anchor="end">${label}</text>`;
  }

  // X labels: first / mid / last (more if many points)
  const xIdx = new Set([0, n - 1]);
  if (n >= 3) xIdx.add(Math.floor((n - 1) / 2));
  if (n >= 8) {
    xIdx.add(Math.floor((n - 1) / 4));
    xIdx.add(Math.floor((3 * (n - 1)) / 4));
  }
  let xLabels = "";
  [...xIdx]
    .sort((a, b) => a - b)
    .forEach((i) => {
      const label = fmtAxisTime(series[i].bucket || series[i].minute);
      xLabels += `<text class="tick-label" x="${xAt(i).toFixed(1)}" y="${(H - 12).toFixed(1)}" text-anchor="middle">${escapeHtml(label)}</text>`;
    });

  // dots only when sparse (readable)
  let dots = "";
  if (n <= 48) {
    for (let i = 0; i < n; i++) {
      const title = `${fmtTs(series[i].bucket)} · avg ${fmtNum(avgs[i])} · p95 ${fmtNum(p95s[i])} · n=${fmtNum(series[i].n)}`;
      dots += `<circle class="dot-p95" cx="${xAt(i).toFixed(1)}" cy="${yAt(p95s[i]).toFixed(1)}" r="2.5"><title>${escapeAttr(title)}</title></circle>`;
      dots += `<circle class="dot-avg" cx="${xAt(i).toFixed(1)}" cy="${yAt(avgs[i]).toFixed(1)}" r="2.8"><title>${escapeAttr(title)}</title></circle>`;
    }
  }

  svg.setAttribute("viewBox", `0 0 ${W} ${H}`);
  svg.innerHTML = `
    <defs>
      <linearGradient id="gradAvg" x1="0" y1="0" x2="0" y2="1">
        <stop offset="0%" stop-color="var(--accent)" stop-opacity="0.22"/>
        <stop offset="100%" stop-color="var(--accent)" stop-opacity="0.02"/>
      </linearGradient>
    </defs>
    ${grid}
    <line class="axis-line" x1="${pad.l}" y1="${pad.t}" x2="${pad.l}" y2="${pad.t + plotH}"/>
    <line class="axis-line" x1="${pad.l}" y1="${pad.t + plotH}" x2="${pad.l + plotW}" y2="${pad.t + plotH}"/>
    ${yLabels}
    ${xLabels}
    <text class="axis-title" x="12" y="${(pad.t + plotH / 2).toFixed(1)}" text-anchor="middle" transform="rotate(-90 12 ${(pad.t + plotH / 2).toFixed(1)})">ms</text>
    ${areaPath ? `<path class="area" d="${areaPath}"></path>` : ""}
    <path class="line-p95" d="${poly(p95s)}"></path>
    <path class="line-avg" d="${poly(avgs)}"></path>
    ${dots}
  `;

  const last = series[series.length - 1];
  if (cap) {
    cap.textContent = `avg ${fmtNum(last.avg_ms)} ms · p95 ${fmtNum(last.p95_ms)} ms · n=${fmtNum(last.n)} · 纵轴 ms`;
  }
  if (chip) chip.textContent = `${series.length} pts`;
}

function applySnapshot(data) {
  state.tables = data.tables || {};
  state.latency = data.latency || [];
  state.paths = data.paths || [];
  state.cursor = data.cursor || null;
  state.lastQueryMs = data.query_ms;
  state.lastMode = data.mode || "snapshot";

  const recent = data.recent || {};
  state.feeds.api = Array.isArray(recent.api) ? recent.api : [];
  state.feeds.lifecycle = Array.isArray(recent.lifecycle) ? recent.lifecycle : [];
  state.feeds.ticks = Array.isArray(recent.ticks) ? recent.ticks : [];
  state.feeds.components = Array.isArray(recent.components) ? recent.components : [];

  renderKpis(state.tables);
  renderChart(state.latency);
  paintFeeds({ flash: false });
  updateMeta();
}

function applyDelta(data) {
  state.lastQueryMs = data.query_ms;
  state.lastMode = "delta";
  if (data.cursor) state.cursor = data.cursor;

  // merge window KPIs that came back
  if (data.tables && typeof data.tables === "object") {
    state.tables = { ...state.tables, ...data.tables };
    renderKpis(state.tables);
  }
  if (Array.isArray(data.latency)) {
    state.latency = data.latency;
    renderChart(state.latency);
  }
  if (Array.isArray(data.paths)) {
    state.paths = data.paths;
  }

  const added = data.added || {};
  const news = {};
  for (const key of ["api", "lifecycle", "ticks", "components"]) {
    const m = mergeFeed(state.feeds[key], added[key], FEED_LIMIT);
    state.feeds[key] = m.rows;
    news[key] = m.newKeys;
  }
  paintFeeds({ flash: true, news });
  updateMeta(Object.values(news).reduce((n, s) => n + s.size, 0));
}

function paintFeeds({ flash = false, news = {} } = {}) {
  // reason is derived client-side from error/attrs/HTTP phrase
  const apiCols = ["ts", "method", "path", "status", "reason", "duration_ms"];
  const lifeCols = ["ts", "service", "action", "ok", "pid", "error"];
  const tickCols = [
    "ts",
    "services_total",
    "services_alive",
    "services_unhealthy",
    "poll_interval_secs",
  ];
  const pathCols = ["path", "n", "avg_ms", "p95_ms", "max_ms"];

  renderTable($("#recentApi"), state.feeds.api, apiCols, {
    flash,
    newKeys: news.api,
  });
  renderTable($("#apiTable"), state.feeds.api, apiCols, {
    flash,
    newKeys: news.api,
  });
  renderTable($("#pathTable"), state.paths, pathCols, { flash });
  renderTable($("#pathTableFull"), state.paths, pathCols, { flash });
  renderTable($("#lifeTable"), state.feeds.lifecycle, lifeCols, {
    flash,
    newKeys: news.lifecycle,
  });
  renderTable($("#tickTable"), state.feeds.ticks, tickCols, {
    flash,
    newKeys: news.ticks,
  });

  // components: dynamic columns
  const comp = state.feeds.components;
  const compCols =
    Array.isArray(comp) && comp[0]
      ? Object.keys(comp[0]).filter((k) => k !== "error" || true).slice(0, 10)
      : ["ts"];
  renderTable($("#compTable"), comp, compCols, {
    flash,
    newKeys: news.components,
  });

  const apiChip = $("#apiCountChip");
  const pathChip = $("#pathCountChip");
  if (apiChip) apiChip.textContent = String(state.feeds.api.length);
  if (pathChip) pathChip.textContent = String((state.paths || []).length);
}

function updateMeta(addedCount) {
  const poll = $("#pollMeta");
  if (!poll) return;
  const mode = state.lastMode === "delta" ? "Δ" : "全量";
  const ms = state.lastQueryMs != null ? `${state.lastQueryMs} ms` : "—";
  const extra =
    addedCount != null && addedCount > 0 ? ` · +${addedCount}` : "";
  poll.textContent = `${state.range} · ${mode} ${ms}${extra}${state.live ? " · live" : ""}`;
}

async function refreshHealth() {
  const dot = $("#healthDot");
  const text = $("#healthText");
  try {
    const h = await api("/api/health");
    if (dot) dot.className = "dot ok";
    if (text) text.textContent = `${h.uri} · ${h.latency_ms} ms`;
  } catch (e) {
    if (dot) dot.className = "dot err";
    if (text) text.textContent = e.message || "offline";
  }
}

async function loadSnapshot({ quiet = false } = {}) {
  if (state.abort) state.abort.abort();
  const ac = new AbortController();
  state.abort = ac;
  state.loading = true;
  if (!quiet) renderKpiSkeleton();
  $("#btnRefresh")?.setAttribute("disabled", "true");
  try {
    const data = await api(`/api/snapshot?range=${encodeURIComponent(state.range)}`, {
      signal: ac.signal,
    });
    applySnapshot(data);
    if (!quiet) toast(`全量 ${data.query_ms} ms · ${state.range}`);
  } catch (e) {
    if (e.name === "AbortError") return;
    toast(e.message || String(e), true);
  } finally {
    state.loading = false;
    if (state.abort === ac) state.abort = null;
    $("#btnRefresh")?.removeAttribute("disabled");
  }
}

async function loadDelta() {
  if (!state.cursor || state.loading || state.abort) return;
  try {
    const q = new URLSearchParams({
      range: state.range,
      since: state.cursor,
    });
    const data = await api(`/api/delta?${q}`);
    applyDelta(data);
  } catch (e) {
    // soft-fail: next full refresh will recover
    console.warn("delta failed", e);
  }
}

function setRange(range) {
  if (state.range === range) return;
  state.range = range;
  state.cursor = null;
  document.querySelectorAll("#rangeSeg button").forEach((b) => {
    b.classList.toggle("active", b.dataset.range === range);
  });
  loadSnapshot();
}

function setLive(on) {
  state.live = !!on;
  const btn = $("#btnLive");
  if (btn) btn.setAttribute("aria-pressed", state.live ? "true" : "false");
  schedulePoll();
  updateMeta();
}

function schedulePoll() {
  clearInterval(state.pollTimer);
  if (!state.live) return;
  // Differential poll: cheap for multi-component high frequency
  state.pollTimer = setInterval(() => {
    if (document.hidden) return;
    if (["overview", "api", "lifecycle", "components"].includes(state.view)) {
      loadDelta();
    }
  }, 5000);
}

function setView(name) {
  state.view = name;
  document.querySelectorAll(".nav button").forEach((b) => {
    b.classList.toggle("active", b.dataset.view === name);
  });
  ["overview", "api", "lifecycle", "components", "explore", "sql"].forEach((v) => {
    const el = $(`#view-${v}`);
    if (el) el.classList.toggle("hidden", v !== name);
  });
  const t = TITLES[name] || [name, ""];
  const title = $("#pageTitle");
  const sub = $("#pageSub");
  if (title) title.textContent = t[0];
  if (sub) sub.textContent = t[1];
}

async function loadTablesCatalog() {
  const data = await api("/api/tables");
  const tables = data.tables || [];
  const sel = $("#tableSelect");
  if (!sel) return;
  sel.innerHTML = tables
    .map((t) => {
      const db = t.database || "";
      const name = t.table_name || t.name;
      const cat = db.startsWith("cat_")
        ? db.slice(4)
        : db === "master"
          ? "meta"
          : db;
      const value = `${cat}/${name}`;
      return `<option value="${escapeAttr(value)}">${escapeHtml(db)}.${escapeHtml(name)}</option>`;
    })
    .join("");
}

async function loadSelectedTable() {
  const value = $("#tableSelect")?.value;
  if (!value) return;
  const [category, table] = value.split("/");
  const data = await api(
    `/api/table/${encodeURIComponent(category)}/${encodeURIComponent(table)}?limit=100&range=${encodeURIComponent(state.range)}`
  );
  renderTable($("#exploreTable"), data.rows);
}

async function runSql(sql) {
  $("#sqlError")?.classList.add("hidden");
  if ($("#sqlMeta")) $("#sqlMeta").textContent = "运行中…";
  try {
    const data = await api("/api/sql", {
      method: "POST",
      body: JSON.stringify({ sql, limit: 200 }),
    });
    renderTable($("#sqlTable"), data.rows, data.columns);
    if ($("#sqlMeta")) $("#sqlMeta").textContent = `${data.count} 行`;
  } catch (e) {
    if ($("#sqlError")) {
      $("#sqlError").textContent = e.message || String(e);
      $("#sqlError").classList.remove("hidden");
    }
    if ($("#sqlMeta")) $("#sqlMeta").textContent = "失败";
  }
}

function bind() {
  $("#nav")?.addEventListener("click", (e) => {
    const btn = e.target.closest("button[data-view]");
    if (!btn) return;
    setView(btn.dataset.view);
  });

  $("#rangeSeg")?.addEventListener("click", (e) => {
    const btn = e.target.closest("button[data-range]");
    if (!btn) return;
    setRange(btn.dataset.range);
  });

  $("#btnLive")?.addEventListener("click", () => setLive(!state.live));
  $("#btnRefresh")?.addEventListener("click", () => loadSnapshot());

  $("#btnLoadTable")?.addEventListener("click", () =>
    loadSelectedTable().catch((e) => toast(e.message, true))
  );
  $("#btnRunSql")?.addEventListener("click", () => runSql($("#sqlInput").value));
  $("#btnSqlApi")?.addEventListener("click", () => {
    $("#sqlInput").value = `SELECT path,
  count(*) AS n,
  avg(duration_ms) AS avg_ms,
  quantile_cont(duration_ms, 0.95) AS p95_ms
FROM cat_orchestrator.api_requests
WHERE cast(ts AS TIMESTAMP) > cast(now() AS TIMESTAMP) - INTERVAL 1 HOUR
GROUP BY 1
ORDER BY n DESC
LIMIT 30`;
  });
  $("#btnSqlLife")?.addEventListener("click", () => {
    $("#sqlInput").value = `SELECT ts, service, action, ok, pid, error
FROM cat_e2ed.service_lifecycle
ORDER BY ts DESC
LIMIT 50`;
  });
  $("#sqlInput")?.addEventListener("keydown", (e) => {
    if ((e.metaKey || e.ctrlKey) && e.key === "Enter") {
      e.preventDefault();
      runSql($("#sqlInput").value);
    }
  });

  document.addEventListener("visibilitychange", () => {
    if (!document.hidden && state.live && state.cursor) loadDelta();
  });
}

async function boot() {
  bind();
  setView("overview");
  setLive(true);
  await refreshHealth();
  await loadSnapshot({ quiet: true });
  loadTablesCatalog().catch(() => {});
  state.healthTimer = setInterval(() => {
    if (!document.hidden) refreshHealth().catch(() => {});
  }, 20000);
  schedulePoll();
}

boot().catch((e) => toast(e.message || String(e), true));
