const $ = (sel) => document.querySelector(sel);

const state = {
  overview: null,
  tables: [],
  view: "overview",
};

async function api(path, opts) {
  const res = await fetch(path, {
    headers: { "content-type": "application/json" },
    ...opts,
  });
  const data = await res.json().catch(() => ({}));
  if (!res.ok) {
    throw new Error(data.detail || res.statusText || "request failed");
  }
  return data;
}

function fmtNum(n) {
  if (n == null || typeof n === "object") return "—";
  if (typeof n === "number") {
    return Number.isInteger(n) ? n.toLocaleString() : n.toFixed(2);
  }
  return String(n);
}

function fmtTs(v) {
  if (!v) return "—";
  try {
    const d = new Date(v);
    if (Number.isNaN(d.getTime())) return String(v);
    return d.toLocaleString();
  } catch {
    return String(v);
  }
}

function cell(v) {
  if (v == null) return "—";
  if (typeof v === "object") return JSON.stringify(v);
  return String(v);
}

function renderTable(el, rows, columns) {
  if (!el) return;
  if (!rows || rows.error) {
    el.innerHTML = `<div class="error" style="padding:12px">${rows?.error || "no data"}</div>`;
    return;
  }
  if (!rows.length) {
    el.innerHTML = `<div class="muted" style="padding:12px">No rows</div>`;
    return;
  }
  const cols = columns || Object.keys(rows[0]);
  const thead = cols.map((c) => `<th>${escapeHtml(c)}</th>`).join("");
  const body = rows
    .map((r) => {
      const tds = cols
        .map((c) => {
          let val = r[c];
          if (c === "ts" || String(c).endsWith("_at")) val = fmtTs(val);
          if (c === "ok" && typeof val === "boolean") {
            return `<td><span class="badge ${val ? "ok" : "err"}">${val ? "ok" : "fail"}</span></td>`;
          }
          if (c === "status" && typeof val === "number") {
            const cls = val >= 400 ? "err" : "ok";
            return `<td><span class="badge ${cls}">${val}</span></td>`;
          }
          return `<td title="${escapeAttr(cell(val))}">${escapeHtml(cell(val))}</td>`;
        })
        .join("");
      return `<tr>${tds}</tr>`;
    })
    .join("");
  el.innerHTML = `<table class="data"><thead><tr>${thead}</tr></thead><tbody>${body}</tbody></table>`;
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

function renderKpis(tables) {
  const items = [
    ["API requests", tables.orch_api, "cat_orchestrator.api_requests"],
    ["Orch events", tables.orch_events, "cat_orchestrator.events"],
    ["Lifecycle", tables.e2ed_lifecycle, "cat_e2ed.service_lifecycle"],
    ["Daemon ticks", tables.e2ed_ticks, "cat_e2ed.daemon_ticks"],
  ];
  $("#kpiGrid").innerHTML = items
    .map(
      ([label, value, hint]) => `
      <div class="card">
        <div class="label">${label}</div>
        <div class="value">${fmtNum(value)}</div>
        <div class="hint">${hint}</div>
      </div>`
    )
    .join("");
}

function renderChart(latency) {
  const el = $("#latencyChart");
  const cap = $("#latencyCaption");
  if (!latency || latency.error || !latency.length) {
    el.innerHTML = `<div class="muted">No latency series yet</div>`;
    cap.textContent = "";
    return;
  }
  const vals = latency.map((r) => Number(r.avg_ms) || 0);
  const max = Math.max(...vals, 1);
  el.innerHTML = latency
    .map((r) => {
      const h = Math.max(4, Math.round(((Number(r.avg_ms) || 0) / max) * 140));
      const title = `${fmtTs(r.minute)} · avg ${fmtNum(r.avg_ms)}ms · p95 ${fmtNum(r.p95_ms)}ms · n=${fmtNum(r.n)}`;
      return `<div class="bar" style="height:${h}px" title="${escapeAttr(title)}"></div>`;
    })
    .join("");
  const last = latency[latency.length - 1];
  cap.textContent = `last avg ${fmtNum(last.avg_ms)} ms · p95 ${fmtNum(last.p95_ms)} ms`;
}

async function refreshHealth() {
  const dot = $("#healthDot");
  const text = $("#healthText");
  try {
    const h = await api("/api/health");
    dot.className = "dot ok";
    text.textContent = `${h.uri} · ${h.latency_ms} ms`;
  } catch (e) {
    dot.className = "dot err";
    text.textContent = e.message || "offline";
  }
}

async function refreshOverview() {
  const data = await api("/api/overview");
  state.overview = data;
  renderKpis(data.tables || {});
  renderChart(data.latency);
  renderTable($("#recentApi"), data.recent?.api, [
    "ts",
    "method",
    "path",
    "status",
    "duration_ms",
  ]);
  renderTable($("#pathTable"), data.paths, ["path", "n", "avg_ms", "max_ms"]);
  renderTable($("#apiTable"), data.recent?.api, [
    "ts",
    "method",
    "path",
    "status",
    "duration_ms",
  ]);
  renderTable($("#lifeTable"), data.recent?.lifecycle, [
    "ts",
    "service",
    "action",
    "ok",
    "pid",
    "error",
  ]);
  renderTable($("#tickTable"), data.recent?.ticks, [
    "ts",
    "services_total",
    "services_alive",
    "services_unhealthy",
    "poll_interval_secs",
  ]);
}

async function loadTablesCatalog() {
  const data = await api("/api/tables");
  state.tables = data.tables || [];
  const sel = $("#tableSelect");
  sel.innerHTML = state.tables
    .map((t) => {
      const db = t.database || "";
      const name = t.table_name || t.name;
      const cat = db.startsWith("cat_") ? db.slice(4) : db === "master" ? "meta" : db;
      const value = `${cat}/${name}`;
      return `<option value="${escapeAttr(value)}">${escapeHtml(db)}.${escapeHtml(name)}</option>`;
    })
    .join("");
}

async function loadSelectedTable() {
  const value = $("#tableSelect").value;
  if (!value) return;
  const [category, table] = value.split("/");
  const data = await api(`/api/table/${encodeURIComponent(category)}/${encodeURIComponent(table)}?limit=100`);
  renderTable($("#exploreTable"), data.rows);
}

async function runSql(sql) {
  $("#sqlError").classList.add("hidden");
  $("#sqlMeta").textContent = "Running…";
  try {
    const data = await api("/api/sql", {
      method: "POST",
      body: JSON.stringify({ sql, limit: 200 }),
    });
    renderTable($("#sqlTable"), data.rows, data.columns);
    $("#sqlMeta").textContent = `${data.count} rows`;
  } catch (e) {
    $("#sqlError").textContent = e.message || String(e);
    $("#sqlError").classList.remove("hidden");
    $("#sqlMeta").textContent = "failed";
  }
}

function setView(name) {
  state.view = name;
  document.querySelectorAll(".nav button").forEach((b) => {
    b.classList.toggle("active", b.dataset.view === name);
  });
  ["overview", "api", "lifecycle", "explore", "sql"].forEach((v) => {
    $(`#view-${v}`).classList.toggle("hidden", v !== name);
  });
}

function bind() {
  $("#nav").addEventListener("click", (e) => {
    const btn = e.target.closest("button[data-view]");
    if (!btn) return;
    setView(btn.dataset.view);
  });

  $("#btnRefresh").addEventListener("pointerdown", () => {
    // instant press feedback path; actual work on click still fine
  });
  $("#btnRefresh").addEventListener("click", async () => {
    await Promise.all([refreshHealth(), refreshOverview()]);
  });
  $("#btnLoadTable").addEventListener("click", () => loadSelectedTable().catch(showTopError));
  $("#btnRunSql").addEventListener("click", () => runSql($("#sqlInput").value));
  $("#btnSqlApi").addEventListener("click", () => {
    $("#sqlInput").value = `SELECT path,
  count(*) AS n,
  avg(duration_ms) AS avg_ms,
  quantile_cont(duration_ms, 0.95) AS p95_ms
FROM cat_orchestrator.api_requests
WHERE ts > now() - INTERVAL 24 HOUR
GROUP BY 1
ORDER BY n DESC
LIMIT 30`;
  });
  $("#btnSqlLife").addEventListener("click", () => {
    $("#sqlInput").value = `SELECT ts, service, action, ok, pid, error
FROM cat_e2ed.service_lifecycle
ORDER BY ts DESC
LIMIT 50`;
  });
  $("#sqlInput").addEventListener("keydown", (e) => {
    if ((e.metaKey || e.ctrlKey) && e.key === "Enter") {
      e.preventDefault();
      runSql($("#sqlInput").value);
    }
  });
}

function showTopError(e) {
  console.error(e);
  alert(e.message || String(e));
}

async function boot() {
  bind();
  setView("overview");
  await refreshHealth();
  await refreshOverview();
  await loadTablesCatalog().catch(() => {});
  // light polling — low frequency to stay calm
  setInterval(() => {
    refreshHealth().catch(() => {});
    if (state.view === "overview") refreshOverview().catch(() => {});
  }, 15000);
}

boot().catch(showTopError);
