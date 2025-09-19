(function () {
  "use strict";

  /* ========== STORAGE UTIL ==========\ */
  const storage = {
    get(k, d = null) { try { const v = localStorage.getItem(k); return v === null ? d : JSON.parse(v); } catch { return d; } },
    set(k, v) { try { localStorage.setItem(k, JSON.stringify(v)); } catch {} },
  };

  /* ========== UI HOOKS ==========\ */
  const html     = document.documentElement;
  const toolbar  = document.querySelector(".toolbar");
  const sommaire = document.getElementById("sommaire");
  const fabToc   = document.getElementById("fabToc");

  const zoomIn   = document.getElementById("zoomIn");
  const zoomOut  = document.getElementById("zoomOut");
  const printBtn = document.getElementById("printBtn");
  const modeBtn  = document.getElementById("modeBtn");

  /* ========== A11Y / SCROLL ==========\ */
  const prefersReduced = window.matchMedia?.("(prefers-reduced-motion: reduce)") ?? { matches: false };
  html.style.scrollBehavior = prefersReduced.matches ? "auto" : "smooth";

  /* ========== THEME ==========\ */
  const THEMES = ["auto","light","dark"];
  const THEME_KEY = "vl.theme";
  const mqDark = window.matchMedia?.("(prefers-color-scheme: dark)");
  const eff = m => m === "auto" ? (mqDark?.matches ? "dark" : "light") : m;

  function applyTheme(mode) {
    const e = eff(mode);
    html.setAttribute("data-theme", e);
    html.style.colorScheme = e;
    if (modeBtn) {
      modeBtn.textContent = mode === "auto" ? "Thème: Auto" : (e === "dark" ? "Thème: Sombre" : "Thème: Clair");
      modeBtn.setAttribute("aria-pressed", mode !== "auto");
    }
    storage.set(THEME_KEY, mode);
  }
  applyTheme(storage.get(THEME_KEY, "auto"));

  modeBtn?.addEventListener("click", () => {
    const cur = storage.get(THEME_KEY, "auto");
    const next = THEMES[(THEMES.indexOf(cur) + 1) % THEMES.length];
    applyTheme(next);
  });
  mqDark?.addEventListener?.("change", () => {
    if (storage.get(THEME_KEY, "auto") === "auto") applyTheme("auto");
  });

  /* ========== ZOOM / PRINT ==========\ */
  const ZOOM_KEY = "vl.zoom";
  const getZoom = () => storage.get(ZOOM_KEY, 16);
  const setZoom = px => {
    const z = Math.max(12, Math.min(22, px | 0));
    html.style.fontSize = z + "px";
    storage.set(ZOOM_KEY, z);
  };
  setZoom(getZoom());

  zoomIn?.addEventListener("click", () => setZoom(getZoom() + 1));
  zoomOut?.addEventListener("click", () => setZoom(getZoom() - 1));
  printBtn?.addEventListener("click", () => window.print());

  /* ========== TOOLBAR HEIGHT VAR ==========\ */
  function updateToolbarH() {
    if (!toolbar) return;
    const h = toolbar.getBoundingClientRect().height || 0;
    html.style.setProperty("--tbH", h + "px");
  }
  const onResize = () => (window.requestAnimationFrame || setTimeout)(updateToolbarH, 16);
  updateToolbarH();
  addEventListener("resize", onResize);

  /* ========== FAB SOMMAIRE ==========\ */
  function thresholdY() {
    const s = sommaire ? (sommaire.offsetTop + sommaire.offsetHeight) : 0;
    return Math.min(innerHeight, s || innerHeight);
  }
  function onScroll() {
    const y = scrollY || document.documentElement.scrollTop || 0;
    fabToc?.toggleAttribute("hidden", !(y > thresholdY()));
  }
  onScroll();
  addEventListener("scroll", onScroll, { passive: true });
  addEventListener("resize", onScroll);

  fabToc?.addEventListener("click", () => {
    (sommaire || document.body).scrollIntoView({ behavior: prefersReduced.matches ? "auto" : "smooth", block: "start" });
  });

  /* ========== SHORTCUTS (sans recherche) ==========\ */
  addEventListener("keydown", (e) => {
    const ctrl = e.ctrlKey || e.metaKey;
    if (ctrl && (e.key === "=" || e.key === "+")) { e.preventDefault(); setZoom(getZoom() + 1); }
    if (ctrl && e.key === "-") { e.preventDefault(); setZoom(getZoom() - 1); }
    if (ctrl && e.key === "0") { e.preventDefault(); setZoom(16); }
  });

  /* ========== OBSERVE DOM CHANGES (pour recalculer la toolbar) ==========\ */
  if (toolbar && "MutationObserver" in window) {
    const mo = new MutationObserver(() => onResize());
    mo.observe(document.body, { childList: true, subtree: true });
  }
})();