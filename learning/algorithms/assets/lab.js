/**
 * Algorithms Lab runtime (product layer)
 *
 * - Quiz: no live answer reveal; final submit grades all
 * - Pass clipboard: problem id + elapsed time + nextHint
 * - Storyboard: image-frame animation (skill illustrations), not text-as-picture
 * - Telemetry: local behavior events for AI coach (interest / confusion)
 *
 * Config: window.LAB or <script type="application/json" id="lab-config">
 */
(function () {
  "use strict";

  function parseConfig() {
    if (window.LAB && typeof window.LAB === "object") return window.LAB;
    const el = document.getElementById("lab-config");
    if (el) {
      try {
        return JSON.parse(el.textContent);
      } catch (e) {
        console.error("lab-config JSON error", e);
      }
    }
    return {};
  }

  const cfg = parseConfig();
  const problemId = cfg.problemId != null ? Number(cfg.problemId) : null;
  const slug = cfg.slug || "";
  const titleZh = cfg.titleZh || cfg.title || "";
  const nextHintBase =
    cfg.nextHint || "理解测完成，开始写 Rust";
  const passScore = cfg.passScore == null ? 1 : Number(cfg.passScore);
  const telemetryEnabled = cfg.telemetry !== false;
  const telemetryEndpoint =
    cfg.telemetryEndpoint ||
    cfg.telemetryUrl ||
    "http://127.0.0.1:9090/api/lab/events";
  const startedAt = Date.now();
  const sessionId =
    cfg.sessionId ||
    "s_" + startedAt.toString(36) + "_" + Math.random().toString(36).slice(2, 8);
  let eventCursor = 0; // unsent events index

  function $(sel, root) {
    return (root || document).querySelector(sel);
  }
  function $all(sel, root) {
    return [...(root || document).querySelectorAll(sel)];
  }

  function toast(msg, kind) {
    let t = $("#lab-toast");
    if (!t) {
      t = document.createElement("div");
      t.id = "lab-toast";
      t.className = "toast";
      document.body.appendChild(t);
    }
    t.textContent = msg;
    t.className = "toast show" + (kind ? " " + kind : "");
    clearTimeout(t._timer);
    t._timer = setTimeout(() => t.classList.remove("show"), 3600);
  }

  function elapsedMs() {
    return Date.now() - startedAt;
  }

  function formatDuration(ms) {
    const s = Math.max(0, Math.round(ms / 1000));
    const m = Math.floor(s / 60);
    const r = s % 60;
    if (m <= 0) return r + "s";
    return m + "m" + String(r).padStart(2, "0") + "s";
  }

  function isoLocal(d) {
    const x = d || new Date();
    const pad = (n) => String(n).padStart(2, "0");
    return (
      x.getFullYear() +
      "-" +
      pad(x.getMonth() + 1) +
      "-" +
      pad(x.getDate()) +
      " " +
      pad(x.getHours()) +
      ":" +
      pad(x.getMinutes()) +
      ":" +
      pad(x.getSeconds())
    );
  }

  function problemTag() {
    const idPart = problemId != null ? "#" + problemId : "#?";
    const slugPart = slug ? " " + slug : "";
    return idPart + slugPart;
  }

  function buildPassClipboard(extra) {
    const lines = [
      "[Lab Pass] " +
        problemTag() +
        (titleZh ? " · " + titleZh : "") +
        " · 用时 " +
        formatDuration(elapsedMs()) +
        " · " +
        isoLocal(),
      nextHintBase,
    ];
    if (extra) lines.push(extra);
    return lines.join("\n");
  }

  // ── Telemetry（行为追溯：给「你」看路径，给 coach 看卡点）────────
  const TELEMETRY_KEY =
    "lab.telemetry.v1." + (problemId != null ? problemId : "unknown") + "." + (slug || "x");

  const SECTION_LABELS = {
    "how-to": "学习怎么用",
    scene: "应用场景",
    approaches: "多种解法",
    storyboard: "图解动画",
    "max-left": "max(left, last[c]+1) 卡点",
    variants: "发散 · 约束一变",
    quiz: "理解测",
  };

  function sectionLabel(id) {
    if (!id) return "未知区块";
    if (SECTION_LABELS[id]) return SECTION_LABELS[id];
    if (String(id).indexOf("quiz:") === 0) return "理解测 " + String(id).slice(5);
    if (String(id).indexOf("storyboard:") === 0)
      return "图解帧 " + (Number(String(id).slice(11)) + 1);
    return String(id);
  }

  const telemetry = {
    sessionId: sessionId,
    problemId: problemId,
    slug: slug,
    titleZh: titleZh,
    startedAt: new Date(startedAt).toISOString(),
    events: [],
    sectionDwell: Object.create(null),
    sectionVisits: Object.create(null),
    sectionReentries: Object.create(null),
    sectionFirstEnterAt: Object.create(null),
    path: [], // [{t, section, kind}]
    answerFlips: Object.create(null),
    answerHistory: Object.create(null), // qid -> [ans]
    quizDwell: Object.create(null), // qid -> ms
    quizFocusSince: Object.create(null),
    storyboard: {
      frames: 0,
      plays: 0,
      manual: 0,
      frameHits: Object.create(null),
      frameDwell: Object.create(null),
      lastFrame: null,
      lastFrameAt: null,
    },
    quiz: {
      submits: 0,
      retries: 0,
      firstScore: null,
      lastScore: null,
      passed: false,
      wrongQids: [],
    },
    maxScroll: 0,
    activeMs: 0,
    hiddenMs: 0,
    _visibleSince: Date.now(),
    _hiddenSince: null,
  };

  function loadTelemetrySoft() {
    if (!telemetryEnabled) return;
    try {
      const raw = localStorage.getItem(TELEMETRY_KEY);
      if (!raw) return;
      const prev = JSON.parse(raw);
      if (prev && prev.lifetime) telemetry.lifetime = prev.lifetime;
    } catch (_) {}
  }

  function persistTelemetry() {
    if (!telemetryEnabled) return;
    try {
      const lifetime = telemetry.lifetime || { sessions: 0, passCount: 0 };
      localStorage.setItem(
        TELEMETRY_KEY,
        JSON.stringify({
          lifetime: lifetime,
          lastSession: summarizeForAi(),
          updatedAt: new Date().toISOString(),
        })
      );
    } catch (_) {}
  }

  function track(type, payload) {
    if (!telemetryEnabled) return;
    const ev = {
      t: Date.now(),
      type: type,
      payload: payload || {},
    };
    telemetry.events.push(ev);
    // keep memory bounded
    if (telemetry.events.length > 800) {
      const drop = telemetry.events.length - 600;
      telemetry.events.splice(0, drop);
      eventCursor = Math.max(0, eventCursor - drop);
    }
    try {
      document.dispatchEvent(
        new CustomEvent("lab:track", { detail: ev })
      );
    } catch (_) {}
  }

  function flushTelemetry(kind) {
    if (!telemetryEnabled || !telemetryEndpoint) return Promise.resolve(false);
    const batch = telemetry.events.slice(eventCursor);
    if (!batch.length && kind !== "summary" && kind !== "pass") {
      return Promise.resolve(false);
    }
    const body = {
      kind: kind || "batch",
      sessionId: sessionId,
      problemId: problemId,
      slug: slug,
      titleZh: titleZh,
      startedAt: telemetry.startedAt,
      events: batch,
      summary: summarizeForAi(),
    };
    const sentUpTo = telemetry.events.length;
    const doFetch = () =>
      fetch(telemetryEndpoint, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
        keepalive: true,
        mode: "cors",
      })
        .then((r) => {
          if (r.ok || r.status === 202) {
            eventCursor = sentUpTo;
            return true;
          }
          return false;
        })
        .catch(() => false);

    // prefer sendBeacon for unload
    if (kind === "unload" && navigator.sendBeacon) {
      try {
        const blob = new Blob([JSON.stringify(body)], {
          type: "application/json",
        });
        const ok = navigator.sendBeacon(telemetryEndpoint, blob);
        if (ok) eventCursor = sentUpTo;
        return Promise.resolve(ok);
      } catch (_) {
        return doFetch();
      }
    }
    return doFetch();
  }

  function rankEntries(map, limit) {
    return Object.keys(map)
      .map((k) => ({ id: k, value: map[k] }))
      .sort((a, b) => b.value - a.value)
      .slice(0, limit || 5);
  }

  function accountVisibility() {
    const now = Date.now();
    if (document.visibilityState === "hidden") {
      if (telemetry._visibleSince != null) {
        telemetry.activeMs += now - telemetry._visibleSince;
        telemetry._visibleSince = null;
        telemetry._hiddenSince = now;
      }
    } else {
      if (telemetry._hiddenSince != null) {
        telemetry.hiddenMs += now - telemetry._hiddenSince;
        telemetry._hiddenSince = null;
        telemetry._visibleSince = now;
      } else if (telemetry._visibleSince == null) {
        telemetry._visibleSince = now;
      }
    }
  }

  function activeElapsedMs() {
    accountVisibility();
    let ms = telemetry.activeMs;
    if (telemetry._visibleSince != null) ms += Date.now() - telemetry._visibleSince;
    return ms;
  }

  function summarizeForAi() {
    accountVisibility();
    const interest = rankEntries(telemetry.sectionDwell, 6).map((x) => ({
      section: x.id,
      label: sectionLabel(x.id),
      dwellMs: x.value,
      visits: telemetry.sectionVisits[x.id] || 0,
      reentries: telemetry.sectionReentries[x.id] || 0,
    }));
    const confusion = [];
    Object.keys(telemetry.sectionReentries).forEach((id) => {
      const n = telemetry.sectionReentries[id] || 0;
      if (n >= 2) {
        confusion.push({
          section: id,
          label: sectionLabel(id),
          reentries: n,
          signal: "revisit",
          weight: n,
        });
      }
    });
    Object.keys(telemetry.answerFlips).forEach((qid) => {
      const n = telemetry.answerFlips[qid] || 0;
      if (n >= 2) {
        confusion.push({
          section: "quiz:" + qid,
          label: sectionLabel("quiz:" + qid),
          answerFlips: n,
          history: telemetry.answerHistory[qid] || [],
          signal: "answer_flip",
          weight: n + 1,
        });
      }
    });
    Object.keys(telemetry.storyboard.frameHits).forEach((f) => {
      if ((telemetry.storyboard.frameHits[f] || 0) >= 3) {
        confusion.push({
          section: "storyboard:" + f,
          label: sectionLabel("storyboard:" + f),
          hits: telemetry.storyboard.frameHits[f],
          dwellMs: telemetry.storyboard.frameDwell[f] || 0,
          signal: "frame_revisit",
          weight: telemetry.storyboard.frameHits[f],
        });
      }
    });
    confusion.sort((a, b) => (b.weight || 0) - (a.weight || 0));

    const understanding = assessUnderstanding();
    return {
      schema: "lab.telemetry.summary.v3",
      audience: "backend_ai_only",
      sessionId: sessionId,
      problemId: problemId,
      slug: slug,
      titleZh: titleZh,
      startedAt: telemetry.startedAt,
      elapsedSec: Math.round(elapsedMs() / 1000),
      elapsedHuman: formatDuration(elapsedMs()),
      activeSec: Math.round(activeElapsedMs() / 1000),
      hiddenSec: Math.round(telemetry.hiddenMs / 1000),
      maxScrollPct: telemetry.maxScroll,
      path: telemetry.path.slice(-40),
      interest: interest,
      confusion: confusion,
      understanding: understanding,
      storyboard: {
        frames: telemetry.storyboard.frames,
        plays: telemetry.storyboard.plays,
        manual: telemetry.storyboard.manual,
        frameHits: Object.assign({}, telemetry.storyboard.frameHits),
        frameDwell: Object.assign({}, telemetry.storyboard.frameDwell),
      },
      quiz: Object.assign({}, telemetry.quiz, {
        dwellByQ: Object.assign({}, telemetry.quizDwell),
        flipsByQ: Object.assign({}, telemetry.answerFlips),
        answerHistory: Object.assign({}, telemetry.answerHistory),
      }),
      eventCount: telemetry.events.length,
      hintForCoach:
        "This payload is for AI evaluation of algorithm understanding, not UI. " +
        "Read understanding.level/score first; use confusion (weight) and wrongQids; " +
        "interest = dwell focus. Keep CS terms in English. " +
        "If quiz.passed=false, do not paste full AC code.",
    };
  }

  async function copyText(text) {
    try {
      await navigator.clipboard.writeText(text);
      return true;
    } catch (e) {
      const ta = document.createElement("textarea");
      ta.value = text;
      ta.style.position = "fixed";
      ta.style.left = "-9999px";
      document.body.appendChild(ta);
      ta.select();
      let ok = false;
      try {
        ok = document.execCommand("copy");
      } catch (_) {}
      document.body.removeChild(ta);
      return ok;
    }
  }

  /**
   * AI-facing understanding estimate (not shown in UI).
   * Heuristic only — coach should combine with quiz correctness + dialogue.
   */
  function assessUnderstanding() {
    const s = {
      quizPassed: !!telemetry.quiz.passed,
      quizScore: telemetry.quiz.lastScore || telemetry.quiz.firstScore || null,
      submits: telemetry.quiz.submits || 0,
      retries: telemetry.quiz.retries || 0,
      flipTotal: 0,
      revisitTotal: 0,
      wrongQids: telemetry.quiz.wrongQids || [],
    };
    Object.keys(telemetry.answerFlips).forEach((k) => {
      s.flipTotal += telemetry.answerFlips[k] || 0;
    });
    Object.keys(telemetry.sectionReentries).forEach((k) => {
      s.revisitTotal += telemetry.sectionReentries[k] || 0;
    });

    let score = 50; // 0–100 rough prior
    if (s.quizPassed) score += 35;
    else if (s.submits > 0) score += 5;
    if (s.quizScore) {
      const parts = String(s.quizScore).split("/");
      const a = Number(parts[0]);
      const b = Number(parts[1]) || 1;
      if (!isNaN(a)) score += Math.round((a / b) * 20) - 10;
    }
    score -= Math.min(20, s.flipTotal * 3);
    score -= Math.min(15, s.revisitTotal * 2);
    score -= Math.min(10, s.retries * 4);
    if (s.submits === 0 && activeElapsedMs() < 20000) score -= 10;
    score = Math.max(0, Math.min(100, score));

    let level = "unknown";
    if (s.submits === 0 && activeElapsedMs() < 30000) level = "insufficient_data";
    else if (score >= 75 && s.quizPassed) level = "strong";
    else if (score >= 55) level = "partial";
    else level = "weak";

    return {
      score: score,
      level: level,
      signals: s,
      coachHint:
        level === "strong"
          ? "理解测通过且行为稳定，可进入 Rust 实现；少讲基础，抓边界。"
          : level === "partial"
            ? "部分掌握：先澄清 confusion/wrongQids，再写代码；勿贴完整 AC。"
            : level === "weak"
              ? "掌握偏弱：回到图解与主推不变量，禁止完整 AC。"
              : "数据不足：引导完成阅读与理解测。",
    };
  }

  // Internal API for agents/devtools only — no learner UI surface.
  window.LAB_TELEMETRY = {
    track: track,
    summary: summarizeForAi,
    understanding: assessUnderstanding,
    flush: flushTelemetry,
    exportJson: function () {
      return JSON.stringify(
        Object.assign({}, summarizeForAi(), {
          understanding: assessUnderstanding(),
        }),
        null,
        2
      );
    },
    raw: telemetry,
    sessionId: sessionId,
    endpoint: telemetryEndpoint,
  };

  // ── Quiz helpers ───────────────────────────────────────────
  function norm(s) {
    return String(s || "")
      .trim()
      .toLowerCase()
      .replace(/\s+/g, "")
      .replace(/[\[\]]/g, "");
  }

  function acceptList(q) {
    const raw = q.dataset.answer || "";
    const alts = (q.dataset.alts || "")
      .split("|")
      .map((s) => s.trim())
      .filter(Boolean);
    return [raw, ...alts].filter(Boolean);
  }

  function getUserAnswer(q) {
    if (q.dataset.multi != null || q.querySelector('input[type="checkbox"]')) {
      return $all('input[type="checkbox"]:checked', q)
        .map((c) => c.value)
        .sort()
        .join(",");
    }
    const radio = q.querySelector('input[type="radio"]:checked');
    if (radio) return radio.value;
    const text = q.querySelector('input[type="text"], textarea');
    if (text) return text.value;
    return "";
  }

  function isCorrect(q) {
    const got = norm(getUserAnswer(q));
    if (!got) return false;
    const accepts = acceptList(q).map(norm);
    if (q.dataset.multi != null || q.querySelector('input[type="checkbox"]')) {
      const want = norm(q.dataset.answer)
        .split(",")
        .filter(Boolean)
        .sort()
        .join(",");
      return got.split(",").filter(Boolean).sort().join(",") === want;
    }
    if (q.dataset.orderless != null && got.includes(",")) {
      const parts = got.split(",").sort().join(",");
      return accepts.some((a) => a.split(",").sort().join(",") === parts);
    }
    return accepts.some((a) => a === got);
  }

  function clearGradeUi(section) {
    section.classList.remove("revealed");
    $all(".q", section).forEach((q) => {
      q.classList.remove("ok-q", "bad-q");
      $all("label.opt", q).forEach((l) => l.classList.remove("correct", "wrong"));
    });
  }

  function revealGrade(section, results) {
    section.classList.add("revealed");
    results.forEach(({ q, ok }) => {
      q.classList.add(ok ? "ok-q" : "bad-q");
      if (!ok) {
        $all("label.opt", q).forEach((lab) => {
          const inp = lab.querySelector("input");
          if (!inp) return;
          if (q.dataset.answer && q.dataset.answer.split(",").includes(inp.value)) {
            lab.classList.add("correct");
          } else if (inp.checked) {
            lab.classList.add("wrong");
          }
        });
      } else {
        $all("label.opt", q).forEach((lab) => {
          const inp = lab.querySelector("input");
          if (!inp) return;
          if (inp.type === "radio" && inp.checked) lab.classList.add("correct");
          if (
            inp.type === "checkbox" &&
            (q.dataset.answer || "").split(",").includes(inp.value)
          ) {
            lab.classList.add("correct");
          }
        });
      }
    });
  }

  function updateProgress(answered, total) {
    const fill = $("#progress-fill");
    const status = $("#status-text");
    if (fill) {
      const pct = total ? Math.round((answered / total) * 100) : 0;
      fill.style.width = pct + "%";
      fill.classList.toggle("done", pct === 100);
    }
    if (status && !status.dataset.locked) {
      status.textContent = "已作答 " + answered + " / " + total;
      status.className = "";
    }
  }

  function countAnswered(questions) {
    return questions.filter((q) => String(getUserAnswer(q)).trim() !== "").length;
  }

  // ── UI widgets ─────────────────────────────────────────────
  function sectionId(el) {
    return (
      el.getAttribute("data-section") ||
      el.id ||
      (el.querySelector("h2")
        ? el.querySelector("h2").textContent.trim().slice(0, 40)
        : "section")
    );
  }

  function pushPath(section, kind) {
    const last = telemetry.path[telemetry.path.length - 1];
    if (last && last.section === section && last.kind === kind) return;
    telemetry.path.push({ t: Date.now(), section: section, kind: kind || "enter" });
    if (telemetry.path.length > 80) telemetry.path.splice(0, 30);
  }

  function initSectionTelemetry() {
    const cards = $all("section.card");
    if (!cards.length || !("IntersectionObserver" in window)) return;

    const visibleSince = Object.create(null);
    const visited = Object.create(null);

    const io = new IntersectionObserver(
      (entries) => {
        entries.forEach((en) => {
          const id = sectionId(en.target);
          if (en.isIntersecting && en.intersectionRatio >= 0.35) {
            if (!visibleSince[id]) visibleSince[id] = Date.now();
            if (visited[id]) {
              telemetry.sectionReentries[id] =
                (telemetry.sectionReentries[id] || 0) + 1;
              pushPath(id, "reentry");
              track("section_reentry", {
                section: id,
                label: sectionLabel(id),
                count: telemetry.sectionReentries[id],
              });
            } else {
              visited[id] = true;
              telemetry.sectionVisits[id] =
                (telemetry.sectionVisits[id] || 0) + 1;
              telemetry.sectionFirstEnterAt[id] = Date.now();
              pushPath(id, "enter");
              track("section_enter", {
                section: id,
                label: sectionLabel(id),
              });
            }
          } else if (visibleSince[id]) {
            const d = Date.now() - visibleSince[id];
            telemetry.sectionDwell[id] = (telemetry.sectionDwell[id] || 0) + d;
            track("section_leave", {
              section: id,
              label: sectionLabel(id),
              dwellMs: d,
            });
            delete visibleSince[id];
          }
        });
      },
      { threshold: [0.35, 0.6] }
    );

    cards.forEach((c) => io.observe(c));

    // per-question dwell inside quiz
    const qs = $all("#quiz-section .q[data-answer]");
    if (qs.length) {
      const qSince = Object.create(null);
      const qio = new IntersectionObserver(
        (entries) => {
          entries.forEach((en) => {
            const q = en.target;
            const qid = q.dataset.qid || "q";
            if (en.isIntersecting && en.intersectionRatio >= 0.5) {
              if (!qSince[qid]) qSince[qid] = Date.now();
            } else if (qSince[qid]) {
              const d = Date.now() - qSince[qid];
              telemetry.quizDwell[qid] = (telemetry.quizDwell[qid] || 0) + d;
              track("quiz_q_dwell", { qid: qid, dwellMs: d });
              delete qSince[qid];
            }
          });
        },
        { threshold: [0.5] }
      );
      qs.forEach((q) => qio.observe(q));
    }

    window.addEventListener("beforeunload", () => {
      Object.keys(visibleSince).forEach((id) => {
        const d = Date.now() - visibleSince[id];
        telemetry.sectionDwell[id] = (telemetry.sectionDwell[id] || 0) + d;
      });
      // close open storyboard frame dwell
      const sb = telemetry.storyboard;
      if (sb.lastFrame != null && sb.lastFrameAt != null) {
        const d = Date.now() - sb.lastFrameAt;
        sb.frameDwell[sb.lastFrame] = (sb.frameDwell[sb.lastFrame] || 0) + d;
      }
      accountVisibility();
      persistTelemetry();
      flushTelemetry("unload");
    });

    document.addEventListener("visibilitychange", () => {
      accountVisibility();
      track("visibility", { state: document.visibilityState });
    });
  }

  function initScrollTelemetry() {
    let last = 0;
    window.addEventListener(
      "scroll",
      () => {
        const now = Date.now();
        if (now - last < 400) return;
        last = now;
        const max =
          document.documentElement.scrollHeight - window.innerHeight || 1;
        const pct = Math.min(100, Math.round((window.scrollY / max) * 100));
        if (pct > telemetry.maxScroll) {
          telemetry.maxScroll = pct;
          track("scroll_depth", { pct: pct });
        }
      },
      { passive: true }
    );
  }

  function initTabs() {
    $all("[data-tabs]").forEach((root) => {
      const buttons = $all(".tab-btn", root);
      const panels = $all(".tab-panel", root);
      buttons.forEach((btn) => {
        btn.addEventListener("click", () => {
          const id = btn.dataset.tab;
          buttons.forEach((b) => b.classList.toggle("active", b === btn));
          panels.forEach((p) =>
            p.classList.toggle("active", p.dataset.panel === id)
          );
          track("tab_switch", {
            section: sectionId(root.closest("section") || root),
            tab: id,
          });
        });
      });
    });
  }

  function initDetails() {
    $all("details.approach").forEach((d) => {
      d.addEventListener("toggle", () => {
        track("details_toggle", {
          open: d.open,
          summary: (d.querySelector("summary") || {}).textContent || "",
        });
      });
    });
  }

  /**
   * Storyboard — SOTA scroll-snap carousel (not display:none slideshow).
   *
   * Why: mobile "page refresh" feel came from main-thread layout thrash
   * (display toggle + CSS entrance animation every 2s). Production carousels
   * keep slides in a fixed viewport and scroll; GPU/compositor owns motion.
   */
  function hydrateStoryboardImg(frame, priority) {
    if (!frame) return;
    const img =
      frame.querySelector("img[data-src]") || frame.querySelector("img");
    if (!img) return;
    if (img.dataset.src) {
      img.src = img.dataset.src;
      img.removeAttribute("data-src");
      delete img.dataset.src;
    }
    img.decoding = "async";
    img.loading = priority === "high" ? "eager" : img.loading || "lazy";
    if (priority === "high") img.fetchPriority = "high";
    if (img.decode) {
      img.decode().catch(function () {});
    }
  }

  function prefersReducedMotion() {
    try {
      return window.matchMedia("(prefers-reduced-motion: reduce)").matches;
    } catch (_) {
      return false;
    }
  }

  function initStoryboard() {
    $all("[data-storyboard]").forEach((root) => {
      const stage = root.querySelector(".sb-stage");
      const frames = $all(".sb-frame", root);
      if (!stage || !frames.length) return;

      telemetry.storyboard.frames = Math.max(
        telemetry.storyboard.frames,
        frames.length
      );

      // A11y: region
      stage.setAttribute("role", "region");
      stage.setAttribute("aria-roledescription", "carousel");
      stage.setAttribute("tabindex", "0");
      frames.forEach((f, idx) => {
        f.setAttribute("role", "group");
        f.setAttribute("aria-roledescription", "slide");
        f.setAttribute("aria-label", idx + 1 + " / " + frames.length);
      });

      // dots (native index affordance)
      let dotsHost = root.querySelector(".sb-dots");
      if (!dotsHost) {
        const controls = root.querySelector(".sb-controls");
        dotsHost = document.createElement("div");
        dotsHost.className = "sb-dots";
        dotsHost.setAttribute("role", "tablist");
        dotsHost.setAttribute("aria-label", "图解帧");
        if (controls) controls.appendChild(dotsHost);
      }
      dotsHost.innerHTML = "";
      const dots = frames.map((_, idx) => {
        const b = document.createElement("button");
        b.type = "button";
        b.className = "sb-dot";
        b.setAttribute("aria-label", "第 " + (idx + 1) + " 帧");
        b.addEventListener("click", () => {
          userIntent = true;
          wantPlay = false;
          syncPlayUi();
          goTo(idx, "smooth", "dot");
        });
        dotsHost.appendChild(b);
        return b;
      });

      let i = 0;
      let wantPlay =
        root.dataset.autoplay !== "false" && !prefersReducedMotion();
      let inView = false;
      let docVisible = document.visibilityState !== "hidden";
      let timer = null;
      let userIntent = false; // after manual nav, don't fight the user
      let scrollSettle = null;
      const period = Math.max(1800, Number(root.dataset.period) || 2800);
      const idxEl = root.querySelector("[data-sb-index]");
      const playBtn = root.querySelector("[data-sb-play]");

      function behavior(mode) {
        if (prefersReducedMotion()) return "auto";
        return mode || "smooth";
      }

      function recordActive(next, source) {
        const now = Date.now();
        const sb = telemetry.storyboard;
        if (sb.lastFrame != null && sb.lastFrameAt != null && sb.lastFrame !== next) {
          const d = now - sb.lastFrameAt;
          sb.frameDwell[sb.lastFrame] = (sb.frameDwell[sb.lastFrame] || 0) + d;
        }
        if (sb.lastFrame === next && source !== "init") return;
        i = next;
        frames.forEach((f, idx) => {
          f.classList.toggle("is-active", idx === i);
          f.setAttribute("aria-hidden", idx === i ? "false" : "true");
        });
        dots.forEach((d, idx) => {
          if (idx === i) d.setAttribute("aria-current", "true");
          else d.removeAttribute("aria-current");
        });
        if (idxEl) idxEl.textContent = i + 1 + " / " + frames.length;
        sb.frameHits[i] = (sb.frameHits[i] || 0) + 1;
        sb.lastFrame = i;
        sb.lastFrameAt = now;
        // warm current + neighbors (no layout cost)
        hydrateStoryboardImg(frames[i], "high");
        hydrateStoryboardImg(frames[(i + 1) % frames.length]);
        hydrateStoryboardImg(frames[(i - 1 + frames.length) % frames.length]);
        track("storyboard_frame", {
          index: i,
          source: source || "scroll",
          caption:
            (frames[i].querySelector("figcaption") || {}).textContent || "",
          hits: sb.frameHits[i],
        });
      }

      function goTo(n, mode, source) {
        const next = ((n % frames.length) + frames.length) % frames.length;
        const left = Math.round(next * stage.clientWidth);
        stage.scrollTo({ left: left, behavior: behavior(mode) });
        // optimistic index; scroll observer confirms
        recordActive(next, source || "goto");
        if (source === "auto") {
          /* keep playing */
        } else if (source && source !== "init") {
          telemetry.storyboard.manual += 1;
        }
      }

      function indexFromScroll() {
        const w = stage.clientWidth || 1;
        return Math.max(
          0,
          Math.min(frames.length - 1, Math.round(stage.scrollLeft / w))
        );
      }

      function onScroll() {
        if (scrollSettle) clearTimeout(scrollSettle);
        scrollSettle = setTimeout(() => {
          recordActive(indexFromScroll(), "scroll");
          armAutoplay();
        }, 80);
      }

      function syncPlayUi() {
        if (!playBtn) return;
        playBtn.textContent = wantPlay && inView && docVisible ? "Pause" : "Play";
      }

      function clearTimer() {
        if (timer) {
          clearTimeout(timer);
          timer = null;
        }
      }

      function armAutoplay() {
        clearTimer();
        if (!wantPlay || !inView || !docVisible || prefersReducedMotion()) {
          syncPlayUi();
          return;
        }
        syncPlayUi();
        timer = setTimeout(() => {
          // scroll-snap advance — compositor path, not DOM rebuild
          goTo(i + 1, "smooth", "auto");
          armAutoplay();
        }, period);
      }

      function setWantPlay(on, reason) {
        wantPlay = !!on;
        if (wantPlay) {
          telemetry.storyboard.plays += 1;
          track("storyboard_play", { reason: reason || "user" });
        } else {
          track("storyboard_pause", { reason: reason || "user" });
        }
        armAutoplay();
      }

      // IntersectionObserver: which slide is visible (root = stage)
      if ("IntersectionObserver" in window) {
        const slideIo = new IntersectionObserver(
          (entries) => {
            let best = null;
            entries.forEach((en) => {
              if (!en.isIntersecting) return;
              if (!best || en.intersectionRatio > best.intersectionRatio) {
                best = en;
              }
            });
            if (!best) return;
            const idx = frames.indexOf(best.target);
            if (idx >= 0) recordActive(idx, "io");
          },
          { root: stage, threshold: [0.55, 0.75, 0.9] }
        );
        frames.forEach((f) => slideIo.observe(f));

        // pause when carousel leaves the viewport (critical on mobile)
        const viewIo = new IntersectionObserver(
          (entries) => {
            const en = entries[0];
            inView = !!(en && en.isIntersecting && en.intersectionRatio > 0.2);
            armAutoplay();
          },
          { threshold: [0, 0.2, 0.5] }
        );
        viewIo.observe(root);
      } else {
        inView = true;
      }

      document.addEventListener("visibilitychange", () => {
        docVisible = document.visibilityState !== "hidden";
        armAutoplay();
      });

      stage.addEventListener("scroll", onScroll, { passive: true });
      // native scrollend when available
      stage.addEventListener("scrollend", () => {
        recordActive(indexFromScroll(), "scrollend");
      });

      // user gesture: stop fighting autoplay
      ["pointerdown", "touchstart", "wheel"].forEach((ev) => {
        stage.addEventListener(
          ev,
          () => {
            userIntent = true;
            if (wantPlay) setWantPlay(false, "gesture");
          },
          { passive: true }
        );
      });

      const prev = root.querySelector("[data-sb-prev]");
      const next = root.querySelector("[data-sb-next]");
      if (prev) {
        prev.addEventListener("click", () => {
          userIntent = true;
          setWantPlay(false, "prev");
          goTo(i - 1, "smooth", "prev");
        });
      }
      if (next) {
        next.addEventListener("click", () => {
          userIntent = true;
          setWantPlay(false, "next");
          goTo(i + 1, "smooth", "next");
        });
      }
      if (playBtn) {
        playBtn.addEventListener("click", () => {
          userIntent = true;
          setWantPlay(!wantPlay, "button");
        });
      }

      stage.addEventListener("keydown", (ev) => {
        if (ev.key === "ArrowRight") {
          ev.preventDefault();
          setWantPlay(false, "key");
          goTo(i + 1, "smooth", "key");
        } else if (ev.key === "ArrowLeft") {
          ev.preventDefault();
          setWantPlay(false, "key");
          goTo(i - 1, "smooth", "key");
        }
      });

      // initial: no smooth jump, hydrate first frames
      hydrateStoryboardImg(frames[0], "high");
      hydrateStoryboardImg(frames[1]);
      stage.scrollTo({ left: 0, behavior: "auto" });
      recordActive(0, "init");
      // start autoplay only if allowed
      if (wantPlay) {
        // inView may still be false until IO fires; arm when ready
        armAutoplay();
      } else {
        syncPlayUi();
      }
    });
  }

  /** Legacy text stepper: manual only — no setInterval class thrash. */
  function initStepper() {
    $all("[data-stepper]").forEach((root) => {
      if (root.closest("[data-storyboard]") || root.querySelector(".sb-frame")) {
        return;
      }
      const steps = $all(".step", root);
      if (!steps.length) return;
      let i = 0;
      const paint = () => {
        steps.forEach((s, idx) => s.classList.toggle("on", idx <= i));
      };
      paint();
      const btn = root.querySelector("[data-step-next]");
      if (btn) {
        btn.addEventListener("click", () => {
          i = (i + 1) % steps.length;
          paint();
          track("legacy_stepper", { index: i });
        });
      }
    });
  }

  function initQuiz() {
    const section = $("#quiz-section");
    if (!section) return;

    const questions = $all(".q[data-answer]", section);
    const total = questions.length;
    const submitBtn = $("#quiz-submit");
    const retryBtn = $("#quiz-retry");
    const status = $("#status-text");
    const lastAnswers = Object.create(null);

    if (retryBtn) retryBtn.hidden = true;

    questions.forEach((q, idx) => {
      if (!q.dataset.qid) q.dataset.qid = "q" + (idx + 1);
    });

    const onChange = (ev) => {
      if (section.classList.contains("revealed")) return;
      updateProgress(countAnswered(questions), total);
      const q = ev.target && ev.target.closest ? ev.target.closest(".q") : null;
      if (q) {
        const qid = q.dataset.qid || "q";
        const ans = getUserAnswer(q);
        if (lastAnswers[qid] != null && lastAnswers[qid] !== ans && ans) {
          telemetry.answerFlips[qid] = (telemetry.answerFlips[qid] || 0) + 1;
          track("answer_flip", { qid: qid, from: lastAnswers[qid], to: ans });
        }
        if (ans) {
          lastAnswers[qid] = ans;
          if (!telemetry.answerHistory[qid]) telemetry.answerHistory[qid] = [];
          const hist = telemetry.answerHistory[qid];
          if (hist[hist.length - 1] !== ans) {
            hist.push(ans);
            if (hist.length > 8) hist.shift();
          }
        }
        track("quiz_change", { qid: qid, ans: ans });
      }
    };

    section.addEventListener("change", onChange);
    section.addEventListener("input", onChange);
    updateProgress(0, total);

    if (submitBtn) {
      submitBtn.addEventListener("click", async () => {
        const answered = countAnswered(questions);
        if (answered < total) {
          toast("还有题目未作答，请全部完成后再提交", "bad");
          track("quiz_submit_blocked", { answered: answered, total: total });
          return;
        }

        const results = questions.map((q) => ({ q, ok: isCorrect(q) }));
        const correct = results.filter((r) => r.ok).length;
        const allOk = correct === total || correct / total >= passScore;
        const scoreStr = correct + "/" + total;
        const wrongQids = results
          .filter((r) => !r.ok)
          .map((r) => r.q.dataset.qid || "?");

        telemetry.quiz.submits += 1;
        if (telemetry.quiz.firstScore == null) telemetry.quiz.firstScore = scoreStr;
        telemetry.quiz.lastScore = scoreStr;
        telemetry.quiz.wrongQids = wrongQids;
        track("quiz_submit", {
          correct: correct,
          total: total,
          allOk: allOk,
          wrongQids: wrongQids,
          elapsedMs: elapsedMs(),
        });

        revealGrade(section, results);

        if (status) {
          status.dataset.locked = "1";
          if (allOk) {
            status.textContent =
              "全部正确 " +
              scoreStr +
              " · 用时 " +
              formatDuration(elapsedMs());
            status.className = "ok";
          } else {
            status.textContent =
              "正确 " + scoreStr + " · 查看解析后重试";
            status.className = "bad";
          }
        }

        if (allOk) {
          telemetry.quiz.passed = true;
          const clip = buildPassClipboard();
          const copied = await copyText(clip);
          toast(
            copied
              ? "全部正确！已复制：题号 + 用时 + 下一步"
              : "全部正确！请手动复制：" + clip,
            "ok"
          );
          if (submitBtn) submitBtn.hidden = true;
          if (retryBtn) retryBtn.hidden = false;
          const fill = $("#progress-fill");
          if (fill) {
            fill.style.width = "100%";
            fill.classList.add("done");
          }
          try {
            telemetry.lifetime = telemetry.lifetime || {
              sessions: 0,
              passCount: 0,
            };
            telemetry.lifetime.passCount =
              (telemetry.lifetime.passCount || 0) + 1;
          } catch (_) {}
          persistTelemetry();
          track("quiz_pass", {
            elapsedMs: elapsedMs(),
            clipboard: clip,
          });
          flushTelemetry("pass");
          section.dispatchEvent(
            new CustomEvent("lab:pass", {
              detail: {
                nextHint: clip,
                correct: correct,
                total: total,
                elapsedMs: elapsedMs(),
                problemId: problemId,
                slug: slug,
                summary: summarizeForAi(),
              },
            })
          );
        } else {
          toast("未全对：已展开解析，请阅读后点「再来一次」", "bad");
          if (retryBtn) retryBtn.hidden = false;
          persistTelemetry();
          track("quiz_fail", { correct: correct, total: total });
          flushTelemetry("fail");
          section.dispatchEvent(
            new CustomEvent("lab:fail", {
              detail: {
                correct: correct,
                total: total,
                summary: summarizeForAi(),
              },
            })
          );
        }
      });
    }

    if (retryBtn) {
      retryBtn.addEventListener("click", () => {
        clearGradeUi(section);
        $all("input[type=radio], input[type=checkbox]", section).forEach((i) => {
          i.checked = false;
        });
        $all("input[type=text], textarea", section).forEach((i) => {
          i.value = "";
        });
        Object.keys(lastAnswers).forEach((k) => delete lastAnswers[k]);
        if (status) {
          delete status.dataset.locked;
          status.className = "";
        }
        updateProgress(0, total);
        if (submitBtn) submitBtn.hidden = false;
        retryBtn.hidden = true;
        telemetry.quiz.retries += 1;
        track("quiz_retry", {});
        toast("已重置，重新作答后再次提交");
        section.scrollIntoView({ behavior: "smooth", block: "start" });
      });
    }
  }

  /**
   * KaTeX math: \( … \) inline, \[ … \] or $$ … $$ display.
   * Assets: assets/vendor/katex/ (local, offline-safe).
   * Skip if cfg.math === false.
   */
  function labAssetUrl(rel) {
    const scripts = document.getElementsByTagName("script");
    let base = "";
    for (let i = scripts.length - 1; i >= 0; i--) {
      const src = scripts[i].src || "";
      if (src.indexOf("lab.js") !== -1) {
        base = src.replace(/lab\.js(?:\?.*)?$/, "");
        break;
      }
    }
    return base + rel;
  }

  function loadScript(src) {
    return new Promise((resolve, reject) => {
      const s = document.createElement("script");
      s.src = src;
      s.onload = () => resolve();
      s.onerror = () => reject(new Error("load fail: " + src));
      document.head.appendChild(s);
    });
  }

  function loadCss(href) {
    if (document.querySelector('link[data-lab-katex="1"]')) return;
    const link = document.createElement("link");
    link.rel = "stylesheet";
    link.href = href;
    link.setAttribute("data-lab-katex", "1");
    document.head.appendChild(link);
  }

  function renderMath(root) {
    if (typeof window.renderMathInElement !== "function") return;
    try {
      window.renderMathInElement(root || document.body, {
        delimiters: [
          { left: "$$", right: "$$", display: true },
          { left: "\\[", right: "\\]", display: true },
          { left: "\\(", right: "\\)", display: false },
        ],
        throwOnError: false,
        strict: "ignore",
      });
    } catch (e) {
      console.warn("lab math render", e);
    }
  }

  function initMath() {
    if (cfg.math === false) return Promise.resolve();
    if (typeof window.renderMathInElement === "function") {
      renderMath(document.body);
      return Promise.resolve();
    }
    const base = labAssetUrl("vendor/katex/");
    loadCss(base + "katex.min.css");
    return loadScript(base + "katex.min.js")
      .then(() => loadScript(base + "auto-render.min.js"))
      .then(() => {
        renderMath(document.body);
      })
      .catch((e) => {
        console.warn("KaTeX unavailable", e);
      });
  }

  // Re-export for pages that inject math after load (e.g. dynamic explain).
  window.LAB_RENDER_MATH = renderMath;

  document.addEventListener("DOMContentLoaded", () => {
    loadTelemetrySoft();
    track("page_view", {
      href: location.href,
      problemId: problemId,
      slug: slug,
    });
    initTabs();
    initDetails();
    initStoryboard();
    initStepper();
    initQuiz();
    initSectionTelemetry();
    initScrollTelemetry();
    initMath();
    // Telemetry is silent backend analytics for AI coach — no learner-facing panel.
    persistTelemetry();
    flushTelemetry("page_view");
    setInterval(() => flushTelemetry("heartbeat"), 15000);
  });
})();
