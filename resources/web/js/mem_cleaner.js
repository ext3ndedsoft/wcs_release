// ============================================================================
// Memory Cleaner Controller (WCS Pro)
// ============================================================================

let reqId = 1;
const pendingRequests = new Map();

function callNative(action, params = {}) {
  return new Promise((resolve) => {
    const id = reqId++;
    pendingRequests.set(id, { resolve });
    if (window.chrome && window.chrome.webview) {
      window.chrome.webview.postMessage({ id, action, params });
    } else {
      resolve({ success: false });
    }
  });
}

let currentThemeAccent = "cyan";

function applyThemeBg(bg) {
  if (!bg) bg = localStorage.getItem("wcs_theme_bg") || "obsidian";
  const bgLayer = document.getElementById("themeBackgroundLayer");
  if (bgLayer) {
    bgLayer.className = `theme-bg-layer theme-bg-${bg}`;
  }
}

function applyTheme(accent) {
  if (!accent) accent = "cyan";
  currentThemeAccent = accent;
  const themes = ["cyan", "emerald", "crimson", "violet", "amber"];
  themes.forEach(t => document.body.classList.remove(`theme-${t}`));
  document.body.classList.add(`theme-${accent}`);

  const logoPath = (accent === "cyan") ? "/branding/wcs_logo_transparent.png" : `/branding/wcs_logo_${accent}.png`;
  const favicons = document.querySelectorAll("link[rel*='icon']");
  favicons.forEach(fav => {
    if (fav && fav.href && fav.href.includes("wcs_logo")) {
      fav.href = logoPath;
    }
  });

  const headerLogo = document.getElementById("headerLogo");
  if (headerLogo) {
    headerLogo.src = logoPath;
  }

  applyThemeBg();

  try {
    localStorage.setItem("wcs_themeAccent", accent);
  } catch (_) {}
}

try {
  const cachedTheme = localStorage.getItem("wcs_themeAccent");
  if (cachedTheme) {
    applyTheme(cachedTheme);
  } else {
    applyThemeBg();
  }
} catch (_) {}

if (window.chrome && window.chrome.webview) {
  window.chrome.webview.addEventListener('message', (event) => {
    const msg = event.data;
    if (msg.id && pendingRequests.has(msg.id)) {
      const { resolve } = pendingRequests.get(msg.id);
      pendingRequests.delete(msg.id);
      resolve(msg.data);
    } else if (msg.event === "telemetry_update") {
      updateMemStats(msg.data);
    } else if (msg.event === "theme_changed") {
      if (msg.data && msg.data.theme) {
        applyTheme(msg.data.theme);
      }
    } else if (msg.event === "theme_bg_changed") {
      if (msg.data && msg.data.themeBg) {
        applyThemeBg(msg.data.themeBg);
      }
    } else if (msg.event === "toast") {
      showToast(msg.data.message, msg.data.isError);
    }
  });
}

let currentMemTab = 0;
let isMemTabSwitching = false;

function switchMemTab(tabIdx) {
  if (tabIdx === currentMemTab && !isMemTabSwitching) return;

  const btn0 = document.getElementById("tabBtn0");
  const btn1 = document.getElementById("tabBtn1");
  const t0 = document.getElementById("memTab0");
  const t1 = document.getElementById("memTab1");

  btn0?.classList.toggle("active", tabIdx === 0);
  btn1?.classList.toggle("active", tabIdx === 1);

  const leavingTab = (tabIdx === 0) ? t1 : t0;
  const enteringTab = (tabIdx === 0) ? t0 : t1;

  if (leavingTab && leavingTab.style.display !== "none") {
    isMemTabSwitching = true;
    leavingTab.classList.remove("tab-entering");
    leavingTab.classList.add("tab-leaving");

    setTimeout(() => {
      requestAnimationFrame(() => {
        leavingTab.style.display = "none";
        leavingTab.classList.remove("tab-leaving");

        if (enteringTab) {
          enteringTab.style.display = "flex";
          enteringTab.classList.remove("tab-leaving");
          enteringTab.classList.add("tab-entering");
        }
        currentMemTab = tabIdx;
        isMemTabSwitching = false;
      });
    }, 100);
  } else {
    requestAnimationFrame(() => {
      if (leavingTab) leavingTab.style.display = "none";
      if (enteringTab) {
        enteringTab.style.display = "flex";
        enteringTab.classList.add("tab-entering");
      }
      currentMemTab = tabIdx;
    });
  }
}

let pendingMemStats = null;
let memRafId = null;

function updateMemStats(data) {
  if (!data) return;
  pendingMemStats = data;
  if (!memRafId) {
    memRafId = requestAnimationFrame(() => {
      memRafId = null;
      if (pendingMemStats) {
        renderMemStatsDOM(pendingMemStats);
      }
    });
  }
}

function renderMemStatsDOM(data) {
  if (!data) return;

  const usedGb = data.ramUsedGb || 0;
  const totalGb = data.ramTotalGb || 16;
  const freeGb = data.ramFreeGb || 0;
  const standbyGb = data.ramStandbyGb || 0;
  const usedPct = data.ramPercent || 0;
  const standbyPct = totalGb > 0 ? (standbyGb / totalGb * 100) : 0;
  const freePct = Math.max(0, 100 - usedPct - standbyPct);

  // Title
  const titleEl = document.getElementById("ramTitleVal");
  if (titleEl) {
    titleEl.innerText = `${usedGb.toFixed(1)} ГБ / ${totalGb.toFixed(1)} ГБ`;
  }
  const badgeEl = document.getElementById("ramPercentBadge");
  if (badgeEl) {
    badgeEl.innerText = `${usedPct.toFixed(0)}%`;
  }
  const headerRamPill = document.getElementById("headerRamPill");
  if (headerRamPill) {
    headerRamPill.innerText = `ОЗУ: ${totalGb.toFixed(0)} ГБ`;
  }

  // Segmented Bars
  const barUsed = document.getElementById("barUsed");
  const barStandby = document.getElementById("barStandby");
  if (barUsed) barUsed.style.width = `${usedPct}%`;
  if (barStandby) barStandby.style.width = `${standbyPct}%`;

  // Legends
  const legUsed = document.getElementById("legUsed");
  if (legUsed) legUsed.innerText = `${usedPct.toFixed(0)}%`;
  const legStandby = document.getElementById("legStandby");
  if (legStandby) legStandby.innerText = `${standbyPct.toFixed(0)}%`;
  const legFree = document.getElementById("legFree");
  if (legFree) legFree.innerText = `${freePct.toFixed(0)}%`;

  // Pagefile
  const pfUsed = data.pagefileUsedGb || 0;
  const pfTotal = data.pagefileTotalGb || 0;
  const pfPct = data.pagefilePercent || 0;
  const pfEl = document.getElementById("pagefileVal");
  if (pfEl) {
    pfEl.innerText = `${pfUsed.toFixed(1)} ГБ / ${pfTotal.toFixed(1)} ГБ (${pfPct.toFixed(0)}%)`;
  }
  const barPf = document.getElementById("barPagefile");
  if (barPf) barPf.style.width = `${pfPct}%`;

  // 4 Stat Tiles: Total, In Use, Available, Standby List
  const totalEl = document.getElementById("statTotalRam");
  if (totalEl) totalEl.innerText = `${totalGb.toFixed(1)} ГБ`;

  const inUseEl = document.getElementById("statInUseRam");
  if (inUseEl) inUseEl.innerText = `${usedGb.toFixed(2)} ГБ`;

  const freeEl = document.getElementById("statFreeRam");
  if (freeEl) freeEl.innerText = `${freeGb.toFixed(2)} ГБ`;

  const standbyEl = document.getElementById("statStandbyRam");
  if (standbyEl) standbyEl.innerText = `${standbyGb.toFixed(2)} ГБ`;

  // Secondary metrics
  const peakEl = document.getElementById("statPeak");
  if (peakEl) peakEl.innerText = `${(data.peakMemoryPercent || 0).toFixed(0)}%`;

  const freedEl = document.getElementById("statFreed");
  if (freedEl) {
    let freedStr = data.lastFreedStr || "0 Б";
    freedStr = freedStr.replace(/GB/g, "ГБ").replace(/MB/g, "МБ").replace(/KB/g, "КБ").replace(/ B/g, " Б");
    freedEl.innerText = freedStr;
  }

  // Status
  if (data.lastActionStatus) {
    const statusEl = document.getElementById("memStatusText");
    if (statusEl) statusEl.innerText = data.lastActionStatus;
  }

  // Countdown
  if (data.remainingCleanSec !== undefined) {
    const m = Math.floor(data.remainingCleanSec / 60);
    const s = data.remainingCleanSec % 60;
    const sPad = s < 10 ? "0" + s : s;
    const countEl = document.getElementById("timerCountdown");
    if (countEl) countEl.innerText = `через ${m}:${sPad}`;
  }
}

async function trimWorkingSet() {
  try {
    const res = await callNative("trimWorkingSet");
    if (res && res.error) showToast(res.error, true);
  } catch (err) {
    showToast("Ошибка при сбросе рабочих наборов памяти", true);
  }
}

async function clearStandbyCache() {
  try {
    const res = await callNative("clearSystemCache");
    if (res && res.error) showToast(res.error, true);
  } catch (err) {
    showToast("Ошибка при очистке кэша Standby", true);
  }
}

async function cleanAllMemory() {
  try {
    const res = await callNative("cleanAllMemory");
    if (res && res.error) showToast(res.error, true);
  } catch (err) {
    showToast("Ошибка при полной оптимизации памяти", true);
  }
}

// Preset Pills & Sliders
function setThresholdPreset(val) {
  const valNum = parseInt(val, 10);
  const rng = document.getElementById("rngThreshold");
  if (rng) rng.value = valNum;
  const lbl = document.getElementById("lblThresholdVal");
  if (lbl) lbl.innerText = valNum + "%";
  updateThresholdPills(valNum);
}

function updateThresholdPills(valNum) {
  document.querySelectorAll(".threshold-pill").forEach(p => {
    const pVal = parseInt(p.dataset.val, 10);
    p.classList.toggle("active", pVal === valNum);
  });
}

function setIntervalPreset(val) {
  const valNum = parseInt(val, 10);
  const rng = document.getElementById("rngInterval");
  if (rng) rng.value = valNum;
  const lbl = document.getElementById("lblIntervalVal");
  if (lbl) lbl.innerText = valNum + " мин";
  updateIntervalPills(valNum);
}

function updateIntervalPills(valNum) {
  document.querySelectorAll(".interval-pill").forEach(p => {
    const pVal = parseInt(p.dataset.val, 10);
    p.classList.toggle("active", pVal === valNum);
  });
}

async function saveMemConfig() {
  try {
    const autoThreshold = document.getElementById("cfgAutoThreshold")?.checked || false;
    const thresholdVal = parseInt(document.getElementById("rngThreshold")?.value || "80", 10);
    const trimOnThresh = document.getElementById("cfgTrimOnThreshold")?.checked || false;
    const clearOnThresh = document.getElementById("cfgClearOnThreshold")?.checked || false;

    const autoInterval = document.getElementById("cfgAutoInterval")?.checked || false;
    const intervalVal = parseInt(document.getElementById("rngInterval")?.value || "15", 10);
    const trimOnInt = document.getElementById("cfgTrimOnInterval")?.checked || false;
    const clearOnInt = document.getElementById("cfgClearOnInterval")?.checked || false;

    const cfgPayload = {
      autoRamCleanEnabled: autoThreshold || autoInterval,
      ramThresholdPercent: thresholdVal,
      trimWorkingSetOnThreshold: trimOnThresh,
      clearCacheOnThreshold: clearOnThresh,
      ramCleanIntervalMinutes: intervalVal,
      trimWorkingSetOnInterval: trimOnInt,
      clearCacheOnInterval: clearOnInt
    };

    const bgEl = document.getElementById("cfgRunInBackground");
    if (bgEl) {
      cfgPayload.runInBackground = bgEl.checked;
      await callNative("updateSetting", { key: "runInBackground", value: bgEl.checked });
    }

    const autoEl = document.getElementById("cfgAutostart");
    if (autoEl) {
      cfgPayload.autostartWithWindows = autoEl.checked;
      await callNative("updateSetting", { key: "autostartWithWindows", value: autoEl.checked });
    }

    await callNative("setMemConfig", cfgPayload);

    const autoCleanDescEl = document.getElementById("autoCleanDesc");
    if (autoCleanDescEl) {
      autoCleanDescEl.innerText = `Порог > ${thresholdVal}% • Каждые ${intervalVal} мин`;
    }
    showToast("Параметры успешно сохранены!", false);
  } catch (err) {
    showToast("Ошибка сохранения параметров памяти", true);
  }
}

function showToast(message, isError = false) {
  const container = document.getElementById("toastContainer");
  if (!container || !message) return;

  const toast = document.createElement("div");
  toast.className = `toast ${isError ? "error" : "success"}`;
  toast.innerHTML = `<span>${message}</span>`;
  container.appendChild(toast);

  setTimeout(() => {
    toast.style.opacity = "0";
    toast.style.transition = "all 0.3s ease";
    setTimeout(() => toast.remove(), 300);
  }, 3500);
}

// Bootstrap
window.addEventListener("DOMContentLoaded", async () => {
  // Trigger staged entrance animation
  requestAnimationFrame(() => {
    document.body.classList.add("app-loaded", "enter-active");
  });

  const init = await callNative("getInitialState");
  if (init && init.success) {
    if (init.memory) updateMemStats(init.memory);
    if (init.config) {
      const c = init.config;
      if (c.themeAccent) {
        applyTheme(c.themeAccent);
      }
      const cfgThresh = document.getElementById("cfgAutoThreshold");
      if (cfgThresh) cfgThresh.checked = !!c.autoRamCleanEnabled;
      const threshVal = c.ramThresholdPercent || 80;
      const rngThresh = document.getElementById("rngThreshold");
      if (rngThresh) rngThresh.value = threshVal;
      const lblThresh = document.getElementById("lblThresholdVal");
      if (lblThresh) lblThresh.innerText = threshVal + "%";
      updateThresholdPills(threshVal);

      const cfgTrimThresh = document.getElementById("cfgTrimOnThreshold");
      if (cfgTrimThresh) cfgTrimThresh.checked = !!c.trimWorkingSetOnThreshold;
      const cfgClearThresh = document.getElementById("cfgClearOnThreshold");
      if (cfgClearThresh) cfgClearThresh.checked = !!c.clearCacheOnThreshold;

      const cfgInt = document.getElementById("cfgAutoInterval");
      if (cfgInt) cfgInt.checked = !!c.autoRamCleanEnabled;
      const intVal = c.ramCleanIntervalMinutes || 15;
      const rngInt = document.getElementById("rngInterval");
      if (rngInt) rngInt.value = intVal;
      const lblInt = document.getElementById("lblIntervalVal");
      if (lblInt) lblInt.innerText = intVal + " мин";
      updateIntervalPills(intVal);

      const cfgTrimInt = document.getElementById("cfgTrimOnInterval");
      if (cfgTrimInt) cfgTrimInt.checked = !!c.trimWorkingSetOnInterval;
      const cfgClearInt = document.getElementById("cfgClearOnInterval");
      if (cfgClearInt) cfgClearInt.checked = !!c.clearCacheOnInterval;

      const cfgBg = document.getElementById("cfgRunInBackground");
      if (cfgBg) cfgBg.checked = (c.runInBackground !== undefined) ? !!c.runInBackground : true;

      const cfgAuto = document.getElementById("cfgAutostart");
      if (cfgAuto) cfgAuto.checked = !!c.autostartWithWindows;

      const descEl = document.getElementById("autoCleanDesc");
      if (descEl) {
        descEl.innerText = `Порог > ${threshVal}% • Каждые ${intVal} мин`;
      }
    }
  }

  // Also query autostart status to ensure sync
  try {
    const autoRes = await callNative("getAutostart");
    if (autoRes && autoRes.success && autoRes.autostart !== undefined) {
      const cfgAuto = document.getElementById("cfgAutostart");
      if (cfgAuto) cfgAuto.checked = !!autoRes.autostart;
    }
  } catch (e) {
    // Ignore if not supported
  }
});
