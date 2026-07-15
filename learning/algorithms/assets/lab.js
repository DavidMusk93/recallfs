/**
 * Algorithms Lab quiz runtime.
 * - No per-question answer reveal while answering
 * - Final submit grades all
 * - All correct → copy nextHint to clipboard + toast
 * - Any wrong → reveal explanations; user can retry
 *
 * Config via window.LAB or <script type="application/json" id="lab-config">
 */
(function () {
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
  const nextHint =
    cfg.nextHint ||
    "理解测完成，开始写 Rust";
  const passScore = cfg.passScore == null ? 1 : Number(cfg.passScore); // 1 = all correct

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
    t._timer = setTimeout(() => t.classList.remove("show"), 3200);
  }

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
    // multi: exact set match already comma-joined sorted
    if (q.dataset.multi != null || q.querySelector('input[type="checkbox"]')) {
      const want = norm(q.dataset.answer)
        .split(",")
        .filter(Boolean)
        .sort()
        .join(",");
      return got.split(",").filter(Boolean).sort().join(",") === want;
    }
    // fill: allow reverse pair "1,2" / "2,1" when data-orderless
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
        // mark selected wrong opts for multi/radio
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
          if (inp.type === "checkbox" && (q.dataset.answer || "").split(",").includes(inp.value)) {
            lab.classList.add("correct");
          }
        });
      }
    });
  }

  async function copyHint() {
    try {
      await navigator.clipboard.writeText(nextHint);
      return true;
    } catch (e) {
      // fallback
      const ta = document.createElement("textarea");
      ta.value = nextHint;
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
    return questions.filter((q) => {
      const a = getUserAnswer(q);
      return String(a).trim() !== "";
    }).length;
  }

  function initTabs() {
    $all("[data-tabs]").forEach((root) => {
      const buttons = $all(".tab-btn", root);
      const panels = $all(".tab-panel", root);
      buttons.forEach((btn) => {
        btn.addEventListener("click", () => {
          const id = btn.dataset.tab;
          buttons.forEach((b) => b.classList.toggle("active", b === btn));
          panels.forEach((p) => p.classList.toggle("active", p.dataset.panel === id));
        });
      });
    });
  }

  function initStepper() {
    $all("[data-stepper]").forEach((root) => {
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

    if (retryBtn) retryBtn.hidden = true;

    const onChange = () => {
      if (section.classList.contains("revealed")) return;
      updateProgress(countAnswered(questions), total);
    };

    section.addEventListener("change", onChange);
    section.addEventListener("input", onChange);
    updateProgress(0, total);

    if (submitBtn) {
      submitBtn.addEventListener("click", async () => {
        const answered = countAnswered(questions);
        if (answered < total) {
          toast("还有题目未作答，请全部完成后再提交", "bad");
          return;
        }

        const results = questions.map((q) => ({ q, ok: isCorrect(q) }));
        const correct = results.filter((r) => r.ok).length;
        const allOk = correct === total || correct / total >= passScore;

        revealGrade(section, results);

        if (status) {
          status.dataset.locked = "1";
          if (allOk) {
            status.textContent = "全部正确 " + correct + "/" + total;
            status.className = "ok";
          } else {
            status.textContent = "正确 " + correct + "/" + total + " · 查看解析后重试";
            status.className = "bad";
          }
        }

        if (allOk) {
          const copied = await copyHint();
          toast(
            copied
              ? "全部正确！下一步提示已复制到剪贴板"
              : "全部正确！请手动复制：" + nextHint,
            "ok"
          );
          if (submitBtn) submitBtn.hidden = true;
          if (retryBtn) retryBtn.hidden = false;
          // confetti-ish pulse
          const fill = $("#progress-fill");
          if (fill) {
            fill.style.width = "100%";
            fill.classList.add("done");
          }
          section.dispatchEvent(
            new CustomEvent("lab:pass", { detail: { nextHint, correct, total } })
          );
        } else {
          toast("未全对：已展开解析，请阅读后点「再来一次」", "bad");
          if (retryBtn) retryBtn.hidden = false;
          section.dispatchEvent(
            new CustomEvent("lab:fail", { detail: { correct, total } })
          );
        }
      });
    }

    if (retryBtn) {
      retryBtn.addEventListener("click", () => {
        clearGradeUi(section);
        // clear selections for fresh attempt
        $all("input[type=radio], input[type=checkbox]", section).forEach((i) => {
          i.checked = false;
        });
        $all("input[type=text], textarea", section).forEach((i) => {
          i.value = "";
        });
        if (status) {
          delete status.dataset.locked;
          status.className = "";
        }
        updateProgress(0, total);
        if (submitBtn) submitBtn.hidden = false;
        retryBtn.hidden = true;
        toast("已重置，重新作答后再次提交");
        section.scrollIntoView({ behavior: "smooth", block: "start" });
      });
    }
  }

  document.addEventListener("DOMContentLoaded", () => {
    initTabs();
    initStepper();
    initQuiz();
  });
})();
