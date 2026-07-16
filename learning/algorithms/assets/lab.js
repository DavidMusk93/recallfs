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
  const startedAt = Date.now();
  const sessionId =
    cfg.sessionId ||
    "s_" + startedAt.toString(36) + "_" + Math.random().toString(36).slice(2, 8);

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

  // ── Telemetry ──────────────────────────────────────────────
  const TELEMETRY_KEY =
    "lab.telemetry.v1." + (problemId != null ? problemId : "unknown") + "." + (slug || "x");

  const telemetry = {
    sessionId: sessionId,
    problemId: problemId,
    slug: slug,
    titleZh: titleZh,
    startedAt: new Date(startedAt).toISOString(),
    events: [],
    sectionDwell: Object.create(null), // sectionId -> ms
    sectionVisits: Object.create(null),
    sectionReentries: Object.create(null),
    answerFlips: Object.create(null),
    storyboard: { frames: 0, plays: 0, manual: 0, frameHits: Object.create(null) },
    quiz: { submits: 0, retries: 0, firstScore: null, lastScore: null, passed: false },
    maxScroll: 0,
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
    if (telemetry.events.length > 800) telemetry.events.splice(0, 200);
    try {
      document.dispatchEvent(
        new CustomEvent("lab:track", { detail: ev })
      );
    } catch (_) {}
  }

  function rankEntries(map, limit) {
    return Object.keys(map)
      .map((k) => ({ id: k, value: map[k] }))
      .sort((a, b) => b.value - a.value)
      .slice(0, limit || 5);
  }

  function summarizeForAi() {
    const interest = rankEntries(telemetry.sectionDwell, 6).map((x) => ({
      section: x.id,
      dwellMs: x.value,
      visits: telemetry.sectionVisits[x.id] || 0,
    }));
    const confusion = [];
    Object.keys(telemetry.sectionReentries).forEach((id) => {
      const n = telemetry.sectionReentries[id] || 0;
      if (n >= 2) {
        confusion.push({
          section: id,
          reentries: n,
          signal: "revisit",
        });
      }
    });
    Object.keys(telemetry.answerFlips).forEach((qid) => {
      const n = telemetry.answerFlips[qid] || 0;
      if (n >= 2) {
        confusion.push({
          section: "quiz:" + qid,
          answerFlips: n,
          signal: "answer_flip",
        });
      }
    });
    const frameHits = telemetry.storyboard.frameHits;
    Object.keys(frameHits).forEach((f) => {
      if (frameHits[f] >= 3) {
        confusion.push({
          section: "storyboard:" + f,
          hits: frameHits[f],
          signal: "frame_revisit",
        });
      }
    });

    return {
      schema: "lab.telemetry.summary.v1",
      sessionId: sessionId,
      problemId: problemId,
      slug: slug,
      titleZh: titleZh,
      startedAt: telemetry.startedAt,
      elapsedSec: Math.round(elapsedMs() / 1000),
      elapsedHuman: formatDuration(elapsedMs()),
      maxScrollPct: telemetry.maxScroll,
      interest: interest,
      confusion: confusion,
      storyboard: {
        frames: telemetry.storyboard.frames,
        plays: telemetry.storyboard.plays,
        manual: telemetry.storyboard.manual,
      },
      quiz: Object.assign({}, telemetry.quiz),
      eventCount: telemetry.events.length,
      hintForCoach:
        "AI 可用 interest 看用户爱看哪、用 confusion 看反复回看/改答案的卡点；" +
        "优先用英文术语（HashMap / complement / carry / two-pointers）讲解。",
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

  window.LAB_TELEMETRY = {
    track: track,
    summary: summarizeForAi,
    exportJson: function () {
      return JSON.stringify(summarizeForAi(), null, 2);
    },
    copyForAi: async function () {
      const text =
        "[Lab Telemetry for AI]\n" + JSON.stringify(summarizeForAi(), null, 2);
      const ok = await copyText(text);
      toast(ok ? "学习洞察已复制，可贴给 AI" : "复制失败，请看控制台", ok ? "ok" : "bad");
      track("telemetry_export", { ok: ok });
      return ok;
    },
    raw: telemetry,
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
              track("section_reentry", { section: id });
            } else {
              visited[id] = true;
              telemetry.sectionVisits[id] =
                (telemetry.sectionVisits[id] || 0) + 1;
              track("section_enter", { section: id });
            }
          } else if (visibleSince[id]) {
            const d = Date.now() - visibleSince[id];
            telemetry.sectionDwell[id] = (telemetry.sectionDwell[id] || 0) + d;
            track("section_leave", { section: id, dwellMs: d });
            delete visibleSince[id];
          }
        });
      },
      { threshold: [0.35, 0.6] }
    );

    cards.forEach((c) => io.observe(c));

    window.addEventListener("beforeunload", () => {
      Object.keys(visibleSince).forEach((id) => {
        const d = Date.now() - visibleSince[id];
        telemetry.sectionDwell[id] = (telemetry.sectionDwell[id] || 0) + d;
      });
      persistTelemetry();
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

  /** Image storyboard — primary animation surface (skill-drawn frames). */
  function initStoryboard() {
    $all("[data-storyboard]").forEach((root) => {
      const frames = $all(".sb-frame", root);
      if (!frames.length) return;
      telemetry.storyboard.frames = Math.max(
        telemetry.storyboard.frames,
        frames.length
      );

      let i = 0;
      let playing = root.dataset.autoplay !== "false";
      let timer = null;
      const period = Number(root.dataset.period) || 2200;
      const idxEl = root.querySelector("[data-sb-index]");
      const playBtn = root.querySelector("[data-sb-play]");

      function show(n, source) {
        i = ((n % frames.length) + frames.length) % frames.length;
        frames.forEach((f, idx) => f.classList.toggle("on", idx === i));
        if (idxEl) idxEl.textContent = i + 1 + " / " + frames.length;
        telemetry.storyboard.frameHits[i] =
          (telemetry.storyboard.frameHits[i] || 0) + 1;
        track("storyboard_frame", {
          index: i,
          source: source || "auto",
          caption:
            (frames[i].querySelector("figcaption") || {}).textContent || "",
        });
      }

      function stop() {
        playing = false;
        if (timer) clearInterval(timer);
        timer = null;
        if (playBtn) playBtn.textContent = "Play";
      }

      function start() {
        playing = true;
        if (playBtn) playBtn.textContent = "Pause";
        if (timer) clearInterval(timer);
        timer = setInterval(() => show(i + 1, "auto"), period);
        telemetry.storyboard.plays += 1;
        track("storyboard_play", {});
      }

      show(0, "init");
      if (playing) start();

      const prev = root.querySelector("[data-sb-prev]");
      const next = root.querySelector("[data-sb-next]");
      if (prev) {
        prev.addEventListener("click", () => {
          stop();
          telemetry.storyboard.manual += 1;
          show(i - 1, "prev");
        });
      }
      if (next) {
        next.addEventListener("click", () => {
          stop();
          telemetry.storyboard.manual += 1;
          show(i + 1, "next");
        });
      }
      if (playBtn) {
        playBtn.addEventListener("click", () => {
          if (playing) stop();
          else start();
        });
      }

      frames.forEach((f, idx) => {
        f.addEventListener("click", () => {
          stop();
          telemetry.storyboard.manual += 1;
          show(idx, "click");
        });
      });
    });
  }

  /** Legacy text stepper — kept as secondary caption track only. */
  function initStepper() {
    $all("[data-stepper]").forEach((root) => {
      // Prefer sibling/parent storyboard when present
      if (root.closest("[data-storyboard]") || root.querySelector(".sb-frame")) {
        return;
      }
      const steps = $all(".step", root);
      if (!steps.length) return;
      let i = 0;
      const tick = () => {
        steps.forEach((s, idx) => s.classList.toggle("on", idx <= i));
        i = (i + 1) % steps.length;
      };
      tick();
      const period = Number(root.dataset.period) || 1400;
      setInterval(tick, period);
      const btn = root.querySelector("[data-step-next]");
      if (btn) {
        btn.addEventListener("click", () => {
          steps.forEach((s, idx) => s.classList.toggle("on", idx <= i));
          i = (i + 1) % (steps.length + 1);
          if (i >= steps.length) {
            i = 0;
            steps.forEach((s) => s.classList.remove("on"));
          }
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
        if (ans) lastAnswers[qid] = ans;
        track("quiz_change", { qid: qid });
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

        telemetry.quiz.submits += 1;
        if (telemetry.quiz.firstScore == null) telemetry.quiz.firstScore = scoreStr;
        telemetry.quiz.lastScore = scoreStr;
        track("quiz_submit", {
          correct: correct,
          total: total,
          allOk: allOk,
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
          track("quiz_pass", {
            elapsedMs: elapsedMs(),
            clipboard: clip,
          });
        } else {
          toast("未全对：已展开解析，请阅读后点「再来一次」", "bad");
          if (retryBtn) retryBtn.hidden = false;
          persistTelemetry();
          section.dispatchEvent(
            new CustomEvent("lab:fail", {
              detail: {
                correct: correct,
                total: total,
                summary: summarizeForAi(),
              },
            })
          );
          track("quiz_fail", { correct: correct, total: total });
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

  function initCoachFab() {
    if ($("#lab-coach-fab")) return;
    const fab = document.createElement("button");
    fab.id = "lab-coach-fab";
    fab.type = "button";
    fab.className = "lab-coach-fab";
    fab.title = "复制学习洞察给 AI";
    fab.textContent = "AI 洞察";
    fab.addEventListener("click", () => {
      window.LAB_TELEMETRY.copyForAi();
    });
    document.body.appendChild(fab);
  }

  function initElapsedTicker() {
    const el = document.createElement("div");
    el.id = "lab-elapsed";
    el.className = "lab-elapsed";
    el.textContent = "用时 0s · " + problemTag();
    document.body.appendChild(el);
    setInterval(() => {
      el.textContent =
        "用时 " + formatDuration(elapsedMs()) + " · " + problemTag();
    }, 1000);
  }

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
    initCoachFab();
    initElapsedTicker();
    persistTelemetry();
  });
})();
