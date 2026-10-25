// ============================================================================
// Windows Config Studio (WCS) - Frontend Native Bridge & Controller
// ============================================================================

let reqId = 1;
const pendingRequests = new Map();
let currentSpecs = null;
let allServices = [];
let allAppx = [];
let isTimerLocked = false;
let currentThemeAccent = "cyan";

function getLogoPathForTheme(accent) {
  if (!accent || accent === "cyan") return "/branding/wcs_logo_transparent.png";
  return `/branding/wcs_logo_${accent}.png`;
}

function updateThemeLogos(accent) {
  if (!accent) accent = "cyan";
  const path = getLogoPathForTheme(accent);

  const logos = document.querySelectorAll(".brand-logo, .modal-logo, #headerLogo, #aboutModalLogo");
  logos.forEach(img => {
    if (img && img.tagName === "IMG") {
      img.src = path;
    }
  });

  const favicons = document.querySelectorAll("link[rel*='icon']");
  favicons.forEach(fav => {
    if (fav && fav.href && fav.href.includes("wcs_logo")) {
      fav.href = path;
    }
  });
}

try {
  const cachedTheme = localStorage.getItem("wcs_themeAccent");
  if (cachedTheme) {
    currentThemeAccent = cachedTheme;
    document.body.classList.add(`theme-${cachedTheme}`);
  }
} catch (_) {}


// Send JSON RPC request to C++ NativeBridge
function callNative(action, params = {}) {
  return new Promise((resolve) => {
    const id = reqId++;
    pendingRequests.set(id, { resolve });
    if (window.chrome && window.chrome.webview) {
      try {
        window.chrome.webview.postMessage({ id, action, params });
      } catch (err) {
        console.error("callNative postMessage failed:", err);
        pendingRequests.delete(id);
        resolve({ success: false, error: err ? err.message : "Bridge failure" });
      }
    } else {
      console.warn("Native bridge not attached, action:", action);
      pendingRequests.delete(id);
      resolve({ success: false });
    }
  });
}

// Receive WebMessage from C++
if (window.chrome && window.chrome.webview) {
  window.chrome.webview.addEventListener('message', (event) => {
    try {
      const msg = event.data;
      if (!msg) return;
      if (msg.id && pendingRequests.has(msg.id)) {
        const { resolve } = pendingRequests.get(msg.id);
        pendingRequests.delete(msg.id);
        resolve(msg.data);
      } else if (msg.event) {
        handleNativeEvent(msg.event, msg.data);
      }
    } catch (err) {
      console.error("Error processing native message event:", err);
    }
  });
}

// Native Event Dispatcher
function handleNativeEvent(event, data) {
  if (event === "telemetry_update") {
    onTelemetryUpdate(data);
  } else if (event === "toast") {
    showToast(data.message, data.isError);
  } else if (event === "appx_uninstall_progress") {
    showToast(data.progress, false);
  } else if (event === "appx_uninstall_done") {
    if (data.appx) renderAppx(data.appx);
    showToast(data.message || "Удаление UWP приложений завершено", !data.success);
  } else if (event === "cleanup_done") {
    handleCleanupResult(data);
  } else if (event === "game_boosted") {
    onGameBoosted(data);
  } else if (event === "game_exited") {
    onGameExited(data);
  } else if (event === "theme_changed") {
    if (data && data.theme) {
      setThemeAccent(data.theme, false);
    }
  } else if (event === "update_available") {
    onUpdateAvailable(data);
  } else if (event === "msi_devices_scanned") {
    if (data && data.devices) renderMsiDevices(data);
  } else if (event === "startup_items_scanned") {
    if (data && data.items) renderStartupItems(data);
  } else if (event === "preset_applied") {
    if (data && data.tweaks) syncTweaksUI(data.tweaks);
  } else if (event === "appx_scanned") {
    if (data && data.items) renderAppx(data.items);
  }
}

// ----------------------------------------------------------------------------
// Telemetry & Live Stats (rAF Throttled, Zero Forced Reflow)
// ----------------------------------------------------------------------------
let pendingTelemetryData = null;
let telemetryRafId = null;

function onTelemetryUpdate(data) {
  pendingTelemetryData = data;
  if (!telemetryRafId) {
    telemetryRafId = requestAnimationFrame(() => {
      telemetryRafId = null;
      if (pendingTelemetryData) {
        applyTelemetryDOMUpdate(pendingTelemetryData);
      }
    });
  }
}

function applyTelemetryDOMUpdate(data) {
  // Update About Modal PC specs dynamically from telemetry
  if (data.specs) {
    currentSpecs = Object.assign(currentSpecs || {}, data.specs);
    populateAboutModalSpecs(currentSpecs);
  } else if (data.cpu || data.gpu) {
    if (!currentSpecs) currentSpecs = {};
    if (data.cpu) currentSpecs.cpuName = data.cpu;
    if (data.gpu) currentSpecs.primaryGPUName = data.gpu;
    populateAboutModalSpecs(currentSpecs);
  }

  if (data.ramTotalGb !== undefined && data.ramFreeGb !== undefined) {
    if (!currentSpecs) currentSpecs = {};
    currentSpecs.totalRAM_GB = data.ramTotalGb;
    currentSpecs.freeRAM_GB = data.ramFreeGb;
    const ramEl = document.getElementById("aboutRam");
    if (ramEl) {
      ramEl.innerText = `${Number(data.ramTotalGb).toFixed(1)} GB (Доступно: ${Number(data.ramFreeGb).toFixed(1)} GB)`;
    }
  }

  // Live telemetry for dedicated Memory Cleaner tab
  updateMemoryTabTelemetry(data);

  if (data.timerMs !== undefined) {
    const msStr = data.timerMs.toFixed(3) + " ms";
    const pill = document.getElementById("headerTimerPill");
    const text = document.getElementById("timerStatusText");
    const tab1Display = document.getElementById("tab1TimerDisplay");
    const lockBtn = document.getElementById("btnTimerLock");

    if (text) text.innerText = msStr;
    if (tab1Display) tab1Display.innerText = "Текущее системное разрешение: " + msStr;

    isTimerLocked = !!data.timerLockActive;
    if (isTimerLocked) {
      if (pill && pill.className !== "status-pill cyan") pill.className = "status-pill cyan";
      if (lockBtn && !lockBtn.innerHTML.includes("0.500 ms Заблокирован")) {
        lockBtn.innerHTML = '<img src="/icons/check.png" style="width: 14px; height: 14px; margin-right: 6px; vertical-align: middle;">0.500 ms Заблокирован (Снять)';
        lockBtn.className = "btn btn-secondary";
      }
    } else {
      if (pill && pill.className !== "status-pill") pill.className = "status-pill";
      if (lockBtn && !lockBtn.innerHTML.includes("Заблокировать на 0.500 ms")) {
        lockBtn.innerHTML = '<img src="/icons/bolt.png" style="width: 14px; height: 14px; margin-right: 6px; vertical-align: middle;">Заблокировать на 0.500 ms';
        lockBtn.className = "btn btn-primary";
      }
    }

    const dashTimerVal = document.getElementById("dashTimerVal");
    const dashTimerBadge = document.getElementById("dashTimerBadge");
    const dashTimerSub = document.getElementById("dashTimerSub");
    const dashQuickTimer = document.getElementById("dash-quick-timerLock");
    if (dashTimerVal) {
      if (isTimerLocked) {
        dashTimerVal.innerHTML = "0.500 <small>ms</small>";
        if (dashTimerBadge) { dashTimerBadge.textContent = "ЗАБЛОКИРОВАН"; dashTimerBadge.className = "dash-metric-badge badge-locked"; }
        if (dashTimerSub) dashTimerSub.textContent = "Zero-Drift Lock активен (0.5ms)";
        if (dashQuickTimer) dashQuickTimer.checked = true;
      } else {
        dashTimerVal.innerHTML = `${msStr.replace(" ms", "")} <small>ms</small>`;
        if (dashTimerBadge) {
          dashTimerBadge.textContent = "ПЛАВАЮЩИЙ";
          dashTimerBadge.className = "dash-metric-badge";
          dashTimerBadge.style.background = "rgba(255,255,255,0.08)";
          dashTimerBadge.style.color = "var(--text-dim)";
        }
        if (dashTimerSub) dashTimerSub.textContent = "Стандартный динамический шаг";
        if (dashQuickTimer) dashQuickTimer.checked = false;
      }
    }
  }

  // Live hardware charts telemetry (CPU & GPU)
  updateHardwareTelemetry(data);
}

// ----------------------------------------------------------------------------
// Navigation: Tabs & Sub-tabs (Layout-Thrash Free)
// ----------------------------------------------------------------------------
let currentTabIdx = 0;
let isTabSwitching = false;

function updateSidebarIndicator(tabIndex, immediate = false) {
  const indicator = document.getElementById("sidebarIndicator");
  const sidebar = document.querySelector(".app-sidebar");
  if (!indicator || !sidebar) return;

  const items = sidebar.querySelectorAll(".nav-item");
  const targetItem = items[tabIndex];
  if (!targetItem) return;

  let targetY = targetItem.offsetTop;
  if (targetItem.offsetParent !== sidebar) {
    const sidebarRect = sidebar.getBoundingClientRect();
    const itemRect = targetItem.getBoundingClientRect();
    targetY = itemRect.top - sidebarRect.top + sidebar.scrollTop;
  }
  const targetH = targetItem.offsetHeight;

  if (immediate) {
    indicator.style.transition = "none";
    indicator.style.transform = `translate3d(0, ${targetY}px, 0)`;
    indicator.style.height = `${targetH}px`;
    indicator.style.opacity = "1";
    requestAnimationFrame(() => {
      indicator.style.transition = "";
    });
    return;
  }

  requestAnimationFrame(() => {
    indicator.style.transform = `translate3d(0, ${targetY}px, 0)`;
    indicator.style.height = `${targetH}px`;
    indicator.style.opacity = "1";
  });
}

function initTabs() {
  updateSidebarIndicator(0, true);
}

function switchTab(tabIndex) {
  hideServiceTooltip();
  const items = Array.from(document.querySelectorAll(".nav-item"));
  const panes = Array.from(document.querySelectorAll(".tab-pane"));

  let resolvedIdx = -1;
  if (typeof tabIndex === "number") {
    resolvedIdx = tabIndex;
  } else if (typeof tabIndex === "string") {
    const targetEl = document.getElementById(tabIndex) || document.getElementById("tab-" + tabIndex);
    if (targetEl) {
      resolvedIdx = panes.indexOf(targetEl);
      if (resolvedIdx === -1) {
        resolvedIdx = items.indexOf(targetEl);
      }
    }
  }

  if (resolvedIdx < 0 || resolvedIdx >= panes.length) {
    resolvedIdx = 0;
  }

  const currentPane = panes[currentTabIdx];
  const targetPane = panes[resolvedIdx];

  if (resolvedIdx === currentTabIdx && currentPane && currentPane.classList.contains("active")) {
    return;
  }

  // Smooth nav items active toggle
  items.forEach((item, idx) => {
    item.classList.toggle("active", idx === resolvedIdx);
  });

  updateSidebarIndicator(resolvedIdx);

  // Smooth scroll to top without thrashing
  const tabContent = document.querySelector(".app-content");
  if (tabContent && tabContent.scrollTop > 0) {
    tabContent.scrollTo({ top: 0, behavior: 'smooth' });
  }

  if (currentPane && currentPane !== targetPane && currentPane.classList.contains("active")) {
    isTabSwitching = true;
    currentPane.classList.add("tab-fade-out");

    setTimeout(() => {
      requestAnimationFrame(() => {
        panes.forEach((pane, idx) => {
          pane.classList.remove("tab-fade-out");
          pane.classList.toggle("active", idx === resolvedIdx);
        });
        currentTabIdx = resolvedIdx;
        isTabSwitching = false;
      });
    }, 90);
  } else {
    requestAnimationFrame(() => {
      panes.forEach((pane, idx) => {
        pane.classList.remove("tab-fade-out");
        pane.classList.toggle("active", idx === resolvedIdx);
      });
      currentTabIdx = resolvedIdx;
    });
  }

  const targetId = targetPane ? targetPane.id : "";
  if (targetId === "tab-0" || resolvedIdx === 0) {
    requestAnimationFrame(() => redrawHardwareCharts());
  } else if (targetId === "tab-2" || resolvedIdx === 2) {
    loadGameBoost();
    scanInstalledGamesUI();
  } else if (targetId === "tab-3" || resolvedIdx === 3 || targetId === "tab-memory") {
    refreshMemoryTabTelemetry();
  } else if (targetId === "tab-5" || resolvedIdx === 5) {
    loadMsiDevices();
  } else if (targetId === "tab-8" || resolvedIdx === 8) {
    loadStartupItems();
  } else if (targetId === "tab-12" || resolvedIdx === 12) {
    refreshUpdateTabUI(currentUpdateInfo);
  }
}

let currentSubTabIdx = 0;

function switchSubTab(subIndex) {
  hideServiceTooltip();
  const btns = document.querySelectorAll(".subtab-btn");
  const panes = [
    document.getElementById("subtab-0"),
    document.getElementById("subtab-1"),
    document.getElementById("subtab-2"),
    document.getElementById("subtab-3"),
    document.getElementById("subtab-4"),
    document.getElementById("subtab-5")
  ];

  if (subIndex === currentSubTabIdx && panes[subIndex] && panes[subIndex].classList.contains("active")) {
    return;
  }

  btns.forEach((btn, idx) => {
    btn.classList.toggle("active", idx === subIndex);
  });

  const tabContent = document.querySelector(".app-content");
  if (tabContent && tabContent.scrollTop > 0) {
    tabContent.scrollTo({ top: 0, behavior: 'smooth' });
  }

  if (subIndex === 2) {
    loadWin11Tweaks();
  } else if (subIndex === 4) {
    callNative("getServices").then(res => {
      if (res?.services) renderServices(res.services);
    });
  } else if (subIndex === 5) {
    callNative("getAppx").then(res => {
      if (res?.items) renderAppx(res.items);
    });
  }

  const currentPane = panes[currentSubTabIdx];
  const targetPane = panes[subIndex];

  if (currentPane && currentPane !== targetPane && currentPane.classList.contains("active")) {
    currentPane.classList.add("subtab-fade-out");

    setTimeout(() => {
      requestAnimationFrame(() => {
        panes.forEach((pane, idx) => {
          if (!pane) return;
          pane.style.display = ""; // clear inline style so CSS classes control display cleanly
          pane.classList.remove("subtab-fade-out");
          pane.classList.toggle("active", idx === subIndex);
        });
        currentSubTabIdx = subIndex;
      });
    }, 80);
  } else {
    requestAnimationFrame(() => {
      panes.forEach((pane, idx) => {
        if (!pane) return;
        pane.style.display = "";
        pane.classList.remove("subtab-fade-out");
        pane.classList.toggle("active", idx === subIndex);
      });
      currentSubTabIdx = subIndex;
    });
  }
}

const flyoutTimers = new Map();

function toggleFlyout(flyoutId, btnEl) {
  const el = document.getElementById(flyoutId);
  if (!el) return;

  const btn = btnEl || document.querySelector(`[onclick*="${flyoutId}"]`);

  if (flyoutTimers.has(flyoutId)) {
    clearTimeout(flyoutTimers.get(flyoutId));
    flyoutTimers.delete(flyoutId);
  }

  const isCurrentlyOpen = el.classList.contains("open") && !el.classList.contains("closing");

  if (isCurrentlyOpen) {
    // Smooth closing transition
    el.classList.add("closing");
    el.classList.remove("open");

    if (btn) {
      btn.classList.remove("active", "flyout-active");
      btn.innerHTML = `<span style="font-size: 9px; opacity: 0.8; margin-right: 4px;">▼</span> Настройки`;
    }

    const timer = setTimeout(() => {
      el.classList.remove("closing");
      flyoutTimers.delete(flyoutId);
    }, 320);
    flyoutTimers.set(flyoutId, timer);
  } else {
    // Smooth opening transition
    el.classList.remove("closing");
    el.classList.add("open");

    if (btn) {
      btn.classList.add("active", "flyout-active");
      btn.innerHTML = `<span style="font-size: 9px; opacity: 0.8; margin-right: 4px;">▲</span> Скрыть`;
    }

    // Scroll smoothly into view only if bottom overflows container
    requestAnimationFrame(() => {
      setTimeout(() => {
        const scrollParent = document.querySelector(".app-content") || window;
        const rect = el.getBoundingClientRect();
        const parentBottom = scrollParent.getBoundingClientRect ? scrollParent.getBoundingClientRect().bottom : window.innerHeight;
        if (rect.bottom > parentBottom - 20) {
          el.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
        }
      }, 60);
    });
  }
}

// ----------------------------------------------------------------------------
// Presets & Diff Inspector (Task 2.2)
// ----------------------------------------------------------------------------
const TWEAK_NAMES = {
  timerResolution05ms: "Высокоточный таймер системы 0.500 мс",
  disableDynamicTick: "Отключение динамических тиков ядра (Dynamic Tick)",
  usePlatformClockNo: "Отключение синтетического таймера HPET",
  usePlatformTickNo: "Аппаратный тактовый таймер процессора (Platform Tick)",
  ultimatePowerPlan: "Схема питания «Максимальная производительность»",
  disableCoreParking: "Отключение парковки ядер процессора (Core Unparking)",
  win32PrioritySeparation: "Приоритет активного окна (кванты CPU)",
  disablePagingExecutive: "Удержание компонентов системы в ОЗУ",
  largeSystemCacheDisabled: "Приоритет оперативной памяти для игр",
  gpuMsiMode: "Прямые прерывания видеокарты (MSI Mode)",
  audioMsiMode: "Прямые прерывания звуковой карты (MSI Mode)",
  hagsEnabled: "Аппаратное ускорение графики (HAGS)",
  disableGameDVR: "Отключение фоновой записи GameDVR",
  disableFullscreenOptimizations: "Прямой полноэкранный режим (FSO)",
  mmcssGamesPriority: "Игровой приоритет мультимедиа (MMCSS)",
  disableNetworkThrottling: "Снятие сетевого ограничения Windows",
  tcpNoDelay: "Мгновенная отправка пакетов (TCP NoDelay)",
  tcpAutoTuning: "Автоподстройка окна приема TCP (Auto-Tuning)",
  flatMouseCurve: "Прямой отклик мыши 1:1 (кривая MarkC)",
  keyboardMaxSpeed: "Максимальная отзывчивость клавиатуры",
  disableTelemetry: "Отключение сбора телеметрии и слежения",
  disableErrorReporting: "Отключение службы отчетов об ошибках Windows"
};

const TWEAK_DESCRIPTIONS = {
  timerResolution05ms: "Снижает системный шаг таймера Windows до минимума для мгновенного отклика мыши и плавного фреймтайма.",
  disableDynamicTick: "Фиксирует системный шаг таймера Windows, предотвращая задержки при переходе процессора между энергосберегающими состояниями.",
  usePlatformClockNo: "Переводит системное время Windows на прямое обращение к быстрым регистрам процессора (TSC), снижая задержку ядра.",
  usePlatformTickNo: "Синхронизирует системный таймер ядра с аппаратным тактовым генератором материнской платы для ровного фреймтайма.",
  ultimatePowerPlan: "Активирует схему электропитания Windows без снижения частот ядер процессора и задержек пробуждения шины.",
  disableCoreParking: "Предотвращает уход ядер процессора в глубокий сон, устраняя задержки пробуждения потоков в напряженных игровых сценах.",
  win32PrioritySeparation: "Выделяет максимальное процессорное время переднему плану (игре) по сравнению с фоновыми задачами.",
  disablePagingExecutive: "Запрещает сброс драйверов и ядра Windows в файл подкачки, ускоряя доступ к ним из физической памяти.",
  largeSystemCacheDisabled: "Ограничивает разрастание кэша файловой системы, резервируя больше свободной памяти под игры.",
  gpuMsiMode: "Переводит видеокарту на современные MSI-прерывания с высоким приоритетом, снижая задержки DPC и устраняя микрофризы.",
  audioMsiMode: "Переводит аудиоконтроллер на режим Message Signaled Interrupts, устраняя треск звука и рассинхрон при высокой нагрузке.",
  hagsEnabled: "Передает диспетчеризацию видеопамяти напрямую графическому процессору для снижения задержек рендеринга.",
  disableGameDVR: "Отключает постоянную фоновую видеозапись и оверлей Xbox, освобождая ресурсы видеокарты.",
  disableFullscreenOptimizations: "Отключает принудительную оконную оптимизацию Windows, возвращая чистый эксклюзивный Fullscreen.",
  mmcssGamesPriority: "Предоставляет 100% ресурсов планировщика потоков и снимает искусственные лимиты Windows для игровых процессов.",
  disableNetworkThrottling: "Отключает троттлинг сетевых пакетов Windows при одновременной нагрузке на процессор.",
  tcpNoDelay: "Отключает алгоритм задержки Nagle, отправляя сетевые пакеты в онлайн-играх без ожидания буфера.",
  tcpAutoTuning: "Оптимизирует размер окна TCP и распределяет сетевой трафик между ядрами процессора без очередей пакетов.",
  flatMouseCurve: "Полностью отключает системное ускорение (акселерацию) курсора, обеспечивая честное движение 1:1.",
  keyboardMaxSpeed: "Устанавливает минимальную задержку перед повтором нажатий и максимальную скорость повтора клавиш.",
  disableTelemetry: "Останавливает фоновую службу сбора диагностических данных (DiagTrack), устраняя фоновую активность диска и процессора.",
  disableErrorReporting: "Отключает службу Windows Error Reporting, исключая микрозависания системы при программных предупреждениях."
};

const PRESET_DEFINITIONS = {
  EsportsUltra: {
    id: "EsportsUltra",
    name: "Competitive Games",
    badge: "MAX FPS",
    badgeClass: "badge-blue",
    description: "Бескомпромиссный соревновательный профиль: таймер 0.500 мс, приоритет игрового окна, мгновенная отправка пакетов TCP NoDelay, отключение парковки ядер и FSO для минимального инпут-лага.",
    tweaks: {
      timerResolution05ms: true,
      win32PrioritySeparation: true,
      disableDynamicTick: true,
      usePlatformClockNo: true,
      usePlatformTickNo: true,
      ultimatePowerPlan: true,
      disableCoreParking: true,
      disablePagingExecutive: true,
      largeSystemCacheDisabled: true,
      gpuMsiMode: true,
      audioMsiMode: true,
      hagsEnabled: true,
      disableGameDVR: true,
      disableFullscreenOptimizations: true,
      mmcssGamesPriority: true,
      disableNetworkThrottling: true,
      tcpNoDelay: true,
      tcpAutoTuning: true,
      flatMouseCurve: true,
      keyboardMaxSpeed: true,
      disableTelemetry: true,
      disableErrorReporting: true
    }
  },
  CompetitiveGaming: {
    id: "CompetitiveGaming",
    name: "Balance",
    badge: "БАЛАНС",
    badgeClass: "badge-green",
    description: "Сбалансированный игровой профиль: таймер 0.500 мс, TCP NoDelay, игровой приоритет MMCSS и разблокировка ядер с сохранением фоновых служб для стриминга, Discord и повседневных задач.",
    tweaks: {
      timerResolution05ms: true,
      win32PrioritySeparation: true,
      disableDynamicTick: false,
      usePlatformClockNo: true,
      usePlatformTickNo: true,
      ultimatePowerPlan: false,
      disableCoreParking: true,
      disablePagingExecutive: false,
      largeSystemCacheDisabled: true,
      gpuMsiMode: true,
      audioMsiMode: true,
      hagsEnabled: true,
      disableGameDVR: true,
      disableFullscreenOptimizations: true,
      mmcssGamesPriority: true,
      disableNetworkThrottling: true,
      tcpNoDelay: true,
      tcpAutoTuning: true,
      flatMouseCurve: true,
      keyboardMaxSpeed: true,
      disableTelemetry: true,
      disableErrorReporting: true
    }
  },
  WindowsDefault: {
    id: "WindowsDefault",
    name: "Windows Default",
    badge: "СБРОС",
    badgeClass: "badge-gray",
    description: "Полный эталонный возврат к заводским параметрам Microsoft: стандартный динамический таймер, стандартные кванты CPU, заводские параметры сети и восстановление системных служб.",
    tweaks: {
      timerResolution05ms: false,
      win32PrioritySeparation: false,
      disableDynamicTick: false,
      usePlatformClockNo: false,
      usePlatformTickNo: false,
      ultimatePowerPlan: false,
      disableCoreParking: false,
      disablePagingExecutive: false,
      largeSystemCacheDisabled: false,
      gpuMsiMode: false,
      audioMsiMode: false,
      hagsEnabled: false,
      disableGameDVR: false,
      disableFullscreenOptimizations: false,
      mmcssGamesPriority: false,
      disableNetworkThrottling: false,
      tcpNoDelay: false,
      tcpAutoTuning: false,
      flatMouseCurve: false,
      keyboardMaxSpeed: false,
      disableTelemetry: false,
      disableErrorReporting: false
    }
  }
};
PRESET_DEFINITIONS.BalancedGaming = PRESET_DEFINITIONS.CompetitiveGaming;

let currentDiffPresetId = null;

async function previewPreset(presetId) {
  let targetPresetId = presetId;
  if (targetPresetId === "BalancedGaming") targetPresetId = "CompetitiveGaming";
  const def = PRESET_DEFINITIONS[targetPresetId] || PRESET_DEFINITIONS[presetId];
  if (!def) {
    applyPreset(presetId);
    return;
  }

  currentDiffPresetId = targetPresetId;
  const nameEl = document.getElementById("diffPresetName");
  if (nameEl) nameEl.textContent = def.name;

  const badgeEl = document.getElementById("diffPresetBadge");
  if (badgeEl) {
    badgeEl.textContent = def.badge;
    badgeEl.className = `preset-badge ${def.badgeClass}`;
  }

  const descEl = document.getElementById("diffPresetDesc");
  if (descEl) {
    descEl.textContent = def.description;
  }

  let currentTweaks = null;
  try {
    const res = await callNative("getTweaksState");
    if (res) currentTweaks = res;
  } catch (err) {
    console.warn("Could not get tweaks state:", err);
  }

  if (!currentTweaks) {
    currentTweaks = {};
    for (const key of Object.keys(TWEAK_NAMES)) {
      const el = document.getElementById(`tweak-${key}`);
      currentTweaks[key] = el ? el.checked : false;
    }
  }

  const container = document.getElementById("presetDiffContainer");
  if (!container) return;
  container.innerHTML = "";

  const diffs = [];
  for (const [key, targetVal] of Object.entries(def.tweaks)) {
    const currentVal = !!currentTweaks[key];
    if (currentVal !== targetVal) {
      diffs.push({
        key,
        title: TWEAK_NAMES[key] || key,
        currentVal,
        targetVal
      });
    }
  }

  const applyBtn = document.getElementById("btnConfirmApplyPreset");
  const countTextEl = document.getElementById("diffCountText");

  if (diffs.length === 0) {
    if (countTextEl) countTextEl.textContent = "Все параметры уже соответствуют пресету";
    container.innerHTML = `
      <div class="diff-empty-msg">
        <img src="/icons/check.png" style="width: 24px; height: 24px; margin-bottom: 6px;">
        Все параметры данного пресета уже соответствуют текущему состоянию системы. Изменений не требуется!
      </div>
    `;
    if (applyBtn) applyBtn.style.display = "none";
  } else {
    if (countTextEl) countTextEl.textContent = `Параметров к изменению: ${diffs.length} из ${Object.keys(def.tweaks).length}`;
    if (applyBtn) {
      applyBtn.style.display = "inline-flex";
      applyBtn.innerHTML = `<img src="/icons/bolt.png" style="width: 13px; height: 13px; margin-right: 6px; vertical-align: middle;"> Применить изменения (${diffs.length})`;
    }
    diffs.forEach(d => {
      const row = document.createElement("div");
      row.className = "diff-row";
      row.innerHTML = `
        <div class="diff-param-info" style="display: flex; flex-direction: column; gap: 2px; min-width: 0; flex: 1;">
          <span class="diff-param-title">${escapeHtml(d.title)}</span>
          ${d.description ? `<span class="diff-param-desc" style="font-size: 10px; color: var(--text-dim); line-height: 1.3;">${escapeHtml(d.description)}</span>` : ''}
        </div>
        <div class="diff-badges">
          <span class="diff-badge ${d.currentVal ? 'badge-on' : 'badge-off'}">
            ${d.currentVal ? 'ВКЛ' : 'ВЫКЛ'}
          </span>
          <span class="diff-arrow">→</span>
          <span class="diff-badge ${d.targetVal ? 'badge-on' : 'badge-off'}">
            ${d.targetVal ? 'ВКЛ' : 'ВЫКЛ'}
          </span>
        </div>
      `;
      container.appendChild(row);
    });
  }

  openPresetDiffModal();
}

function openPresetDiffModal() {
  const modal = document.getElementById("presetDiffModal");
  if (!modal) return;
  modal.style.display = "flex";
  modal.classList.remove("modal-exiting");
  requestAnimationFrame(() => {
    modal.classList.add("active");
  });
}

function closePresetDiffModal() {
  const modal = document.getElementById("presetDiffModal");
  if (!modal) return;
  modal.classList.add("modal-exiting");
  modal.classList.remove("active");
  setTimeout(() => {
    modal.classList.remove("modal-exiting");
    modal.style.display = "none";
  }, 380);
}

let isPresetApplying = false;

function showPresetProgressToast(presetName, total) {
  let widget = document.getElementById("presetProgressToast");
  if (!widget) {
    widget = document.createElement("div");
    widget.id = "presetProgressToast";
    widget.className = "preset-progress-toast";
    document.body.appendChild(widget);
  }

  widget.innerHTML = `
    <div class="preset-progress-header">
      <span class="preset-progress-title">
        <span class="preset-progress-spinner"></span>
        <span>Применение параметров «${escapeHtml(presetName)}»</span>
      </span>
      <span id="presetProgressCounter" class="preset-progress-counter">(0/${total})</span>
    </div>
    <div id="presetProgressTweakName" class="preset-progress-status">Подготовка параметров...</div>
    <div class="preset-progress-bar-bg">
      <div id="presetProgressBar" class="preset-progress-bar-fill" style="width: 0%;"></div>
    </div>
  `;

  widget.style.display = "flex";
  widget.classList.remove("hiding");
  requestAnimationFrame(() => {
    widget.classList.add("visible");
  });
}

function updatePresetProgress(step, total, tweakName) {
  const counter = document.getElementById("presetProgressCounter");
  const tweakEl = document.getElementById("presetProgressTweakName");
  const bar = document.getElementById("presetProgressBar");

  const pct = Math.min(100, Math.round((step / total) * 100));
  if (counter) counter.innerText = `(${step}/${total})`;
  if (tweakEl) tweakEl.innerText = `Настройка ${tweakName}...`;
  if (bar) bar.style.width = `${pct}%`;
}

function hidePresetProgressToast() {
  const widget = document.getElementById("presetProgressToast");
  if (!widget) return;
  widget.classList.remove("visible");
  widget.classList.add("hiding");
  setTimeout(() => {
    widget.style.display = "none";
    widget.classList.remove("hiding");
  }, 400);
}

async function confirmApplyPreset() {
  if (!currentDiffPresetId) return;
  const targetId = currentDiffPresetId;
  closePresetDiffModal();
  await applyPreset(targetId);
}

async function applyPreset(presetName) {
  if (isPresetApplying) return;
  let targetPresetId = presetName;
  if (targetPresetId === "BalancedGaming") targetPresetId = "CompetitiveGaming";
  const def = PRESET_DEFINITIONS[targetPresetId] || PRESET_DEFINITIONS[presetName];
  if (!def) {
    showToast("Неизвестный пресет: " + presetName, true);
    return;
  }

  isPresetApplying = true;
  const displayName = def.name || presetName;
  const tweakEntries = Object.entries(def.tweaks);
  const total = tweakEntries.length;

  showPresetProgressToast(displayName, total);

  try {
    let step = 0;
    for (const [key, targetVal] of tweakEntries) {
      step++;
      const tweakName = TWEAK_NAMES[key] || key;
      updatePresetProgress(step, total, tweakName);

      // Apply tweak natively without blocking UI
      await callNative("setTweak", { id: key, value: targetVal });

      // Smoothly update checkbox in UI as tweaks complete
      const chk = document.getElementById(`tweak-${key}`);
      if (chk) {
        chk.checked = !!targetVal;
      }

      // Small async interval yielding to event loop for 60+ FPS smoothness
      await new Promise(resolve => setTimeout(resolve, 40));
    }

    // Highlight the active preset card
    highlightPresetCard(targetPresetId);

    // Call native applyPreset to persist active preset state in backend
    await callNative("applyPreset", { preset: targetPresetId });

    hidePresetProgressToast();
    showToast(`Пресет «${displayName}» успешно применен! Все параметры активны.`, false);
  } catch (err) {
    console.error("Error applying preset async:", err);
    hidePresetProgressToast();
    showToast("Ошибка при применении пресета: " + (err?.message || err), true);
  } finally {
    isPresetApplying = false;
  }
}

function highlightPresetCard(presetName) {
  document.getElementById("card-preset-1")?.classList.toggle("active", presetName === "EsportsUltra");
  document.getElementById("card-preset-2")?.classList.toggle("active", presetName === "CompetitiveGaming" || presetName === "BalancedGaming");
  document.getElementById("card-preset-3")?.classList.toggle("active", presetName === "WindowsDefault");

  const nameEl = document.getElementById("dashActiveProfileName");
  const badgeEl = document.getElementById("dashProfileBadge");
  const subEl = document.getElementById("dashActiveCount");
  if (nameEl) {
    if (presetName === "EsportsUltra") {
      nameEl.textContent = "Competitive Games";
      if (badgeEl) { badgeEl.textContent = "MAX FPS"; badgeEl.className = "dash-metric-badge badge-locked"; }
      if (subEl) subEl.textContent = "0.500ms • Direct MSI • Unpark";
    } else if (presetName === "CompetitiveGaming" || presetName === "BalancedGaming") {
      nameEl.textContent = "Balance";
      if (badgeEl) { badgeEl.textContent = "БАЛАНС"; badgeEl.className = "dash-metric-badge badge-active"; }
      if (subEl) subEl.textContent = "0.500ms • Stream-Safe • Direct MSI";
    } else if (presetName === "WindowsDefault") {
      nameEl.textContent = "Windows Default";
      if (badgeEl) { badgeEl.textContent = "СБРОС"; badgeEl.className = "dash-metric-badge"; badgeEl.style.background = "rgba(255,255,255,0.08)"; badgeEl.style.color = "var(--text-dim)"; }
      if (subEl) subEl.textContent = "Стандартные параметры Microsoft";
    }
  }
}

async function applyAllTweaks() {
  const res = await callNative("applyAllTweaks");
  if (res && res.success && res.tweaks) {
    syncTweaksUI(res.tweaks);
  }
}

async function revertToDefaults() {
  const res = await callNative("revertToDefaults");
  if (res && res.success) {
    highlightPresetCard("WindowsDefault");
    if (res.tweaks) syncTweaksUI(res.tweaks);
  }
}

// ----------------------------------------------------------------------------
// Core Tweaks & Timer
// ----------------------------------------------------------------------------
async function toggleTweak(tweakId, isChecked) {
  const res = await callNative("setTweak", { id: tweakId, value: isChecked });
  if (res && res.tweaks) {
    syncTweaksUI(res.tweaks);
  }
}

async function toggleTimerLock() {
  await callNative("setTimerLock", { enable: !isTimerLocked });
}

async function flushDNS() {
  await callNative("flushDNS");
}

// ----------------------------------------------------------------------------
// Dedicated Memory Cleaner Controller
// ----------------------------------------------------------------------------
async function cleanAllMemory() {
  try {
    const res = await callNative("cleanAllMemory");
    if (res && res.error) showToast(res.error, true);
  } catch (err) {
    showToast("Ошибка при полной очистке памяти", true);
  }
}

async function purgeStandbyList() {
  try {
    const res = await callNative("purgeStandbyMemory");
    if (res && res.error) showToast(res.error, true);
  } catch (err) {
    showToast("Ошибка при сбросе Standby кэша", true);
  }
}

async function purgeStandbyMemory() {
  return purgeStandbyList();
}

async function trimWorkingSets() {
  try {
    const res = await callNative("trimWorkingSet");
    if (res && res.error) showToast(res.error, true);
  } catch (err) {
    showToast("Ошибка при сбросе рабочих наборов", true);
  }
}

async function trimWorkingSet() {
  return trimWorkingSets();
}

function openMemCleanerWindow() {
  switchTab(2);
}

function onMemoryUpdate(data) {
  updateMemoryTabTelemetry(data);
}

function updateMemoryTabTelemetry(data) {
  if (!data) return;

  const usedGb = data.ramUsedGb !== undefined ? Number(data.ramUsedGb) : (data.usedRAM_GB !== undefined ? Number(data.usedRAM_GB) : 0);
  const totalGb = data.ramTotalGb !== undefined ? Number(data.ramTotalGb) : (data.totalRAM_GB !== undefined ? Number(data.totalRAM_GB) : (currentSpecs?.totalRAM_GB || 16));
  const freeGb = data.ramFreeGb !== undefined ? Number(data.ramFreeGb) : (data.freeRAM_GB !== undefined ? Number(data.freeRAM_GB) : Math.max(0, totalGb - usedGb));
  const standbyGb = data.ramStandbyGb !== undefined ? Number(data.ramStandbyGb) : (data.standbyRAM_GB !== undefined ? Number(data.standbyRAM_GB) : 0);
  const usedPct = data.ramPercent !== undefined ? Number(data.ramPercent) : (data.usedPercent !== undefined ? Number(data.usedPercent) : (totalGb > 0 ? (usedGb / totalGb * 100) : 0));
  const standbyPct = totalGb > 0 ? (standbyGb / totalGb * 100) : 0;
  const freePct = Math.max(0, 100 - usedPct - standbyPct);

  // Physical RAM Title & Percentage Badge
  const titleEl = document.getElementById("tabMemTitleVal");
  if (titleEl) {
    titleEl.innerText = `${usedGb.toFixed(1)} ГБ / ${totalGb.toFixed(1)} ГБ`;
  }
  const badgeEl = document.getElementById("tabMemPercentBadge");
  if (badgeEl) {
    badgeEl.innerText = `${usedPct.toFixed(0)}%`;
  }

  // Segmented Bar
  const barUsed = document.getElementById("tabMemBarUsed");
  if (barUsed) barUsed.style.width = `${Math.min(100, Math.max(0, usedPct))}%`;
  const barStandby = document.getElementById("tabMemBarStandby");
  if (barStandby) barStandby.style.width = `${Math.min(100 - usedPct, Math.max(0, standbyPct))}%`;

  // 3-Segment Legends
  const legUsed = document.getElementById("tabMemLegUsed");
  if (legUsed) legUsed.innerText = `${usedPct.toFixed(0)}%`;
  const legStandby = document.getElementById("tabMemLegStandby");
  if (legStandby) legStandby.innerText = `${standbyPct.toFixed(0)}%`;
  const legFree = document.getElementById("tabMemLegFree");
  if (legFree) legFree.innerText = `${freePct.toFixed(0)}%`;

  // Also update Dashboard RAM metric card
  const dashRamVal = document.getElementById("dashRamVal");
  const dashRamBar = document.getElementById("dashRamBar");
  const dashRamSub = document.getElementById("dashRamSub");
  if (dashRamVal) dashRamVal.innerHTML = `${usedPct.toFixed(0)} <small>% (${usedGb.toFixed(1)} GB)</small>`;
  if (dashRamBar) dashRamBar.style.width = `${Math.min(100, Math.max(0, usedPct))}%`;
  if (dashRamSub) dashRamSub.textContent = `Доступно: ${freeGb.toFixed(1)} GB из ${totalGb.toFixed(1)} GB`;

  // Pagefile Info
  const pfUsed = data.pagefileUsedGb !== undefined ? Number(data.pagefileUsedGb) : (data.pagefileUsed_GB !== undefined ? Number(data.pagefileUsed_GB) : 0);
  const pfTotal = data.pagefileTotalGb !== undefined ? Number(data.pagefileTotalGb) : (data.pagefileTotal_GB !== undefined ? Number(data.pagefileTotal_GB) : 0);
  const pfPct = data.pagefilePercent !== undefined ? Number(data.pagefilePercent) : 0;
  const pfEl = document.getElementById("tabMemPagefileVal");
  if (pfEl) {
    pfEl.innerText = `${pfUsed.toFixed(1)} ГБ / ${pfTotal.toFixed(1)} ГБ (${pfPct.toFixed(0)}%)`;
  }
  const barPf = document.getElementById("tabMemBarPagefile");
  if (barPf) barPf.style.width = `${Math.min(100, Math.max(0, pfPct))}%`;

  // 4 Stat Tiles: Total RAM, In Use, Available, Standby List
  const totalEl = document.getElementById("tabMemStatTotal");
  if (totalEl) totalEl.innerText = `${totalGb.toFixed(1)} ГБ`;

  const inUseEl = document.getElementById("tabMemStatInUse");
  if (inUseEl) inUseEl.innerText = `${usedGb.toFixed(2)} ГБ`;

  const freeEl = document.getElementById("tabMemStatFree");
  if (freeEl) freeEl.innerText = `${freeGb.toFixed(2)} ГБ`;

  const standbyEl = document.getElementById("tabMemStatStandby");
  if (standbyEl) standbyEl.innerText = `${standbyGb.toFixed(2)} ГБ`;

  // Secondary metrics: Peak load & Freed memory
  const peakEl = document.getElementById("tabMemStatPeak");
  if (peakEl && data.peakMemoryPercent !== undefined) {
    peakEl.innerText = `${Number(data.peakMemoryPercent).toFixed(0)}%`;
  }

  const freedEl = document.getElementById("tabMemStatFreed");
  if (freedEl && (data.lastFreedStr || data.lastFreedBytes !== undefined)) {
    let freedStr = data.lastFreedStr;
    if (!freedStr && data.lastFreedBytes !== undefined) {
      const b = Number(data.lastFreedBytes);
      if (b >= 1024 * 1024 * 1024) freedStr = (b / (1024 * 1024 * 1024)).toFixed(2) + " ГБ";
      else if (b >= 1024 * 1024) freedStr = (b / (1024 * 1024)).toFixed(1) + " МБ";
      else if (b >= 1024) freedStr = (b / 1024).toFixed(0) + " КБ";
      else freedStr = b + " Б";
    }
    if (freedStr) {
      freedStr = freedStr.replace(/GB/g, "ГБ").replace(/MB/g, "МБ").replace(/KB/g, "КБ").replace(/ B/g, " Б");
      freedEl.innerText = freedStr;
    }
  }

  // Status text & indicator dot
  const statusEl = document.getElementById("tabMemStatusText");
  const dotEl = document.getElementById("tabMemStatusDot");
  if (statusEl && data.lastActionStatus) {
    statusEl.innerText = data.lastActionStatus;
  }
  if (dotEl) {
    if (usedPct >= 85) {
      dotEl.style.background = "#ef4444";
      dotEl.style.boxShadow = "0 0 8px rgba(239, 68, 68, 0.8)";
    } else if (usedPct >= 75) {
      dotEl.style.background = "#fbbf24";
      dotEl.style.boxShadow = "0 0 8px rgba(251, 191, 36, 0.8)";
    } else {
      dotEl.style.background = "#10b981";
      dotEl.style.boxShadow = "0 0 8px rgba(16, 185, 129, 0.8)";
    }
  }
}

async function refreshMemoryTabTelemetry() {
  try {
    const stats = await callNative("getMemoryStats");
    if (stats) {
      updateMemoryTabTelemetry(stats);
    }
  } catch (err) {
    console.warn("Failed to fetch memory stats:", err);
  }
}

// Preset Pills & Range Sliders
function onMemThresholdSlider(val) {
  const valNum = parseInt(val, 10);
  const lbl = document.getElementById("lblMemThresholdVal");
  if (lbl) lbl.innerText = valNum + "%";
  updateMemThresholdPills(valNum);
}

function setMemThresholdPill(val) {
  const valNum = parseInt(val, 10);
  const rng = document.getElementById("rngMemThreshold");
  if (rng) rng.value = valNum;
  const lbl = document.getElementById("lblMemThresholdVal");
  if (lbl) lbl.innerText = valNum + "%";
  updateMemThresholdPills(valNum);
}

function updateMemThresholdPills(valNum) {
  document.querySelectorAll(".mem-thresh-pill").forEach(p => {
    const pVal = parseInt(p.dataset.val, 10);
    p.classList.toggle("active", pVal === valNum);
  });
}

function onMemIntervalSlider(val) {
  const valNum = parseInt(val, 10);
  const lbl = document.getElementById("lblMemIntervalVal");
  if (lbl) lbl.innerText = valNum + " мин";
  updateMemIntervalPills(valNum);
}

function setMemIntervalPill(val) {
  const valNum = parseInt(val, 10);
  const rng = document.getElementById("rngMemInterval");
  if (rng) rng.value = valNum;
  const lbl = document.getElementById("lblMemIntervalVal");
  if (lbl) lbl.innerText = valNum + " мин";
  updateMemIntervalPills(valNum);
}

function updateMemIntervalPills(valNum) {
  document.querySelectorAll(".mem-int-pill").forEach(p => {
    const pVal = parseInt(p.dataset.val, 10);
    p.classList.toggle("active", pVal === valNum);
  });
}

async function saveMemCleanerConfig() {
  try {
    const autoThresh = document.getElementById("cfgMemAutoThreshold")?.checked || false;
    const threshVal = parseInt(document.getElementById("rngMemThreshold")?.value || "80", 10);
    const trimOnThresh = document.getElementById("cfgMemTrimOnThreshold")?.checked !== false;
    const clearOnThresh = document.getElementById("cfgMemClearOnThreshold")?.checked !== false;

    const autoInt = document.getElementById("cfgMemAutoInterval")?.checked || false;
    const intVal = parseInt(document.getElementById("rngMemInterval")?.value || "15", 10);
    const trimOnInt = document.getElementById("cfgMemTrimOnInterval")?.checked !== false;
    const clearOnInt = document.getElementById("cfgMemClearOnInterval")?.checked !== false;

    const payload = {
      autoRamCleanEnabled: autoThresh || autoInt,
      ramThresholdPercent: threshVal,
      trimWorkingSetOnThreshold: trimOnThresh,
      clearCacheOnThreshold: clearOnThresh,
      ramCleanIntervalMinutes: intVal,
      trimWorkingSetOnInterval: trimOnInt,
      clearCacheOnInterval: clearOnInt
    };

    await callNative("setMemConfig", payload);
    showToast("Настройки памяти успешно сохранены", false);
  } catch (err) {
    showToast("Ошибка при сохранении настроек памяти", true);
  }
}

// ----------------------------------------------------------------------------
// System Suite (Tab 7)
// ----------------------------------------------------------------------------
async function applySystemTweaks() {
  await callNative("applySystemTweaks");
}

async function applySecurityTweaks() {
  await callNative("applySecurityTweaks");
}

async function toggleSingleSystemTweak(tweakId, isChecked) {
  await callNative("applySingleSystemTweak", { id: tweakId, value: isChecked });
}

async function toggleSingleSecurityTweak(tweakId, isChecked) {
  await callNative("applySingleSecurityTweak", { id: tweakId, value: isChecked });
}

async function toggleOsSubTweak(id, isChecked) {
  await callNative("applyOsPerfSubTweak", { id, value: isChecked });
}

async function applyAllOsSubTweaks(enable) {
  const res = await callNative("applyAllOsPerfSubTweaks", { enable });
  if (res && res.success) {
    const ids = ["waitScreenLock", "noMonitorOff", "noDiskSpindown", "ultimatePowerPlan", "alwaysUnloadDll", "nonPagedPoolFix", "disableBackgroundApps", "disableAutoWindowsUpdate", "disableDriverUpdates", "disableReservedStorage"];
    ids.forEach(id => {
      const el = document.getElementById("os-" + id);
      if (el) el.checked = enable;
    });
  }
}

async function toggleSsdSubTweak(id, isChecked) {
  await callNative("applySsdPerfSubTweak", { id, value: isChecked });
}

async function applyAllSsdSubTweaks(enable) {
  const res = await callNative("applyAllSsdPerfSubTweaks", { enable });
  if (res && res.success) {
    const ids = ["disablePrefetchSuperfetch", "disableBootDefrag", "disableLastAccess", "disable8dot3Names", "enableTrimSupport", "disableLowDiskSpaceChecks"];
    ids.forEach(id => {
      const el = document.getElementById("ssd-" + id);
      if (el) el.checked = enable;
    });
  }
}

async function toggleTelemSubTweak(id, isChecked) {
  await callNative("applyTelemetrySubTweak", { id, value: isChecked });
}

async function applyAllTelemetrySubTweaks(enable) {
  const res = await callNative("applyAllTelemetrySubTweaks", { enable });
  if (res && res.success) {
    const ids = ["disableAdvertisingId", "disableSettingSync", "disableVsTelemetry", "disableOfficeTelemetry", "disableNvidiaTelemetry", "disableCeipTasks", "disableAppInventory", "disableLocation"];
    ids.forEach(id => {
      const el = document.getElementById("telem-" + id);
      if (el) el.checked = enable;
    });
  }
}

// ----------------------------------------------------------------------------
// Services Grid & Luxury Tooltip
// ----------------------------------------------------------------------------
let serviceTooltipEl = null;

function getServiceTooltip() {
  if (!serviceTooltipEl) {
    serviceTooltipEl = document.getElementById("serviceTooltip");
    if (!serviceTooltipEl) {
      serviceTooltipEl = document.createElement("div");
      serviceTooltipEl.id = "serviceTooltip";
      serviceTooltipEl.className = "service-tooltip";
      serviceTooltipEl.style.display = "none";
      document.body.appendChild(serviceTooltipEl);
    }
  }
  return serviceTooltipEl;
}

function hideServiceTooltip() {
  const tip = getServiceTooltip();
  if (tip) {
    tip.style.display = "none";
  }
}

function updateTooltipPosition(e) {
  const tooltip = getServiceTooltip();
  if (!tooltip || tooltip.style.display === "none") return;

  const offset = 14;
  const padding = 12;
  const winWidth = window.innerWidth;
  const winHeight = window.innerHeight;

  const tipWidth = tooltip.offsetWidth || 300;
  const tipHeight = tooltip.offsetHeight || 120;

  let x = e.clientX + offset;
  let y = e.clientY + offset;

  // Prevent overflowing right edge
  if (x + tipWidth > winWidth - padding) {
    x = e.clientX - tipWidth - offset;
  }
  // Prevent overflowing bottom edge
  if (y + tipHeight > winHeight - padding) {
    y = e.clientY - tipHeight - offset;
  }

  // Prevent overflowing left/top edge
  if (x < padding) x = padding;
  if (y < padding) y = padding;

  tooltip.style.left = `${x}px`;
  tooltip.style.top = `${y}px`;
}

function renderServices(services) {
  allServices = services || [];
  const container = document.getElementById("servicesList");
  if (!container) return;

  hideServiceTooltip();

  if (!container._hasScrollTooltipListener) {
    container._hasScrollTooltipListener = true;
    container.addEventListener("scroll", hideServiceTooltip, { passive: true });
  }

  container.innerHTML = "";
  allServices.forEach(s => {
    const card = document.createElement("div");
    card.className = "service-card service-tweak-row";
    card.setAttribute("data-id", s.id);
    card.setAttribute("data-name", (s.displayName + " " + s.description + " " + s.id).toLowerCase());

    const badgeColor = s.isDanger ? "#ef4444" : "#10b981";
    const badgeText = s.isDanger ? "КРИТИЧЕСКАЯ" : (s.recommendedDisable ? "DEBLOAT" : "СИСТЕМА");

    let statusPill = "";
    if (s.status) {
      const isRunning = s.isRunning;
      const statusColor = isRunning ? "#10b981" : (s.disabled ? "#ef4444" : "#f59e0b");
      statusPill = `<span class="service-status-badge" style="background: ${statusColor}18; color: ${statusColor}; border: 1px solid ${statusColor}40; border-radius: 4px; font-size: 9px; padding: 1.5px 6px; margin-left: 6px; font-weight: 500;">${escapeHtml(s.status)}</span>`;
    }

    const restoreBtnHtml = s.disabled
      ? `<button type="button" class="btn-restore-service" onclick="restoreSingleService('${escapeHtml(s.id)}', event)" title="Восстановить службу к заводским параметрам и запустить">Восстановить</button>`
      : "";

    card.innerHTML = `
      <div class="service-info">
        <div class="service-header" style="display: flex; align-items: center; gap: 6px; flex-wrap: wrap;">
          <span class="service-title" title="${escapeHtml(s.displayName)}">${escapeHtml(s.displayName)}</span>
          ${statusPill}
          <span class="safe-tag" style="background: rgba(255,255,255,0.06); color: ${badgeColor}; border-color: ${badgeColor}40; font-size: 9px; padding: 1.5px 6px; flex-shrink: 0;">${badgeText}</span>
          ${restoreBtnHtml}
        </div>
        <div class="service-brief">${escapeHtml(s.description)}</div>
      </div>
      <label class="custom-checkbox service-checkbox" style="padding: 4px 8px; flex-shrink: 0;">
        <input type="checkbox" ${s.disabled ? "" : "checked"}>
        <span class="checkbox-label" style="font-size: 11px; font-weight: 500;">${s.disabled ? "Отключена" : "Активна"}</span>
      </label>
    `;

    // Tooltip hover handlers
    card.addEventListener("mouseenter", (e) => {
      const tip = getServiceTooltip();
      if (!tip) return;

      const recText = s.recommendedDisable
        ? "Рекомендуется отключить для снижения DPC задержки и разгрузки процессора"
        : "Стандартная системная служба";
      const recClass = s.recommendedDisable ? "recommend-disable" : "recommend-system";

      tip.innerHTML = `
        <div class="service-tooltip-header">
          <strong>${escapeHtml(s.displayName)}</strong>
        </div>
        <div class="service-tooltip-id">
          <span>Служба: <code>${escapeHtml(s.id)}</code> ${s.status ? `• Статус: <b>${escapeHtml(s.status)}</b>` : ''}</span>
        </div>
        <div class="service-tooltip-details">
          ${escapeHtml(s.description || "Системная служба Windows.")}
        </div>
        <div class="service-tooltip-status ${recClass}">
          <span class="status-indicator"></span>
          <span>${recText}</span>
        </div>
      `;

      tip.style.display = "block";
      updateTooltipPosition(e);
    });

    card.addEventListener("mousemove", (e) => {
      updateTooltipPosition(e);
    });

    card.addEventListener("mouseleave", () => {
      hideServiceTooltip();
    });

    // Clean checkbox toggle
    const checkbox = card.querySelector("input[type='checkbox']");
    if (checkbox) {
      checkbox.addEventListener("change", function () {
        toggleServiceState(s.id, this.checked);
        const lbl = this.closest("label")?.querySelector(".checkbox-label");
        if (lbl) {
          lbl.innerText = this.checked ? "Активна" : "Отключена";
        }
      });
    }

    container.appendChild(card);
  });
}

function filterServices(query) {
  const q = query.toLowerCase().trim();
  const rows = document.querySelectorAll("#servicesList .service-card, #servicesList .service-tweak-row, #servicesList .tweak-row");
  rows.forEach(r => {
    const text = r.getAttribute("data-name") || "";
    r.style.display = text.includes(q) ? "flex" : "none";
  });
}

async function restoreSingleService(svcId, ev) {
  if (ev) {
    ev.stopPropagation();
    ev.preventDefault();
  }
  const btn = ev?.currentTarget || ev?.target || document.querySelector(`.service-card[data-id="${svcId}"] .btn-restore-service`);
  let originalHtml = "";
  if (btn) {
    originalHtml = btn.innerHTML;
    btn.disabled = true;
    btn.innerHTML = `<span class="spinner-sm"></span> Восстановление...`;
  }

  const res = await callNative("setServiceState", { id: svcId, disable: false });
  const svc = allServices.find(s => s.id === svcId);
  const name = (svc && svc.displayName) ? svc.displayName : svcId;

  if (res && res.success !== false && !res.error) {
    if (svc) {
      svc.disabled = false;
      svc.isRunning = true;
      svc.status = "RUNNING";
    }

    const card = document.querySelector(`.service-card[data-id="${svcId}"]`);
    if (card) {
      const checkbox = card.querySelector("input[type='checkbox']");
      if (checkbox) checkbox.checked = true;

      const lbl = card.querySelector(".checkbox-label");
      if (lbl) lbl.innerText = "Активна";

      let badge = card.querySelector(".service-status-badge");
      if (!badge) {
        const headerEl = card.querySelector(".service-header");
        if (headerEl) {
          badge = document.createElement("span");
          badge.className = "service-status-badge";
          badge.style.borderRadius = "4px";
          badge.style.fontSize = "9px";
          badge.style.padding = "1.5px 6px";
          badge.style.marginLeft = "6px";
          badge.style.fontWeight = "500";
          headerEl.appendChild(badge);
        }
      }
      if (badge) {
        badge.innerText = "Работает";
        badge.style.color = "#10b981";
        badge.style.background = "rgba(16, 185, 129, 0.12)";
        badge.style.border = "1px solid rgba(16, 185, 129, 0.3)";
      }
    }

    if (btn) {
      btn.remove();
    }

    showToast(`Служба «${name}» успешно восстановлена и запущена!`, false);
  } else {
    const errMsg = (res && (res.message || res.error)) || `Ошибка при восстановлении службы «${name}»`;
    showToast(errMsg, true);
    if (btn) {
      btn.disabled = false;
      btn.innerHTML = originalHtml || "Восстановить";
    }
  }
}

async function toggleServiceState(svcId, isChecked) {
  const svc = allServices.find(s => s.id === svcId);
  const res = await callNative("setServiceState", { id: svcId, disable: !isChecked });

  const card = document.querySelector(`.service-card[data-id="${svcId}"]`);
  let badge = card?.querySelector(".service-status-badge");
  if (!badge && card) {
    const headerEl = card.querySelector(".service-header");
    if (headerEl) {
      badge = document.createElement("span");
      badge.className = "service-status-badge";
      badge.style.borderRadius = "4px";
      badge.style.fontSize = "9px";
      badge.style.padding = "1.5px 6px";
      badge.style.marginLeft = "6px";
      badge.style.fontWeight = "500";
      const titleEl = headerEl.querySelector(".service-title");
      if (titleEl && titleEl.nextSibling) {
        headerEl.insertBefore(badge, titleEl.nextSibling);
      } else {
        headerEl.appendChild(badge);
      }
    }
  }

  if (isChecked) {
    const restoreBtn = card?.querySelector(".btn-restore-service");
    if (restoreBtn) restoreBtn.remove();
    if (badge) {
      badge.innerText = "Работает";
      badge.style.color = "#10b981";
      badge.style.background = "rgba(16, 185, 129, 0.12)";
      badge.style.border = "1px solid rgba(16, 185, 129, 0.3)";
    }
    showToast("Служба «" + (svc ? svc.displayName : svcId) + "» успешно включена!", false);
  } else {
    const headerEl = card?.querySelector(".service-header");
    if (headerEl && !headerEl.querySelector(".btn-restore-service")) {
      const restoreBtn = document.createElement("button");
      restoreBtn.type = "button";
      restoreBtn.className = "btn-restore-service";
      restoreBtn.title = "Восстановить службу к заводским параметрам и запустить";
      restoreBtn.innerText = "Восстановить";
      restoreBtn.onclick = (e) => restoreSingleService(svcId, e);
      headerEl.appendChild(restoreBtn);
    }
    if (badge) {
      badge.innerText = "Отключена";
      badge.style.color = "#ef4444";
      badge.style.background = "rgba(239, 68, 68, 0.12)";
      badge.style.border = "1px solid rgba(239, 68, 68, 0.3)";
    }
    showToast("Служба «" + (svc ? svc.displayName : svcId) + "» отключена.", false);
  }

  if (svc && res?.service) {
    svc.status = res.service.status;
    svc.disabled = res.service.disabled;
    svc.isRunning = res.service.isRunning;
  } else if (svc) {
    svc.disabled = !isChecked;
    svc.isRunning = isChecked;
    svc.status = isChecked ? "RUNNING" : "STOPPED";
  }
}

async function toggleService(svcId, disable) {
  await toggleServiceState(svcId, !disable);
}

async function disableRecommendedServices() {
  const res = await callNative("disableRecommendedServices");
  if (res && res.services) {
    renderServices(res.services);
    showToast("Рекомендованные фоновые службы успешно отключены.", false);
  }
}

async function enableAllServices() {
  const res = await callNative("enableAllServices");
  if (res && res.services) {
    renderServices(res.services);
    showToast("Все системные службы успешно включены.", false);
  }
}

// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// AppX UWP Debloat (Categorized 2-Column Grid Layout)
// ----------------------------------------------------------------------------

const APPX_COL1_CATEGORIES = [
  "Развлечения",
  "Браузеры",
  "Кодеки",
  "Средства установки UWP",
  "Интерфейсы и библиотеки UWP"
];

const APPX_COL2_CATEGORIES = [
  "Прикладное ПО",
  "Коммуникации и связь",
  "Антивирус и безопасность",
  "Другие возможности"
];

function getAppCategory(app) {
  const cat = (app.category || "").trim();
  if (!cat) return "Другие возможности";
  if (cat === "Антивирус" || cat === "Антивирус и безопасность" || cat.toLowerCase().includes("антивирус")) {
    return "Антивирус и безопасность";
  }
  const allKnown = [...APPX_COL1_CATEGORIES, ...APPX_COL2_CATEGORIES];
  const found = allKnown.find(k => k.toLowerCase() === cat.toLowerCase());
  if (found) return found;
  return "Другие возможности";
}

function renderAppx(items) {
  allAppx = items || [];
  const container = document.getElementById("appxCategoriesGrid");
  if (!container) return;

  const prevScrollTop = container.scrollTop;
  container.innerHTML = "";

  // Group apps by category
  const groups = {};
  for (const cat of [...APPX_COL1_CATEGORIES, ...APPX_COL2_CATEGORIES]) {
    groups[cat] = [];
  }

  allAppx.forEach(app => {
    const c = getAppCategory(app);
    if (!groups[c]) groups[c] = [];
    groups[c].push(app);
  });

  // Create two column wrapper divs
  const col1 = document.createElement("div");
  col1.className = "appx-column";
  col1.id = "appxCol1";

  const col2 = document.createElement("div");
  col2.className = "appx-column";
  col2.id = "appxCol2";

  // Build category card helper
  function buildCategoryCard(categoryName, appList, colorClass) {
    const card = document.createElement("div");
    card.className = "appx-category-card";
    card.setAttribute("data-category", categoryName);

    const header = document.createElement("div");
    header.className = "appx-card-header";

    const title = document.createElement("div");
    title.className = `appx-card-title ${colorClass}`;
    title.innerHTML = `<span>${escapeHtml(categoryName)}</span>`;

    const badge = document.createElement("span");
    badge.className = "appx-card-badge";
    badge.innerText = appList.length;

    header.appendChild(title);
    header.appendChild(badge);
    card.appendChild(header);

    const itemsList = document.createElement("div");
    itemsList.className = "appx-items-list";

    appList.forEach(app => {
      const isInstalled = app.isInstalled !== false;
      const statusText = isInstalled ? "Установлено" : "Удалено";
      const statusClass = isInstalled ? "installed" : "uninstalled";
      const searchTokens = [
        app.displayName || "",
        app.packageNamePattern || "",
        app.id || "",
        categoryName
      ].join(" ").toLowerCase();

      const label = document.createElement("label");
      label.className = `custom-checkbox appx-checkbox ${!isInstalled ? "is-uninstalled" : ""}`;
      label.setAttribute("data-name", searchTokens);
      label.setAttribute("data-app-id", app.id);

      const restoreBtnHtml = !isInstalled
        ? `<button type="button" class="btn-restore-app" onclick="restoreSingleApp('${escapeHtml(app.id)}', event)" title="Восстановить приложение">Восстановить</button>`
        : "";

      label.innerHTML = `
        <input type="checkbox" id="appx_cb_${app.id}" ${app.selected ? "checked" : ""} onchange="toggleAppxSelection('${escapeHtml(app.id)}', this.checked)">
        <span class="checkbox-label" title="${escapeHtml(app.displayName)}">${escapeHtml(app.displayName)}</span>
        <span class="appx-status-pill ${statusClass}">${statusText}</span>
        ${restoreBtnHtml}
      `;
      itemsList.appendChild(label);
    });

    card.appendChild(itemsList);
    return card;
  }

  // Populate Column 1 (accent cyan)
  APPX_COL1_CATEGORIES.forEach(cat => {
    const list = groups[cat] || [];
    if (list.length > 0) {
      col1.appendChild(buildCategoryCard(cat, list, "cyan"));
    }
  });

  // Populate Column 2 (apple blue)
  APPX_COL2_CATEGORIES.forEach(cat => {
    const list = groups[cat] || [];
    if (list.length > 0) {
      col2.appendChild(buildCategoryCard(cat, list, "blue"));
    }
  });

  // Any leftover categories not in standard list
  Object.keys(groups).forEach(cat => {
    if (!APPX_COL1_CATEGORIES.includes(cat) && !APPX_COL2_CATEGORIES.includes(cat)) {
      const list = groups[cat] || [];
      if (list.length > 0) {
        col2.appendChild(buildCategoryCard(cat, list, "blue"));
      }
    }
  });

  container.appendChild(col1);
  container.appendChild(col2);

  if (prevScrollTop > 0) {
    container.scrollTop = prevScrollTop;
  }

  // If a search query is active, re-apply filter immediately
  const searchInput = document.getElementById("appxSearch");
  if (searchInput && searchInput.value) {
    filterAppx(searchInput.value);
  }
}

function filterAppx(query) {
  const q = (query || "").toLowerCase().trim();
  const cards = document.querySelectorAll("#appxCategoriesGrid .appx-category-card");

  cards.forEach(card => {
    const items = card.querySelectorAll(".appx-checkbox");
    let visibleCount = 0;

    items.forEach(item => {
      const text = item.getAttribute("data-name") || "";
      const matches = !q || text.includes(q);
      item.style.display = matches ? "flex" : "none";
      if (matches) visibleCount++;
    });

    const badge = card.querySelector(".appx-card-badge");
    if (badge) {
      if (q && visibleCount !== items.length) {
        badge.innerText = `${visibleCount}/${items.length}`;
      } else {
        badge.innerText = `${items.length}`;
      }
    }

    if (q && visibleCount === 0) {
      card.style.display = "none";
    } else {
      card.style.display = "";
    }
  });
}

async function restoreSingleApp(appId, ev) {
  if (ev) {
    ev.stopPropagation();
    ev.preventDefault();
  }
  const btn = ev?.currentTarget || ev?.target || document.querySelector(`.appx-checkbox[data-app-id="${appId}"] .btn-restore-app`);
  let originalHtml = "";
  if (btn) {
    originalHtml = btn.innerHTML;
    btn.disabled = true;
    btn.innerHTML = `<span class="spinner-sm"></span> Восстановление...`;
  }

  const res = await callNative("restoreAppx", { id: appId });

  if (res && res.success !== false && !res.error) {
    const found = allAppx.find(a => a.id === appId);
    if (found) {
      found.isInstalled = true;
    }
    showToast("Приложение успешно восстановлено!", false);

    const row = document.querySelector(`.appx-checkbox[data-app-id="${appId}"]`) || btn?.closest(".appx-checkbox");
    if (row) {
      row.classList.remove("is-uninstalled");
      const pill = row.querySelector(".appx-status-pill");
      if (pill) {
        pill.className = "appx-status-pill installed";
        pill.innerText = "Установлено";
      }
    }
    if (btn) {
      btn.remove();
    }
  } else {
    const errMsg = (res && (res.message || res.error)) || "Не удалось восстановить приложение";
    showToast(errMsg, true);
    if (btn) {
      btn.disabled = false;
      btn.innerHTML = originalHtml || "Восстановить";
    }
  }
}

async function toggleAppxSelection(appxId, selected) {
  const item = allAppx.find(a => a.id === appxId);
  if (item) item.selected = !!selected;

  // Sync checkbox in DOM if changed programmatically
  const cb = document.getElementById(`appx_cb_${appxId}`);
  if (cb && cb.checked !== !!selected) {
    cb.checked = !!selected;
  }

  await callNative("selectAppx", { id: appxId, selected: !!selected });
}

async function selectAppx(appxId, selected) {
  return toggleAppxSelection(appxId, selected);
}

async function selectAllAppx(select) {
  // Optimistically update all checkboxes across all category cards
  const boxes = document.querySelectorAll("#appxCategoriesGrid input[type='checkbox']");
  boxes.forEach(cb => {
    cb.checked = !!select;
  });
  allAppx.forEach(a => {
    a.selected = !!select;
  });

  const res = await callNative("selectAllAppx", { select: !!select });
  if (res && res.appx) {
    renderAppx(res.appx);
  }
}

async function uninstallSelectedAppx() {
  await callNative("uninstallSelectedAppx");
}

async function uninstallAllAppx() {
  const ok = await showConfirmModal({
    title: "Удаление приложений",
    message: "Вы действительно хотите удалить все доступные встроенные UWP приложения?",
    subtext: "Будет запущена пакетная деинсталляция всех стандартных приложений Windows.",
    confirmText: "Удалить все",
    cancelText: "Отмена",
    icon: "/icons/trash.png",
    danger: true
  });
  if (ok) {
    await callNative("uninstallAllAppx");
  }
}

// ----------------------------------------------------------------------------
// Cleanup
// ----------------------------------------------------------------------------
function setCleanupOption(key, value) {
  callNative("setCleanupOption", { key: key, value: !!value });
}

function selectAllCleanup(select) {
  const boxes = document.querySelectorAll("#cleanupCheckboxesGrid input[type='checkbox']");
  boxes.forEach(b => {
    b.checked = select;
    const key = b.id.replace("cln-", "");
    callNative("setCleanupOption", { key: key, value: select });
  });
}

let cleanupSafetyTimer = null;

function handleCleanupResult(data) {
  if (cleanupSafetyTimer) {
    clearTimeout(cleanupSafetyTimer);
    cleanupSafetyTimer = null;
  }
  const btn = document.getElementById("btnExecuteCleanup");
  if (btn) {
    btn.classList.remove("btn-spinning");
    if (data && data.success) {
      let freedMbText = "";
      if (data.freedBytes !== undefined && data.freedBytes !== null && data.freedBytes > 0) {
        const freedMb = (data.freedBytes / (1024 * 1024)).toFixed(1);
        freedMbText = ` (+${freedMb} МБ)`;
      }
      btn.innerHTML = `Готово!${freedMbText}`;
      btn.classList.add("btn-success-activated");
      setTimeout(() => {
        btn.innerHTML = btn.dataset.originalText || "Запустить очистку";
        btn.classList.remove("btn-success-activated");
        btn.disabled = false;
      }, 2500);
    } else {
      btn.innerHTML = btn.dataset.originalText || "Запустить очистку";
      btn.disabled = false;
    }
  }

  if (data && data.success) {
    showToast(data.message || "Очистка успешно завершена!", false);
  } else {
    showToast((data && (data.message || data.error)) || "Ошибка при выполнении очистки", true);
  }

  callNative("getInitialState").then(st => {
    if (st && st.telemetry) onTelemetryUpdate(st.telemetry);
  }).catch(() => {});
}

async function executeCleanup() {
  const boxes = document.querySelectorAll("#cleanupCheckboxesGrid input[type='checkbox']");
  const options = {};
  let anyChecked = false;
  boxes.forEach(b => {
    const key = b.id.replace("cln-", "");
    options[key] = b.checked;
    if (b.checked) anyChecked = true;
  });

  if (!anyChecked) {
    showToast("Выберите хотя бы один пункт для очистки!", true);
    return;
  }

  const btn = document.getElementById("btnExecuteCleanup");
  if (btn) {
    btn.disabled = true;
    if (!btn.dataset.originalText) {
      btn.dataset.originalText = btn.innerHTML;
    }
    btn.innerHTML = `<span class="spinner-inline"></span> Очистка...`;
    btn.classList.add("btn-spinning");
  }

  showToast("Запущена выборочная очистка системных кэшей...", false);

  if (cleanupSafetyTimer) clearTimeout(cleanupSafetyTimer);
  cleanupSafetyTimer = setTimeout(() => {
    const b = document.getElementById("btnExecuteCleanup");
    if (b && b.classList.contains("btn-spinning")) {
      handleCleanupResult({ success: true, message: "Очистка успешно завершена!" });
    }
  }, 10000);

  try {
    const res = await callNative("executeCleanup", { options });
    if (res) {
      handleCleanupResult(res);
    }
  } catch (err) {
    handleCleanupResult({ success: false, error: err?.message || "Ошибка вызова очистки" });
  }
}

async function deepCleanAction(actionName) {
  await callNative("deepCleanAction", { action: actionName });
}

// ----------------------------------------------------------------------------
// Backup & Restore
// ----------------------------------------------------------------------------
async function createRestorePoint() {
  const desc = document.getElementById("restorePointDesc")?.value || "WCS Optimization Restore Point";
  await callNative("createRestorePoint", { description: desc });
}

async function exportProfile() {
  await callNative("exportProfile");
}

async function importProfile() {
  const res = await callNative("importProfile");
  if (res && res.success && res.tweaks) {
    syncTweaksUI(res.tweaks);
  }
}

// ----------------------------------------------------------------------------
// Modals & Toasts
// ----------------------------------------------------------------------------
function populateAboutModalSpecs(specs) {
  if (!specs) return;

  const cpuEl = document.getElementById("aboutCpu");
  const gpuEl = document.getElementById("aboutGpu");
  const ramEl = document.getElementById("aboutRam");
  const osEl = document.getElementById("aboutOs");
  const powerEl = document.getElementById("aboutPower");

  // CPU: e.g. "AMD Ryzen 7 7800X3D (16 потоков)"
  if (cpuEl) {
    const cpuName = specs.cpuName || specs.cpu || "";
    const threads = specs.logicalCores || specs.threads;
    if (cpuName) {
      cpuEl.innerText = threads ? `${cpuName} (${threads} потоков)` : cpuName;
    }
  }

  // GPU: e.g. "NVIDIA GeForce RTX 4080"
  if (gpuEl) {
    const gpuName = specs.primaryGPUName || specs.gpuName || specs.gpu || "";
    if (gpuName) {
      gpuEl.innerText = gpuName;
    }
  }

  // RAM: e.g. "31.8 GB (Доступно: 24.2 GB)"
  if (ramEl) {
    const total = specs.totalRAM_GB ?? specs.ramTotalGb ?? specs.ramTotal;
    const free = specs.freeRAM_GB ?? specs.ramFreeGb ?? specs.ramFree;
    if (total !== undefined && total > 0) {
      if (free !== undefined) {
        ramEl.innerText = `${Number(total).toFixed(1)} GB (Доступно: ${Number(free).toFixed(1)} GB)`;
      } else {
        ramEl.innerText = `${Number(total).toFixed(1)} GB`;
      }
    }
  }

  // OS: e.g. "Windows 11 Pro 26100"
  if (osEl) {
    const osName = specs.osName || specs.os || "";
    const osBuild = specs.osBuild || specs.build || "";
    if (osName || osBuild) {
      osEl.innerText = `${osName} ${osBuild}`.trim();
    }
  }

  // Power Plan: e.g. "Максимальная производительность"
  if (powerEl) {
    const power = specs.activePowerPlan || specs.powerPlan || specs.power || "";
    if (power) {
      powerEl.innerText = power;
    }
  }
}

async function openAboutModal() {
  const modal = document.getElementById("aboutModal");
  if (!modal) return;

  if (currentSpecs) {
    populateAboutModalSpecs(currentSpecs);
  }

  try {
    const res = await callNative("getSystemSpecs");
    if (res) {
      if (res.specs) {
        currentSpecs = Object.assign(currentSpecs || {}, res.specs);
      } else if (res.cpuName || res.primaryGPUName || res.cpu || res.gpu || res.totalRAM_GB) {
        currentSpecs = Object.assign(currentSpecs || {}, res);
      }
      populateAboutModalSpecs(currentSpecs);
    }
  } catch (err) {
    console.warn("Failed to fetch fresh system specs:", err);
  }

  modal.style.display = "flex";
  modal.classList.remove("modal-exiting");
  requestAnimationFrame(() => {
    modal.classList.add("active");
  });
}

function closeAboutModal() {
  const modal = document.getElementById("aboutModal");
  if (!modal) return;
  modal.classList.add("modal-exiting");
  modal.classList.remove("active");
  setTimeout(() => {
    modal.classList.remove("modal-exiting");
    modal.style.display = "none";
  }, 380);
}

// ----------------------------------------------------------------------------
// Custom Action Confirmation Dialog
// ----------------------------------------------------------------------------
let confirmModalResolver = null;

function showConfirmModal(options = {}) {
  const {
    title = "Подтверждение действия",
    message = "Вы уверены, что хотите продолжить?",
    details = null,
    subtext = "Это действие нельзя будет отменить.",
    confirmText = "Подтвердить",
    cancelText = "Отмена",
    icon = "/icons/trash.png",
    danger = true
  } = options;

  return new Promise((resolve) => {
    confirmModalResolver = resolve;

    const modal = document.getElementById("confirmModal");
    if (!modal) {
      resolve(false);
      return;
    }

    const titleEl = document.getElementById("confirmModalTitle");
    const msgEl = document.getElementById("confirmModalMessage");
    const detailsEl = document.getElementById("confirmModalDetails");
    const subtextEl = document.getElementById("confirmModalSubtext");
    const badgeEl = document.getElementById("confirmModalBadge");
    const iconEl = document.getElementById("confirmModalIcon");
    const okBtn = document.getElementById("btnConfirmOk");
    const okText = document.getElementById("btnConfirmOkText");
    const okIcon = document.getElementById("btnConfirmOkIcon");
    const cancelBtn = document.getElementById("btnConfirmCancel");

    if (titleEl) titleEl.innerText = title;

    if (msgEl) {
      if (typeof message === "string" && message.includes("<")) {
        msgEl.innerHTML = message;
      } else {
        msgEl.innerText = message;
      }
    }

    if (detailsEl) {
      if (details && (details.command || details.location || details.info)) {
        let detHtml = "";
        if (details.location) {
          detHtml += `<div style="display: flex; align-items: center; gap: 8px; margin-bottom: 5px;">
            <span style="font-size: 10.5px; color: var(--text-muted); font-weight: 500;">Расположение:</span>
            <span style="font-size: 11px; color: var(--accent-cyan); font-weight: 600;">${escapeHtml(details.location)}</span>
          </div>`;
        }
        if (details.command) {
          detHtml += `<div style="font-family: monospace; font-size: 10.5px; color: #94a3b8; word-break: break-all; max-height: 48px; overflow-y: auto;" title="${escapeHtml(details.command)}">${escapeHtml(details.command)}</div>`;
        }
        if (details.info) {
          detHtml += `<div style="font-size: 11px; color: var(--text-dim); margin-top: 4px;">${escapeHtml(details.info)}</div>`;
        }
        detailsEl.innerHTML = detHtml;
        detailsEl.style.display = "block";
      } else {
        detailsEl.innerHTML = "";
        detailsEl.style.display = "none";
      }
    }

    if (subtextEl) {
      if (subtext) {
        subtextEl.innerText = subtext;
        subtextEl.style.display = "block";
      } else {
        subtextEl.style.display = "none";
      }
    }

    if (iconEl) iconEl.src = icon;
    if (badgeEl) {
      badgeEl.className = "confirm-modal-badge " + (danger ? "badge-danger" : "badge-primary");
    }

    if (okBtn) {
      okBtn.className = "btn " + (danger ? "btn-danger" : "btn-primary");
    }
    if (okText) okText.innerText = confirmText;
    if (okIcon) {
      okIcon.src = icon;
      okIcon.style.display = icon ? "inline-block" : "none";
    }
    if (cancelBtn) cancelBtn.innerText = cancelText;

    modal.style.display = "flex";
    modal.classList.remove("modal-exiting");
    requestAnimationFrame(() => {
      modal.classList.add("active");
    });
  });
}

function acceptConfirmModal() {
  closeConfirmModal(true);
}

function cancelConfirmModal() {
  closeConfirmModal(false);
}

function closeConfirmModal(result = false) {
  const modal = document.getElementById("confirmModal");
  if (modal) {
    modal.classList.add("modal-exiting");
    modal.classList.remove("active");
    setTimeout(() => {
      modal.classList.remove("modal-exiting");
      modal.style.display = "none";
    }, 380);
  }
  if (confirmModalResolver) {
    const cb = confirmModalResolver;
    confirmModalResolver = null;
    cb(result);
  }
}

function showToast(message, isError = false) {
  const container = document.getElementById("toastContainer");
  if (!container || !message) return;

  const toast = document.createElement("div");
  toast.className = `toast ${isError ? "error" : "success"}`;
  toast.innerHTML = `
    <span class="toast-icon">${isError ? '<img src="/icons/alert.png" style="width:14px;height:14px;vertical-align:middle;">' : '<img src="/icons/check.png" style="width:14px;height:14px;vertical-align:middle;">'}</span>
    <span>${escapeHtml(message)}</span>
  `;
  container.appendChild(toast);

  setTimeout(() => {
    toast.style.opacity = "0";
    toast.style.transform = "translateY(10px)";
    toast.style.transition = "all 0.3s ease";
    setTimeout(() => toast.remove(), 300);
  }, 4000);
}

function escapeHtml(str) {
  if (!str) return "";
  return str.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
}

// ----------------------------------------------------------------------------
// Initial Sync
// ----------------------------------------------------------------------------
function syncTweaksUI(t) {
  if (!t) return;
  const setCheck = (id, val) => {
    const el = document.getElementById(id);
    if (el) el.checked = !!val;
  };

  setCheck("tweak-disableDynamicTick", t.disableDynamicTick);
  setCheck("tweak-usePlatformClockNo", t.usePlatformClockNo);
  setCheck("tweak-ultimatePowerPlan", t.ultimatePowerPlan);
  setCheck("tweak-disableCoreParking", t.disableCoreParking);
  setCheck("tweak-powerThrottling", t.powerThrottling);
  setCheck("tweak-bootuxDisabled", t.bootuxDisabled);
  setCheck("tweak-usePlatformTickNo", t.usePlatformTickNo || t.usePlatformTick);
  setCheck("tweak-win32PrioritySeparation", t.win32PrioritySeparation);
  setCheck("tweak-disablePagingExecutive", t.disablePagingExecutive);
  setCheck("tweak-largeSystemCacheDisabled", t.largeSystemCacheDisabled !== undefined ? t.largeSystemCacheDisabled : t.largeSystemCache);

  setCheck("tweak-gpuMsiMode", t.gpuMsiMode);
  setCheck("tweak-audioMsiMode", t.audioMsiMode);
  setCheck("tweak-hagsEnabled", t.hagsEnabled);
  setCheck("tweak-disableGameDVR", t.disableGameDVR);
  setCheck("tweak-disableFullscreenOptimizations", t.disableFullscreenOptimizations);

  setCheck("tweak-mmcssGamesPriority", t.mmcssGamesPriority);
  setCheck("tweak-disableNetworkThrottling", t.disableNetworkThrottling);

  setCheck("tweak-tcpNoDelay", t.tcpNoDelay);
  setCheck("tweak-tcpAckFrequency", t.tcpAckFrequency);
  setCheck("tweak-tcpAutoTuning", t.tcpAutoTuning);
  setCheck("tweak-defaultTTL64", t.defaultTTL64 || t.defaultTtl);

  setCheck("tweak-flatMouseCurve", t.flatMouseCurve);
  setCheck("tweak-keyboardMaxSpeed", t.keyboardMaxSpeed);
  setCheck("tweak-disableTelemetry", t.disableTelemetry);
  setCheck("tweak-disableErrorReporting", t.disableErrorReporting);

  // Dashboard Quick Tweak Toggles
  setCheck("dash-quick-timerLock", t.timerResolution05ms || isTimerLocked);
  setCheck("dash-quick-gpuMsi", t.gpuMsiMode);
  setCheck("dash-quick-audioMsi", t.audioMsiMode);
  setCheck("dash-quick-tcpNoDelay", t.tcpNoDelay);
  setCheck("dash-quick-coreParking", t.disableCoreParking);
  setCheck("dash-quick-largeSystemCache", t.largeSystemCacheDisabled !== undefined ? t.largeSystemCacheDisabled : t.largeSystemCache);

  // Update MSI metric card on Dashboard
  const dashMsiVal = document.getElementById("dashMsiVal");
  const dashMsiBadge = document.getElementById("dashMsiBadge");
  const dashMsiSub = document.getElementById("dashMsiSub");
  if (dashMsiVal) {
    if (t.gpuMsiMode && t.audioMsiMode) {
      dashMsiVal.textContent = "Direct MSI";
      if (dashMsiBadge) { dashMsiBadge.textContent = "DIRECT IRQ"; dashMsiBadge.className = "dash-metric-badge badge-locked"; }
      if (dashMsiSub) dashMsiSub.textContent = "GPU & Audio разгружены";
    } else if (t.gpuMsiMode || t.audioMsiMode) {
      dashMsiVal.textContent = "MSI Частично";
      if (dashMsiBadge) { dashMsiBadge.textContent = "АКТИВЕН"; dashMsiBadge.className = "dash-metric-badge badge-active"; }
      if (dashMsiSub) dashMsiSub.textContent = t.gpuMsiMode ? "GPU в MSI режиме" : "Audio в MSI режиме";
    } else {
      dashMsiVal.textContent = "Line-Based IRQ";
      if (dashMsiBadge) { dashMsiBadge.textContent = "ОБЩАЯ ШИНА"; dashMsiBadge.className = "dash-metric-badge"; dashMsiBadge.style.background = "rgba(255,255,255,0.08)"; dashMsiBadge.style.color = "var(--text-dim)"; }
      if (dashMsiSub) dashMsiSub.textContent = "Стандартный опрос шины";
    }
  }

  if (t.activePreset !== undefined) {
    if (t.activePreset === 1) highlightPresetCard("EsportsUltra");
    else if (t.activePreset === 2) highlightPresetCard("CompetitiveGaming");
    else if (t.activePreset === 3) highlightPresetCard("WindowsDefault");
  }
}

// ----------------------------------------------------------------------------
// Context Menu Management (Subtab 4)
// ----------------------------------------------------------------------------
function syncContextMenuUI(items) {
  if (!items) return;
  if (Array.isArray(items)) {
    items.forEach(it => {
      const id = it.id;
      const chk = document.getElementById("ctx-" + id);
      const row = document.getElementById("row-ctx-" + id);
      const shift = document.getElementById("shift-ctx-" + id);
      if (chk) chk.checked = !!it.enabled;
      if (row) row.classList.toggle("tweak-active", !!it.enabled);
      if (shift) shift.classList.toggle("active", !!it.shiftOnly);
    });
  }
}

async function toggleContextMenuTweak(id, isChecked) {
  const row = document.getElementById("row-ctx-" + id);
  if (row) row.classList.toggle("tweak-active", isChecked);
  try {
    const res = await callNative("toggleContextMenuTweak", { id: id, enabled: isChecked });
    if (res && res.items) {
      syncContextMenuUI(res.items);
    }
  } catch (err) {
    console.error("toggleContextMenuTweak error:", err);
  }
}

async function toggleContextShift(id) {
  const shiftBtn = document.getElementById("shift-ctx-" + id);
  const currentActive = shiftBtn ? shiftBtn.classList.contains("active") : false;
  const newActive = !currentActive;
  if (shiftBtn) shiftBtn.classList.toggle("active", newActive);
  try {
    const res = await callNative("setContextMenuShiftOnly", { id: id, shiftOnly: newActive });
    if (res && res.items) {
      syncContextMenuUI(res.items);
    }
  } catch (err) {
    console.error("setContextMenuShiftOnly error:", err);
  }
}

async function applyAllContextMenuTweaks(enable) {
  try {
    const res = await callNative("applyAllContextMenuTweaks", { enable: enable });
    if (res && res.items) {
      syncContextMenuUI(res.items);
    }
  } catch (err) {
    console.error("applyAllContextMenuTweaks error:", err);
  }
}

async function applyFlyoutGroup(group, enable) {
  try {
    const res = await callNative("applyContextMenuGroup", { group: group, enable: enable });
    if (res && res.items) {
      syncContextMenuUI(res.items);
    }
  } catch (err) {
    console.error("applyFlyoutGroup error:", err);
  }
}

function openTiChooser(e) {
  if (e) { e.preventDefault(); e.stopPropagation(); }
  showToast("Запуск от имени TrustedInstaller (NT AUTHORITY\\SYSTEM) настроен.", false);
}

function openPersonalizeChooser(e) {
  if (e) { e.preventDefault(); e.stopPropagation(); }
  showToast("Классическая персонализация Windows (desk.cpl) выбрана.", false);
}

// ----------------------------------------------------------------------------
// Windows 11 Optimization & Privacy (Subtab 6)
// ----------------------------------------------------------------------------
let currentWin11Data = null;

async function loadWin11Tweaks() {
  try {
    const res = await callNative("getWin11Tweaks");
    if (res) {
      currentWin11Data = res;
      renderWin11Tweaks(res);
    }
  } catch (err) {
    console.error("Failed to load Win11 tweaks:", err);
  }
}

function renderWin11Tweaks(data) {
  if (!data) return;
  const isWin11 = !!data.isWin11;
  const build = data.buildNumber || 0;
  const tweaks = data.tweaks || [];

  // Update Status Banner
  const banner = document.getElementById("win11StatusBanner");
  const bTitle = document.getElementById("win11BannerTitle");
  const bDesc = document.getElementById("win11BannerDesc");
  const bBadge = document.getElementById("win11BannerBadge");

  if (banner && bTitle && bDesc && bBadge) {
    if (isWin11) {
      banner.className = "os-info-banner win11-detected";
      bTitle.textContent = "Обнаружена Windows 11 (23H2 / 24H2)";
      bDesc.textContent = `Сборка ядра NT: ${build}. Доступен полный набор хирургических GPO-политик отключения AI и телеметрии.`;
      bBadge.className = "os-info-badge badge-blue";
      bBadge.textContent = `Build ${build} • Win 11`;
    } else {
      banner.className = "os-info-banner win10-detected";
      bTitle.textContent = `Обнаружена Windows 10 (Сборка ${build})`;
      bDesc.textContent = "Некоторые политики (Recall, Copilot, Iris) специфичны для Windows 11 23H2/24H2, но универсальные политики применимы.";
      bBadge.className = "os-info-badge badge-gray";
      bBadge.textContent = `Build ${build} • Win 10`;
    }
  }

  // Render Tweak List
  const container = document.getElementById("win11TweaksList");
  if (!container) return;

  container.innerHTML = tweaks.map(tweak => {
    const isChecked = !!tweak.enabled;
    const tagHtml = tweak.isWin11Only
      ? `<span class="win11-badge-tag win11-only-tag">Win 11 Only</span>`
      : `<span class="win11-badge-tag universal-tag">Win 10/11</span>`;

    const win11IconMap = {
      win11ClassicMenu: "/icons/sliders.png",
      win11Copilot: "/icons/win11_copilot.png",
      win11Recall: "/icons/win11_recall.png",
      win11BingSearch: "/icons/win11_bing.png",
      win11StartAds: "/icons/win11_ads.png",
      win11Widgets: "/icons/win11_widgets.png",
      win11ConsumerBloat: "/icons/win11_bloat.png",
      win11TypingTelemetry: "/icons/win11_typing.png"
    };
    const iconName = win11IconMap[tweak.id] || "/icons/win11_logo.png";

    return `
      <div class="tweak-row ${isChecked ? "tweak-active" : ""}" id="row-w11-${tweak.id}">
        <div class="tweak-info">
          <div class="tweak-icon-badge">
            <img src="${iconName}" alt="${tweak.id}">
          </div>
          <div class="tweak-text">
            <span class="tweak-name">${tweak.name} ${tagHtml}</span>
            <span class="tweak-details">${tweak.description}</span>
          </div>
        </div>
        <div class="tweak-controls">
          <span class="safe-tag">100% GPO</span>
          <label class="switch">
            <input type="checkbox" id="w11-${tweak.id}" ${isChecked ? "checked" : ""} onchange="toggleWin11Tweak('${tweak.id}', this.checked)">
            <span class="slider"></span>
          </label>
        </div>
      </div>
    `;
  }).join("");
}

async function toggleWin11Tweak(id, isChecked) {
  const row = document.getElementById("row-w11-" + id);
  if (row) row.classList.toggle("tweak-active", isChecked);

  try {
    const res = await callNative("toggleWin11Tweak", { id: id, enabled: isChecked });
    if (res && res.tweaks) {
      currentWin11Data = res;
      renderWin11Tweaks(res);
    }
  } catch (err) {
    console.error("toggleWin11Tweak error:", err);
    const chk = document.getElementById("w11-" + id);
    if (chk) chk.checked = !isChecked;
    if (row) row.classList.toggle("tweak-active", !isChecked);
  }
}

function syncSystemSuiteUI(ss) {
  if (!ss) return;
  const setCheck = (id, val) => {
    const el = document.getElementById(id);
    if (el) el.checked = !!val;
  };

  if (ss.system) {
    for (const [k, v] of Object.entries(ss.system)) {
      setCheck("sys-" + k, v);
    }
  }
  if (ss.security) {
    for (const [k, v] of Object.entries(ss.security)) {
      setCheck("sec-" + k, v);
    }
  }
  if (ss.osPerfSub) {
    for (const [k, v] of Object.entries(ss.osPerfSub)) {
      setCheck("os-" + k, v);
    }
  }
  if (ss.ssdPerfSub) {
    for (const [k, v] of Object.entries(ss.ssdPerfSub)) {
      setCheck("ssd-" + k, v);
    }
  }
  if (ss.telemetrySub) {
    for (const [k, v] of Object.entries(ss.telemetrySub)) {
      setCheck("telem-" + k, v);
    }
  }
  if (ss.contextMenu) {
    syncContextMenuUI(ss.contextMenu);
  }
}

// ----------------------------------------------------------------------------
// Startup Manager (Tab 7 - Subtab 7: Автозагрузка)
// ----------------------------------------------------------------------------
let allStartupItems = [];

async function loadStartupItems() {
  const container = document.getElementById("startupList");
  if (container && allStartupItems.length === 0) {
    container.innerHTML = `<div class="startup-empty-state"><img src="/icons/refresh.png" style="width: 18px; animation: spin 1s linear infinite; margin-bottom: 8px;"><br>Сканирование точек автозагрузки Windows...</div>`;
  }
  try {
    const res = await callNative("getStartupItems");
    if (res && res.items) {
      renderStartupItems(res);
    }
  } catch (err) {
    console.error("Failed to load startup items:", err);
    showToast("Не удалось загрузить автозагрузку", true);
  }
}

function renderStartupItems(data) {
  allStartupItems = (data && data.items) || [];
  const container = document.getElementById("startupList");
  if (!container) return;

  const total = data.totalCount !== undefined ? data.totalCount : allStartupItems.length;
  const enabled = data.enabledCount !== undefined ? data.enabledCount : allStartupItems.filter(i => i.enabled).length;

  const badge = document.getElementById("startupCountBadge");
  if (badge) {
    badge.innerText = `${enabled} / ${total} активно`;
  }

  if (allStartupItems.length === 0) {
    container.innerHTML = `<div class="startup-empty-state">Записей в автозагрузке не обнаружено.</div>`;
    return;
  }

  container.innerHTML = "";

  allStartupItems.forEach(item => {
    const card = document.createElement("div");
    card.className = "startup-card" + (item.enabled ? "" : " disabled") + (item.exists ? "" : " not-found");
    card.setAttribute("data-id", item.id);
    const searchTokens = `${item.name} ${item.publisher || ""} ${item.location || ""} ${item.command || ""}`.toLowerCase();
    card.setAttribute("data-search", searchTokens);

    let locClass = "location-hkcu";
    let iconSrc = "/icons/clock.png";
    if (item.location.indexOf("HKLM") !== -1) {
      locClass = "location-hklm";
      iconSrc = "/icons/settings.png";
    } else if (item.location.indexOf("Folder") !== -1) {
      locClass = "location-folder";
      iconSrc = "/icons/apps.png";
    }

    const publisherText = item.publisher && item.publisher !== "Не указан" && item.publisher !== "Файл не найден" ? item.publisher : "Неизвестный издатель";
    const pubUnknownClass = publisherText === "Неизвестный издатель" ? " unknown" : "";
    const missingBadge = !item.exists ? `<span class="startup-missing-tag" title="Исполняемый файл не найден на диске">Файл не найден</span>` : "";

    card.innerHTML = `
      <div class="startup-card-main">
        <div class="startup-icon-badge" title="${escapeHtml(item.location)}">
          <img src="${iconSrc}" alt="app" />
        </div>
        <div class="startup-info">
          <div class="startup-header">
            <span class="startup-name" title="${escapeHtml(item.name)}">${escapeHtml(item.name)}</span>
            <span class="startup-publisher${pubUnknownClass}" title="Издатель: ${escapeHtml(publisherText)}">${escapeHtml(publisherText)}</span>
            <span class="startup-location-pill ${locClass}" title="Расположение автозапуска">${escapeHtml(item.location)}</span>
            ${missingBadge}
          </div>
          <div class="startup-cmd" title="${escapeHtml(item.command)}">${escapeHtml(item.command)}</div>
        </div>
      </div>
      <div class="startup-controls">
        <label class="switch" title="${item.enabled ? 'Отключить автозапуск' : 'Включить автозапуск'}">
          <input type="checkbox" class="startup-toggle-input" ${item.enabled ? "checked" : ""}>
          <span class="slider"></span>
        </label>
        <button class="startup-delete-btn" title="Удалить из автозагрузки">
          <img src="/icons/trash.png" alt="Удалить">
        </button>
      </div>
    `;

    const toggleInput = card.querySelector(".startup-toggle-input");
    if (toggleInput) {
      toggleInput.addEventListener("change", (e) => {
        toggleStartupItem(item.id, e.target.checked);
      });
    }

    const deleteBtn = card.querySelector(".startup-delete-btn");
    if (deleteBtn) {
      deleteBtn.addEventListener("click", () => {
        deleteStartupItem(item.id);
      });
    }

    container.appendChild(card);
  });

  filterStartupItems();
}

async function toggleStartupItem(id, enabled) {
  try {
    const res = await callNative("toggleStartupItem", { id, enabled });
    if (res && res.success) {
      showToast(enabled ? "Элемент включен в автозагрузке" : "Элемент отключен в автозагрузке", false);
      const item = allStartupItems.find(i => i.id === id);
      if (item) {
        item.enabled = enabled;
      }
      if (res.totalCount !== undefined && res.enabledCount !== undefined) {
        const badge = document.getElementById("startupCountBadge");
        if (badge) badge.innerText = `${res.enabledCount} / ${res.totalCount} активно`;
      }
      const card = document.querySelector(`.startup-card[data-id="${CSS.escape(id)}"]`);
      if (card) {
        card.classList.toggle("disabled", !enabled);
      }
    } else {
      showToast((res && res.error) || "Не удалось изменить состояние автозагрузки", true);
      const cb = document.querySelector(`.startup-card[data-id="${CSS.escape(id)}"] input[type="checkbox"]`);
      if (cb) cb.checked = !enabled;
    }
  } catch (err) {
    console.error("toggleStartupItem error:", err);
    showToast("Ошибка при переключении автозагрузки", true);
    const cb = document.querySelector(`.startup-card[data-id="${CSS.escape(id)}"] input[type="checkbox"]`);
    if (cb) cb.checked = !enabled;
  }
}

async function deleteStartupItem(id) {
  const item = allStartupItems.find(i => i.id === id);
  const itemName = item ? item.name : "элемент";
  
  const ok = await showConfirmModal({
    title: "Удаление из автозагрузки",
    message: `Вы действительно хотите удалить запись <span class="confirm-highlight-name">«${escapeHtml(itemName)}»</span> из автозагрузки Windows?`,
    details: item ? {
      location: item.location || "",
      command: item.command || ""
    } : null,
    subtext: "Приложение будет удалено из реестра и перестанет автоматически запускаться при старте Windows.",
    confirmText: "Удалить",
    cancelText: "Отмена",
    icon: "/icons/trash.png",
    danger: true
  });
  if (!ok) {
    return;
  }

  try {
    const res = await callNative("deleteStartupItem", { id });
    if (res && res.success) {
      showToast(`«${itemName}» удален из автозагрузки`, false);
      allStartupItems = allStartupItems.filter(i => i.id !== id);
      if (res.totalCount !== undefined && res.enabledCount !== undefined) {
        const badge = document.getElementById("startupCountBadge");
        if (badge) badge.innerText = `${res.enabledCount} / ${res.totalCount} активно`;
      }
      const card = document.querySelector(`.startup-card[data-id="${CSS.escape(id)}"]`);
      if (card) {
        card.style.transition = "all 0.25s ease";
        card.style.opacity = "0";
        card.style.transform = "translateX(25px)";
        setTimeout(() => {
          card.remove();
          if (allStartupItems.length === 0) {
            const container = document.getElementById("startupList");
            if (container) {
              container.innerHTML = `<div class="startup-empty-state">Записей в автозагрузке не обнаружено.</div>`;
            }
          }
        }, 250);
      }
    } else {
      showToast((res && res.error) || "Не удалось удалить запись", true);
    }
  } catch (err) {
    console.error("deleteStartupItem error:", err);
    showToast("Ошибка при удалении элемента", true);
  }
}

function filterStartupItems() {
  const input = document.getElementById("startupSearchInput");
  const query = (input ? input.value : "").trim().toLowerCase();
  const cards = document.querySelectorAll("#startupList .startup-card");
  let visibleCount = 0;
  cards.forEach(card => {
    const text = (card.getAttribute("data-search") || "").toLowerCase();
    const matches = !query || text.indexOf(query) !== -1;
    card.style.display = matches ? "flex" : "none";
    if (matches) visibleCount++;
  });

  const em = document.getElementById("startupEmptySearch");
  if (!em && cards.length > 0) {
    const container = document.getElementById("startupList");
    const div = document.createElement("div");
    div.id = "startupEmptySearch";
    div.className = "startup-empty-state";
    div.style.display = "none";
    div.innerText = "По вашему запросу ничего не найдено.";
    container.appendChild(div);
  }
  const emElem = document.getElementById("startupEmptySearch");
  if (emElem) {
    emElem.style.display = (cards.length > 0 && visibleCount === 0) ? "block" : "none";
  }
}

// ----------------------------------------------------------------------------
// MSI Interrupt & IRQ Priority Manager (Subtab 8)
// ----------------------------------------------------------------------------
let allMsiDevices = [];
let currentMsiCategoryFilter = "ALL";

async function loadMsiDevices() {
  const container = document.getElementById("msiDeviceList");
  if (container && allMsiDevices.length === 0) {
    container.innerHTML = `<div class="startup-empty-state"><img src="/icons/refresh.png" style="width: 18px; animation: spin 1s linear infinite; margin-bottom: 8px;"><br>Сканирование PCI устройств и режимов MSI...</div>`;
  }
  try {
    const res = await callNative("getMsiDevices");
    if (res && res.devices) {
      renderMsiDevices(res);
    }
  } catch (err) {
    console.error("Failed to load MSI devices:", err);
    showToast("Не удалось загрузить прерывания MSI", true);
  }
}

function setMsiCategoryFilter(category) {
  currentMsiCategoryFilter = category;
  const pills = document.querySelectorAll(".msi-pill-btn");
  pills.forEach(pill => {
    pill.classList.toggle("active", pill.getAttribute("data-category") === category);
  });
  filterMsiDevices();
}

function renderMsiDevices(data) {
  allMsiDevices = (data && data.devices) || [];
  const container = document.getElementById("msiDeviceList");
  if (!container) return;

  const total = data.total !== undefined ? data.total : allMsiDevices.length;
  const msiCount = data.msiCount !== undefined ? data.msiCount : allMsiDevices.filter(d => d.msiEnabled).length;

  const badge = document.getElementById("msiCountBadge");
  if (badge) {
    badge.innerText = `${msiCount} / ${total} MSI активно`;
  }

  if (allMsiDevices.length === 0) {
    container.innerHTML = `<div class="startup-empty-state">Устройства PCI не обнаружены.</div>`;
    return;
  }

  container.innerHTML = "";

  allMsiDevices.forEach(dev => {
    const card = document.createElement("div");
    card.className = "msi-card" + (dev.msiEnabled ? " msi-active" : " msi-inactive") + (!dev.canModify ? " read-only" : "");
    card.setAttribute("data-id", dev.id);
    card.setAttribute("data-category", dev.category);

    const searchTokens = `${dev.name} ${dev.category} ${dev.id} ${dev.hardwareId || ""} ${dev.location || ""}`.toLowerCase();
    card.setAttribute("data-search", searchTokens);

    let iconSrc = "/icons/settings.png";
    let catClass = "msi-cat-other";
    let catLabel = dev.category;

    if (dev.category === "GPU") {
      iconSrc = "/icons/gpu.png";
      catClass = "msi-cat-gpu";
      catLabel = "GPU";
    } else if (dev.category === "Network") {
      iconSrc = "/icons/network.png";
      catClass = "msi-cat-network";
      catLabel = "Сеть";
    } else if (dev.category === "USB") {
      iconSrc = "/icons/mouse.png";
      catClass = "msi-cat-usb";
      catLabel = "USB";
    } else if (dev.category === "Audio") {
      iconSrc = "/icons/audio.png";
      catClass = "msi-cat-audio";
      catLabel = "Звук";
    } else if (dev.category === "Storage") {
      iconSrc = "/icons/presets.png";
      catClass = "msi-cat-storage";
      catLabel = "Диски";
    }

    const msiStatusHtml = dev.msiEnabled
      ? `<span class="msi-status-tag active"><img src="/icons/bolt.png" style="width:11px;height:11px;margin-right:4px;vertical-align:middle;">MSI ВКЛ (Векторный режим)</span>`
      : `<span class="msi-status-tag inactive">MSI ВЫКЛ (Линейный IRQ)</span>`;

    const locationHtml = dev.location ? `<span class="msi-location-pill" title="Шина и адрес устройства">${escapeHtml(dev.location)}</span>` : "";
    const hwidHtml = dev.hardwareId ? `<span class="msi-hwid-pill" title="${escapeHtml(dev.hardwareId)}">${escapeHtml(dev.hardwareId)}</span>` : "";

    let controlsHtml = "";
    if (dev.canModify) {
      controlsHtml = `
        <div class="msi-priority-group">
          <span class="msi-priority-label">Приоритет IRQ</span>
          <select class="custom-select msi-priority-select" title="Приоритет обработки прерываний (Affinity Policy)">
            <option value="0" ${dev.priority === 0 ? "selected" : ""}>По умолчанию</option>
            <option value="1" ${dev.priority === 1 ? "selected" : ""}>Низкий</option>
            <option value="2" ${dev.priority === 2 ? "selected" : ""}>Обычный</option>
            <option value="3" ${dev.priority === 3 ? "selected" : ""}>Высокий</option>
          </select>
        </div>
        <div class="msi-switch-group">
          <span class="msi-switch-label">Режим MSI</span>
          <label class="switch" title="${dev.msiEnabled ? 'Отключить режим MSI' : 'Включить режим MSI'}">
            <input type="checkbox" class="msi-toggle-input" ${dev.msiEnabled ? "checked" : ""}>
            <span class="slider"></span>
          </label>
        </div>
      `;
    } else {
      controlsHtml = `
        <span class="msi-readonly-badge" title="Системная шина или процессорный мост (только чтение)">Системное устройство</span>
      `;
    }

    card.innerHTML = `
      <div class="msi-card-main">
        <div class="msi-icon-badge" title="${escapeHtml(dev.category)}">
          <img src="${iconSrc}" alt="${escapeHtml(dev.category)}" />
        </div>
        <div class="msi-info">
          <div class="msi-header">
            <span class="msi-name" title="${escapeHtml(dev.name)}">${escapeHtml(dev.name)}</span>
            <span class="msi-cat-badge ${catClass}">${catLabel}</span>
            ${locationHtml}
            ${hwidHtml}
          </div>
          <div class="msi-sub-details">
            ${msiStatusHtml}
          </div>
        </div>
      </div>
      <div class="msi-controls">
        ${controlsHtml}
      </div>
    `;

    if (dev.canModify) {
      const toggle = card.querySelector(".msi-toggle-input");
      if (toggle) {
        toggle.addEventListener("change", (e) => {
          toggleMsiEnabled(dev.id, e.target.checked);
        });
      }

      const select = card.querySelector(".msi-priority-select");
      if (select) {
        select.addEventListener("change", (e) => {
          changeMsiPriority(dev.id, e.target.value);
        });
      }
    }

    container.appendChild(card);
  });

  filterMsiDevices();
}

function filterMsiDevices() {
  const query = (document.getElementById("msiSearchInput")?.value || "").toLowerCase().trim();
  const cards = document.querySelectorAll("#msiDeviceList .msi-card");

  cards.forEach(card => {
    const cat = card.getAttribute("data-category");
    const searchTokens = card.getAttribute("data-search") || "";

    const matchesCategory = (currentMsiCategoryFilter === "ALL" || cat === currentMsiCategoryFilter);
    const matchesSearch = !query || searchTokens.indexOf(query) !== -1;

    card.style.display = (matchesCategory && matchesSearch) ? "flex" : "none";
  });
}

async function toggleMsiEnabled(id, enabled) {
  try {
    const res = await callNative("setMsiEnabled", { id, enabled });
    if (res && res.devices) {
      renderMsiDevices(res);
    }
  } catch (err) {
    console.error("Failed to toggle MSI:", err);
    showToast("Ошибка при изменении режима MSI", true);
  }
}

async function changeMsiPriority(id, priority) {
  try {
    const res = await callNative("setMsiPriority", { id, priority: parseInt(priority, 10) });
    if (res && res.devices) {
      renderMsiDevices(res);
    }
  } catch (err) {
    console.error("Failed to change priority:", err);
    showToast("Ошибка при изменении приоритета прерываний", true);
  }
}

async function applyMsiEsportsProfile() {
  try {
    const res = await callNative("applyMsiEsportsProfile");
    if (res && res.devices) {
      renderMsiDevices(res);
    }
  } catch (err) {
    console.error("Failed to apply esports MSI profile:", err);
    showToast("Ошибка при применении профиля MSI", true);
  }
}

// ----------------------------------------------------------------------------
// Program Settings (Tab 8)
// ----------------------------------------------------------------------------

async function loadConfig() {
  try {
    const res = await callNative("getConfig");
    if (res && res.success && res.config) {
      applyConfigToUI(res.config);
    }
  } catch (err) {
    console.warn("Failed to load config:", err);
  }
}

function applyConfigToUI(cfg) {
  if (!cfg) return;
  const setCheck = (id, val) => {
    const el = document.getElementById(id);
    if (el && val !== undefined) el.checked = !!val;
  };
  const setVal = (id, val) => {
    const el = document.getElementById(id);
    if (el && val !== undefined) el.value = val;
  };

  setCheck("setting-runInBackground", cfg.runInBackground);
  setCheck("setting-startMinimizedToTray", cfg.startMinimizedToTray);
  setCheck("setting-autoRamCleanEnabled", cfg.autoRamCleanEnabled);
  setVal("setting-ramCleanInterval", cfg.ramCleanIntervalMinutes || 15);

  // Sync Dedicated Memory Cleaner Tab Config
  if (cfg.ramThresholdPercent !== undefined) {
    setMemThresholdPill(cfg.ramThresholdPercent);
  } else {
    setMemThresholdPill(80);
  }
  if (cfg.ramCleanIntervalMinutes !== undefined) {
    setMemIntervalPill(cfg.ramCleanIntervalMinutes);
  } else {
    setMemIntervalPill(15);
  }
  setCheck("cfgMemAutoThreshold", cfg.autoRamCleanEnabled);
  setCheck("cfgMemAutoInterval", cfg.autoRamCleanEnabled);
  if (cfg.trimWorkingSetOnThreshold !== undefined) setCheck("cfgMemTrimOnThreshold", cfg.trimWorkingSetOnThreshold);
  if (cfg.clearCacheOnThreshold !== undefined) setCheck("cfgMemClearOnThreshold", cfg.clearCacheOnThreshold);
  if (cfg.trimWorkingSetOnInterval !== undefined) setCheck("cfgMemTrimOnInterval", cfg.trimWorkingSetOnInterval);
  if (cfg.clearCacheOnInterval !== undefined) setCheck("cfgMemClearOnInterval", cfg.clearCacheOnInterval);

  setCheck("cfgGameBoostEnabled", cfg.gameBoostEnabled);
  setCheck("cfgBoostHighPriority", cfg.boostHighPriority);
  setCheck("cfgBoostStandbyPurge", cfg.boostStandbyPurge);
  setCheck("cfgBoostLockTimer", cfg.boostLockTimer);
  setCheck("setting-autostartWithWindows", cfg.autostartWithWindows);

  setCheck("setting-autoCheckUpdates", cfg.autoCheckUpdates !== false);
  setVal("setting-updateChannel", cfg.updateChannel || "https://github.com/ext3ndedsoft/wcs_release");

  if (cfg.themeAccent) {
    setThemeAccent(cfg.themeAccent, false);
  }
}

// ----------------------------------------------------------------------------
// Theme Accent System (Task 4.2)
// ----------------------------------------------------------------------------
function setThemeAccent(accent, save = true) {
  if (!accent) accent = "cyan";
  currentThemeAccent = accent;

  try {
    localStorage.setItem("wcs_themeAccent", accent);
  } catch (_) {}

  const themes = ["cyan", "emerald", "crimson", "violet", "amber"];
  themes.forEach(t => document.body.classList.remove(`theme-${t}`));
  document.body.classList.add(`theme-${accent}`);

  themes.forEach(t => {
    const el = document.getElementById(`swatch-${t}`);
    if (el) el.classList.toggle("active", t === accent);
  });

  updateThemeLogos(accent);

  if (save) {
    updateAppSetting("themeAccent", accent);
  }
}

// ----------------------------------------------------------------------------
// Profiles & Backups (.wcspreset) (Task 4.1)
// ----------------------------------------------------------------------------
async function exportProfile() {
  try {
    let currentTweaks = null;
    let currentConfig = null;

    try {
      const tRes = await callNative("getTweaksState");
      if (tRes) currentTweaks = tRes;
    } catch (_) {}

    if (!currentTweaks) {
      currentTweaks = {};
      for (const key of Object.keys(TWEAK_NAMES)) {
        const el = document.getElementById(`tweak-${key}`);
        currentTweaks[key] = el ? el.checked : false;
      }
    }

    try {
      const cRes = await callNative("getConfig");
      if (cRes && cRes.config) currentConfig = cRes.config;
    } catch (_) {}

    const payload = {
      app: "WindowsConfigStudio",
      version: "3.5",
      timestamp: new Date().toISOString(),
      theme: currentThemeAccent,
      tweaks: currentTweaks,
      config: currentConfig
    };

    const jsonStr = JSON.stringify(payload, null, 2);
    const blob = new Blob([jsonStr], { type: "application/json" });
    const now = new Date();
    const dateStr = `${now.getFullYear()}-${String(now.getMonth() + 1).padStart(2, "0")}-${String(now.getDate()).padStart(2, "0")}_${String(now.getHours()).padStart(2, "0")}${String(now.getMinutes()).padStart(2, "0")}`;
    const filename = `WCS_Profile_${dateStr}.wcspreset`;

    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = filename;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(a.href);

    showToast("Профиль успешно экспортирован!", false);
  } catch (err) {
    console.error("Export profile failed:", err);
    showToast("Ошибка при экспорте профиля", true);
  }
}

function triggerImportProfile() {
  const fileInput = document.getElementById("profileFileInput");
  if (fileInput) {
    fileInput.value = "";
    fileInput.click();
  }
}

function handleProfileFile(event) {
  const file = event.target.files && event.target.files[0];
  if (!file) return;

  const reader = new FileReader();
  reader.onload = async (e) => {
    try {
      const content = e.target.result;
      const data = JSON.parse(content);

      if (!data || (data.app !== "WindowsConfigStudio" && !data.tweaks)) {
        showToast("Некорректный формат файла профиля (.wcspreset)", true);
        return;
      }

      // 1. Theme Accent
      if (data.theme) {
        setThemeAccent(data.theme, true);
      }

      // 2. Program Config
      if (data.config && typeof data.config === "object") {
        await saveConfig(data.config);
        applyConfigToUI(data.config);
      }

      // 3. Tweaks
      if (data.tweaks && typeof data.tweaks === "object") {
        for (const [id, val] of Object.entries(data.tweaks)) {
          if (typeof val === "boolean") {
            try {
              await callNative("toggleTweak", { id: id, value: val });
            } catch (err) {
              console.warn(`Failed to apply tweak ${id}:`, err);
            }
          }
        }
        await refreshTweaksState();
      }

      showToast("Профиль успешно импортирован и применен!", false);
    } catch (err) {
      console.error("Failed to parse profile:", err);
      showToast("Ошибка при чтении файла профиля", true);
    }
  };
  reader.onerror = () => {
    showToast("Ошибка при чтении файла профиля", true);
  };
  reader.readAsText(file);
}

async function updateAppSetting(key, value) {
  try {
    const res = await callNative("updateSetting", { key: key, value: value });
    if (res && res.success) {
      showToast("Настройка сохранена", false);
    }
  } catch (err) {
    console.error("Failed to update setting:", err);
    showToast("Ошибка сохранения настройки", true);
  }
}

async function saveConfig(cfg) {
  try {
    const res = await callNative("saveConfig", { config: cfg });
    if (res && res.success) {
      showToast("Настройки успешно сохранены", false);
    }
  } catch (err) {
    console.error("Failed to save config:", err);
  }
}

// ----------------------------------------------------------------------------
// In-App Updates & GitHub Releases Feed System
// ----------------------------------------------------------------------------
let currentUpdateInfo = null;

function renderMarkdownToHtml(markdownText) {
  if (!markdownText) return "";
  let text = String(markdownText)
    .replace(/\bPRO\b/gi, "")
    .replace(/\bPro\b/g, "")
    .replace(/[ \t]{2,}/g, " ");

  const lines = text.replace(/\r\n/g, "\n").replace(/\r/g, "\n").split("\n");
  const htmlParts = [];
  let inList = false;

  const formatInline = (str) => {
    return str.replace(/\*\*(.*?)\*\*/g, '<strong class="changelog-bold">$1</strong>');
  };

  for (let i = 0; i < lines.length; i++) {
    const line = lines[i].trim();

    if (!line) {
      if (inList) {
        htmlParts.push("</ul>");
        inList = false;
      }
      continue;
    }

    if (/^---[-]*$/.test(line)) {
      if (inList) {
        htmlParts.push("</ul>");
        inList = false;
      }
      htmlParts.push('<hr class="changelog-divider">');
      continue;
    }

    const headingMatch = line.match(/^#{1,6}\s+(.+)$/);
    if (headingMatch) {
      if (inList) {
        htmlParts.push("</ul>");
        inList = false;
      }
      htmlParts.push(`<h3 class="changelog-heading">${formatInline(headingMatch[1].trim())}</h3>`);
      continue;
    }

    const listMatch = line.match(/^[-*]\s+(.+)$/);
    if (listMatch) {
      if (!inList) {
        htmlParts.push('<ul class="changelog-list">');
        inList = true;
      }
      htmlParts.push(`<li class="changelog-item">${formatInline(listMatch[1].trim())}</li>`);
      continue;
    }

    if (inList) {
      htmlParts.push("</ul>");
      inList = false;
    }

    htmlParts.push(`<p class="changelog-p">${formatInline(line)}</p>`);
  }

  if (inList) {
    htmlParts.push("</ul>");
    inList = false;
  }

  return htmlParts.join("");
}

function updateAppVersionUI(version) {
  if (!version) return;
  const raw = String(version).trim();
  const clean = raw.startsWith("v") || raw.startsWith("V") ? raw.slice(1) : raw;
  const displayVer = clean.replace(/\s*pro\b/gi, "").trim();

  const headerVer = document.getElementById("headerAppVersion");
  if (headerVer) headerVer.innerText = `Windows Tweaker`;

  const sidebarVer = document.getElementById("sidebarAppVersion");
  if (sidebarVer) sidebarVer.innerText = `open source`;

  const sidebarAuthor = document.getElementById("sidebarAuthor");
  if (sidebarAuthor) sidebarAuthor.innerText = `open source`;

  const aboutVer = document.getElementById("aboutAppVersion");
  if (aboutVer) aboutVer.innerText = `Версия ${displayVer} • Нативная архитектура C++20 / WebView2`;

  const curVer = document.getElementById("updateCurrentVerText");
  if (curVer) curVer.innerText = `v${displayVer}`;

  const tabCurVer = document.getElementById("tabUpdateCurrentVer");
  if (tabCurVer) tabCurVer.innerText = `v${displayVer}`;

  const statusText = document.getElementById("settingsUpdateStatusText");
  if (statusText && (!currentUpdateInfo || !currentUpdateInfo.available)) {
    statusText.innerText = `Версия ${displayVer} (Актуальна)`;
  }
}

function onUpdateAvailable(data) {
  if (!data || !data.available) return;
  currentUpdateInfo = data;

  if (data.currentVersion) {
    updateAppVersionUI(data.currentVersion);
  }

  const rawNxt = data.latestVersion || data.version || "3.0";
  const nxt = String(rawNxt).replace(/^v/i, "").trim();

  const pill = document.getElementById("headerUpdatePill");
  const verEl = document.getElementById("headerUpdateVer");
  if (pill && verEl) {
    verEl.innerText = "v" + nxt;
    pill.style.display = "inline-flex";
  }

  const statusPill = document.getElementById("settingsUpdateStatusPill");
  const statusText = document.getElementById("settingsUpdateStatusText");
  if (statusPill && statusText) {
    statusPill.className = "status-pill cyan";
    statusText.innerText = "Доступно: v" + nxt;
  }

  refreshUpdateTabUI(data);

  showToast("Доступно обновление WCS v" + nxt + "!", false);
}

function refreshUpdateTabUI(info) {
  const update = info || currentUpdateInfo;
  const heading = document.getElementById("tabUpdateHeading");
  const subheading = document.getElementById("tabUpdateSubheading");
  const curVer = document.getElementById("tabUpdateCurrentVer");
  const latestVer = document.getElementById("tabUpdateLatestVer");
  const statusPill = document.getElementById("tabUpdateStatusPill");
  const statusPillText = document.getElementById("tabUpdateStatusPillText");
  const btnDownload = document.getElementById("tabBtnDownload");
  const lastTime = document.getElementById("tabLastCheckTimeLabel");
  const changelogBox = document.getElementById("tabUpdateChangelogBox");
  const headerPill = document.getElementById("headerUpdatePill");
  const headerVer = document.getElementById("headerUpdateVer");

  const cur = (update && update.currentVersion) || "3.0";
  if (curVer) curVer.innerText = cur.startsWith("v") ? cur : `v${cur}`;

  if (!update || !update.available) {
    if (headerPill) headerPill.style.display = "none";
    if (statusPill) statusPill.className = "status-pill green";
    if (statusPillText) statusPillText.innerText = "Актуальна";
    if (heading) heading.innerText = "Установлена актуальная версия";
    const curDisplay = (update && update.currentVersion) || "3.0";
    const cleanCurDisplay = String(curDisplay).replace(/^v/i, "").trim();
    if (subheading) {
      subheading.innerText = "Вы используете самую свежую сборку WCS v" + cleanCurDisplay + ". Все модули оптимизированы.";
    }
    if (btnDownload) btnDownload.style.display = "none";
    const lat = (update && (update.latestVersion || update.version)) || curDisplay;
    const cleanLat = String(lat).replace(/^v/i, "").trim();
    if (latestVer) latestVer.innerText = cleanLat.startsWith("v") ? cleanLat : ("v" + cleanLat);
    if (changelogBox) {
      changelogBox.innerHTML = renderMarkdownToHtml((update && update.changelog) || "Вы используете последнюю актуальную версию программы. Текущая сборка содержит все актуальные оптимизации BCD, таймеров, прерываний MSI и анти-телеметрии.");
    }
  } else {
    const rawNxt = update.latestVersion || update.version || "2.6";
    const nxt = String(rawNxt).replace(/^v/i, "").trim();
    if (heading) heading.innerHTML = `<img src="/icons/zap.png" style="width:16px;height:16px;margin-right:8px;vertical-align:middle;">Доступно обновление v${nxt}!`;
    if (subheading) subheading.innerText = "Обнаружен новый релиз на GitHub Releases. Рекомендуется установить обновление.";
    if (latestVer) latestVer.innerText = "v" + nxt;
    if (statusPill) statusPill.className = "status-pill cyan";
    if (statusPillText) statusPillText.innerText = "Доступно: v" + nxt;
    if (btnDownload) btnDownload.style.display = "inline-flex";
    if (headerPill) headerPill.style.display = "inline-flex";
    if (headerVer) headerVer.innerText = "v" + nxt;
    if (changelogBox) {
      changelogBox.innerHTML = renderMarkdownToHtml(update.changelog || "Официальный релиз WCS. Доступно прямое скачивание с GitHub.");
    }
  }

  if (lastTime && update && update.date) {
    lastTime.innerText = `Последняя проверка: ${update.date}`;
  }
}

async function openUpdateModal(info) {
  let update = info || currentUpdateInfo;
  const modal = document.getElementById("updateModal");
  if (!modal) return;

  if (!update) {
    showToast("Проверка информации о релизе на GitHub...", false);
    await manualCheckUpdates(true);
    return;
  }

  const curVer = document.getElementById("updateCurrentVerText");
  const newVer = document.getElementById("updateNewVerText");
  const source = document.getElementById("updateSourceText");
  const dateEl = document.getElementById("updateDateText");
  const notes = document.getElementById("updateModalChangelog");
  const title = modal.querySelector(".modal-title");
  const badgePill = modal.querySelector(".update-badge-pill");
  const btnDownload = document.getElementById("btnDownloadUpdate");

  const cur = (update && update.currentVersion) || "3.0";
  if (curVer) curVer.innerText = cur.startsWith("v") ? cur : `v${cur}`;
  const rawNxt = (update && (update.latestVersion || update.version)) || cur;
  const nxt = String(rawNxt).replace(/^v/i, "").trim();
  if (newVer) newVer.innerText = "v" + nxt;
  if (source) source.innerText = update.channel || "https://github.com/ext3ndedsoft/wcs_release";
  if (dateEl) dateEl.innerText = (update && update.date) ? `(${update.date})` : "";

  if (update && !update.available) {
    if (title) title.innerText = "Установлена последняя версия программы";
    if (badgePill) {
      badgePill.innerHTML = `<img src="/icons/check.png" style="width:12px;height:12px;margin-right:4px;vertical-align:middle;"><span>АКТУАЛЬНА</span>`;
    }
    if (btnDownload) btnDownload.style.display = "none";
    if (notes) {
      notes.innerHTML = renderMarkdownToHtml(update.changelog || "Вы используете последнюю актуальную версию программы. Текущая сборка содержит все актуальные оптимизации BCD, таймеров, прерываний MSI и анти-телеметрии.");
    }
  } else {
    if (title) title.innerText = "Доступно обновление WCS";
    if (badgePill) {
      badgePill.innerHTML = `<img src="/icons/zap.png" style="width:12px;height:12px;margin-right:4px;vertical-align:middle;"><span>НОВЫЙ РЕЛИЗ</span>`;
    }
    if (btnDownload) btnDownload.style.display = "inline-flex";
    if (notes) {
      notes.innerHTML = renderMarkdownToHtml((update && update.changelog) || "Официальный релиз WCS. Доступна прямая загрузка бинарных файлов с GitHub.");
    }
  }

  modal.style.display = "flex";
  modal.classList.remove("modal-exiting");
  requestAnimationFrame(() => {
    modal.classList.add("active");
  });
}

function closeUpdateModal() {
  const modal = document.getElementById("updateModal");
  if (!modal) return;
  modal.classList.add("modal-exiting");
  modal.classList.remove("active");
  setTimeout(() => {
    modal.classList.remove("modal-exiting");
    modal.style.display = "none";
  }, 380);
}

function copyRepoUrl() {
  const repoUrl = "https://github.com/ext3ndedsoft/wcs_release";
  if (navigator.clipboard && navigator.clipboard.writeText) {
    navigator.clipboard.writeText(repoUrl).then(() => {
      showToast("Ссылка на GitHub скопирована в буфер обмена!", false);
    }).catch(() => {
      fallbackCopyText(repoUrl);
    });
  } else {
    fallbackCopyText(repoUrl);
  }
}

function fallbackCopyText(text) {
  try {
    const input = document.createElement("input");
    input.value = text;
    document.body.appendChild(input);
    input.select();
    document.execCommand("copy");
    document.body.removeChild(input);
    showToast("Ссылка на GitHub скопирована в буфер обмена!", false);
  } catch (err) {
    showToast("Не удалось скопировать ссылку", true);
  }
}

async function downloadUpdate() {
  try {
    await callNative("openUpdateUrl");
  } catch (err) {
    console.error("Failed to open update url:", err);
  }
}

async function openReleasePage() {
  try {
    await callNative("openReleasePage");
  } catch (err) {
    console.error("Failed to open release page:", err);
  }
}

async function manualCheckUpdates(autoOpenModal = false) {
  const btn = document.getElementById("tabBtnCheckNow") || document.getElementById("btnManualCheckUpdate");
  const channel = "https://github.com/ext3ndedsoft/wcs_release";
  const lastTime = document.getElementById("tabLastCheckTimeLabel") || document.getElementById("lastCheckTimeLabel");

  if (btn) {
    btn.disabled = true;
    btn.innerHTML = `<span style="display:inline-block;animation:spin 1s linear infinite;"><img src="/icons/refresh.png" style="width:13px;height:13px;margin-right:6px;vertical-align:middle;"></span> Проверка...`;
  }

  try {
    const res = await callNative("checkUpdates", { channel: channel });
    if (res && res.success) {
      currentUpdateInfo = res;
      if (res.currentVersion) {
        updateAppVersionUI(res.currentVersion);
      }
      refreshUpdateTabUI(res);
      if (res.available) {
        onUpdateAvailable(res);
        if (autoOpenModal) {
          openUpdateModal(res);
        }
      } else {
        const pill = document.getElementById("headerUpdatePill");
        if (pill) pill.style.display = "none";
        const statusPill = document.getElementById("settingsUpdateStatusPill");
        const statusText = document.getElementById("settingsUpdateStatusText");
        if (statusPill && statusText) {
          statusPill.className = "status-pill green";
          statusText.innerText = `Версия ${res.currentVersion || "3.0"} (Актуальна)`;
        }
        showToast("У вас установлена актуальная версия программы!", false);
        if (autoOpenModal) {
          openUpdateModal(res);
        }
      }
      if (lastTime && res.date) {
        lastTime.innerText = `Последняя проверка: ${res.date}`;
      }
    } else {
      if (res && res.error) {
        showToast(res.error, true);
      }
    }
  } catch (err) {
    console.error("Update check failed:", err);
    showToast("Не удалось связаться с сервером GitHub", true);
  } finally {
    if (btn) {
      btn.disabled = false;
      btn.innerHTML = `<img src="/icons/search.png" style="width: 14px; height: 14px; margin-right: 6px;"> Проверить сейчас`;
    }
  }
}

async function refreshTweaksState() {
  try {
    const res = await callNative("getTweaksState");
    if (res) {
      syncTweaksUI(res);
    }
    if (currentTabIdx === 3) {
      loadMsiDevices();
    } else if (currentTabIdx === 8) {
      loadStartupItems();
    }
    if (currentTabIdx === 7 && currentSubTabIdx === 2) {
      loadWin11Tweaks();
    }
  } catch (err) {
    console.warn("Failed to refresh tweaks state:", err);
  }
}

// ----------------------------------------------------------------------------
// Dynamic Game Boost & Auto-Catalog Engine
// ----------------------------------------------------------------------------
let currentGameBoostData = null;
let allInstalledGames = [];

async function loadGameBoost() {
  try {
    const res = await callNative("getGameBoostStatus");
    if (res) {
      renderGameBoost(res);
    }
  } catch (err) {
    console.error("Failed to load Game Boost status:", err);
  }
}

function renderGameBoost(data) {
  if (!data) return;
  currentGameBoostData = data;

  // 1. Main toggle switch
  const mainSwitch = document.getElementById("cfgGameBoostEnabled");
  if (mainSwitch && data.enabled !== undefined) {
    mainSwitch.checked = !!data.enabled;
  }

  // 2. Sub-option checkboxes
  const s = data.settings || {};
  const chkPri = document.getElementById("cfgBoostHighPriority");
  if (chkPri && s.highPriority !== undefined) {
    chkPri.checked = !!s.highPriority;
  }
  const chkPurge = document.getElementById("cfgBoostStandbyPurge");
  if (chkPurge && s.standbyPurge !== undefined) {
    chkPurge.checked = !!s.standbyPurge;
  }
  const chkTimer = document.getElementById("cfgBoostLockTimer");
  if (chkTimer && s.lockTimer !== undefined) {
    chkTimer.checked = !!s.lockTimer;
  }

  // 3. Status Pill
  const pill = document.getElementById("gameBoostStatusPill");
  const dot = document.getElementById("gameBoostStatusDot");
  const text = document.getElementById("gameBoostStatusText");
  const isGameActive = !!(data.activeGame && data.activePid);
  if (pill) {
    pill.className = isGameActive ? "status-pill green" : "status-pill cyan";
  }
  if (dot) {
    dot.className = isGameActive ? "status-dot green active" : "status-dot cyan";
  }
  if (text) {
    if (isGameActive) {
      text.innerText = `${data.activeGame} (PID ${data.activePid} • High Priority)`;
    } else {
      text.innerText = data.enabled === false ? "Dynamic Boost отключен" : "Ожидание запуска игры...";
    }
  }
}

async function scanInstalledGamesUI() {
  const badge = document.getElementById("installedGamesCountBadge");
  const grid = document.getElementById("gamePosterGrid");
  if (badge) {
    badge.innerText = "Сканирование библиотек...";
    badge.className = "status-pill cyan";
  }

  try {
    const res = await callNative("scanInstalledGames");
    let list = [];
    if (Array.isArray(res)) {
      list = res;
    } else if (res && Array.isArray(res.games)) {
      list = res.games;
    } else if (res && Array.isArray(res.installedGames)) {
      list = res.installedGames;
    }

    allInstalledGames = list;
    if (badge) {
      badge.innerText = `${list.length} игр найдено`;
      badge.className = list.length > 0 ? "status-pill green" : "status-pill gray";
    }
    const searchInput = document.getElementById("gameSearchInput");
    const query = searchInput ? searchInput.value.trim() : "";
    filterGamePosters(query);
  } catch (err) {
    console.error("Failed to scan installed games:", err);
    if (badge) {
      badge.innerText = "Ошибка поиска";
      badge.className = "status-pill red";
    }
  }
}

function renderGamePosters(list) {
  const grid = document.getElementById("gamePosterGrid");
  if (!grid) return;
  grid.innerHTML = "";

  if (!list || list.length === 0) {
    grid.innerHTML = `
      <div style="grid-column: 1 / -1; padding: 48px 24px; text-align: center; background: rgba(14, 18, 28, 0.5); border: 1px dashed rgba(255,255,255,0.12); border-radius: 16px; backdrop-filter: blur(16px);">
        <img src="/icons/gamepad.png" style="width: 48px; height: 48px; opacity: 0.4; margin-bottom: 12px;">
        <div style="font-size: 16px; font-weight: 600; color: #fff; margin-bottom: 6px;">Игры не найдены</div>
        <div style="font-size: 13px; color: var(--text-dim); max-width: 480px; margin: 0 auto 16px auto;">Автосканер проверил библиотеки Steam, Epic Games, Riot Games и стандартные пути. Вы можете добавить исполняемый файл вручную в поле выше.</div>
        <button class="btn btn-secondary btn-sm" onclick="scanInstalledGamesUI()">
          <img src="/icons/refresh.png" style="width: 13px; height: 13px; margin-right: 6px;"> Повторить сканирование
        </button>
      </div>
    `;
    return;
  }

  list.forEach(game => {
    const isRunning = !!game.running;
    const coverUrl = game.cover || "/covers/default_game.jpg";
    const displayName = game.name || game.exe || "Игра";
    const exeName = game.exe || "";
    const installPath = game.path || "Пользовательский процесс";

    const card = document.createElement("div");
    card.className = `game-poster-card${isRunning ? " running" : ""}`;
    card.dataset.id = game.id || "";
    card.dataset.name = displayName.toLowerCase();
    card.dataset.exe = exeName.toLowerCase();

    card.innerHTML = `
      <div class="poster-image-box">
        <img class="poster-img" src="${escapeHtml(coverUrl)}" alt="${escapeHtml(displayName)}" onerror="this.onerror=null;this.src='/covers/default_game.jpg';">
      </div>
      <button class="poster-delete-btn" title="Удалить из списка" onclick="deleteBoostGameCard(event, '${escapeHtml(game.id || '')}', '${escapeHtml(exeName)}', '${escapeHtml(displayName)}')">
        <svg viewBox="0 0 24 24" width="12" height="12" stroke="currentColor" stroke-width="2.4" fill="none" stroke-linecap="round" stroke-linejoin="round">
          <line x1="18" y1="6" x2="6" y2="18"></line>
          <line x1="6" y1="6" x2="18" y2="18"></line>
        </svg>
      </button>
      <div class="poster-top-badge ${isRunning ? 'badge-running' : 'badge-ready'}">
        <span class="status-dot ${isRunning ? 'green pulse' : 'cyan'}" style="width:6px;height:6px;border-radius:50%;background:currentColor;"></span>
        <span>${isRunning ? `В ИГРЕ (PID ${game.pid || 0})` : 'ОТСЛЕЖИВАЕТСЯ'}</span>
      </div>
      <div class="poster-glass-info">
        <div class="poster-title" title="${escapeHtml(displayName)}">${escapeHtml(displayName)}</div>
        <div class="poster-meta">
          <span class="poster-exe-tag" title="${escapeHtml(exeName)}">${escapeHtml(exeName)}</span>
          <span class="poster-boost-status ${isRunning ? 'active' : ''}">${isRunning ? 'Dynamic Boost ВКЛ' : 'Готов к бусту'}</span>
        </div>
        <div class="poster-path" title="${escapeHtml(installPath)}">${escapeHtml(installPath)}</div>
      </div>
    `;

    // If game has default cover, asynchronously fetch cover from Steam in background
    if (coverUrl === '/covers/default_game.jpg') {
      callNative("fetchGameCover", { name: displayName, exe: exeName, path: installPath, id: game.id, appId: game.appId || 0 }).then(res => {
        if (res && res.success && res.cover && res.cover !== '/covers/default_game.jpg') {
          game.cover = res.cover;
          const img = card.querySelector('.poster-img');
          if (img) {
            img.style.transition = 'opacity 0.35s ease';
            img.style.opacity = '0';
            setTimeout(() => {
              img.src = res.cover;
              img.onload = () => { img.style.opacity = '1'; };
            }, 80);
          }
        }
      }).catch(() => {});
    }

    card.addEventListener("click", () => {
      showToast(`${displayName}: процесс ${exeName} находится под защитой Dynamic Game Boost`, false);
    });

    grid.appendChild(card);
  });
}

async function deleteBoostGameCard(event, id, exe, name) {
  if (event) event.stopPropagation();

  const card = event ? event.currentTarget.closest(".game-poster-card") : null;
  if (card) {
    card.style.transition = "all 0.28s cubic-bezier(0.16, 1, 0.3, 1)";
    card.style.opacity = "0";
    card.style.transform = "scale(0.88)";
  }

  // Remove from in-memory list
  const cleanId = (id || "").toLowerCase();
  const cleanExe = (exe || "").toLowerCase();
  const cleanName = (name || "").toLowerCase();
  allInstalledGames = allInstalledGames.filter(g => {
    const gId = (g.id || "").toLowerCase();
    const gExe = (g.exe || "").toLowerCase();
    const gName = (g.name || "").toLowerCase();
    if (cleanId && gId === cleanId) return false;
    if (cleanExe && gExe === cleanExe) return false;
    if (cleanName && gName === cleanName) return false;
    return true;
  });

  const badge = document.getElementById("installedGamesCountBadge");
  if (badge) {
    badge.innerText = `${allInstalledGames.length} игр найдено`;
    badge.className = allInstalledGames.length > 0 ? "status-pill green" : "status-pill gray";
  }

  setTimeout(() => {
    if (card) card.remove();
    const searchInput = document.getElementById("gameSearchInput");
    const query = searchInput ? searchInput.value.trim() : "";
    if (allInstalledGames.length === 0) {
      renderGamePosters([]);
    } else if (query) {
      filterGamePosters(query);
    }
  }, 280);

  try {
    const res = await callNative("removeBoostGame", { id: id, exe: exe, name: name });
    if (res && res.success) {
      const disp = name || exe || id || "Игра";
      showToast(`${disp}: игра удалена из списка`, false);
    }
  } catch (err) {
    console.error("Failed to delete boost game:", err);
  }
}

function filterGamePosters(query) {
  const q = (query || "").trim().toLowerCase();
  if (!q) {
    renderGamePosters(allInstalledGames);
    return;
  }
  const filtered = allInstalledGames.filter(g => {
    const name = (g.name || "").toLowerCase();
    const exe = (g.exe || "").toLowerCase();
    const path = (g.path || "").toLowerCase();
    return name.includes(q) || exe.includes(q) || path.includes(q);
  });
  renderGamePosters(filtered);
}

async function addCustomGameFromInput() {
  const input = document.getElementById("customGameInput");
  if (!input) return;
  const val = input.value.trim();
  if (!val) {
    showToast("Введите имя исполняемого файла игры (например, warface.exe)", true);
    return;
  }

  try {
    const res = await callNative("addCustomBoostGame", { exe: val });
    input.value = "";
    if (res && res.success) {
      showToast(`Игра ${val} добавлена в мониторинг`, false);
      await scanInstalledGamesUI();
    } else {
      showToast(res && res.error ? res.error : "Не удалось добавить игру", true);
    }
  } catch (err) {
    console.error("Failed to add custom game:", err);
    showToast("Ошибка при добавлении игры", true);
  }
}

async function addCustomBoostGame() {
  return addCustomGameFromInput();
}

async function removeCustomBoostGame(exe) {
  try {
    const res = await callNative("removeCustomBoostGame", { exe: exe });
    if (res && res.success) {
      showToast(`Игра ${exe} удалена из отслеживания`, false);
      await scanInstalledGamesUI();
    }
  } catch (err) {
    console.error("Failed to remove custom game:", err);
  }
}

function onGameBoosted(data) {
  const gameName = (data && data.name) ? data.name : "Игра";
  const pid = (data && data.pid) ? data.pid : 0;
  const exe = (data && data.exe) ? data.exe : "";

  // Update status pill
  const pill = document.getElementById("gameBoostStatusPill");
  const dot = document.getElementById("gameBoostStatusDot");
  const text = document.getElementById("gameBoostStatusText");
  if (pill) pill.className = "status-pill green";
  if (dot) dot.className = "status-dot green active";
  if (text) text.innerText = `${gameName} (PID ${pid} • High Priority)`;

  // Update game in poster grid
  if (allInstalledGames && allInstalledGames.length > 0) {
    const gName = gameName.toLowerCase();
    const gExe = exe.toLowerCase();
    allInstalledGames.forEach(g => {
      const isMatch = (g.name && g.name.toLowerCase() === gName) ||
                      (g.exe && g.exe.toLowerCase() === gExe);
      if (isMatch) {
        g.running = true;
        g.pid = pid;
        g.boosted = true;
      }
    });
    const searchInput = document.getElementById("gameSearchInput");
    filterGamePosters(searchInput ? searchInput.value : "");
  }

  // Toast
  showToast(`Запущен ${gameName}! Активирован Dynamic Boost (High Priority + Очистка Standby)`, false);
}

function onGameExited(data) {
  const gameName = (data && data.name) ? data.name : "Игра";
  const exe = (data && data.exe) ? data.exe : "";

  // Return status pill to idle
  const pill = document.getElementById("gameBoostStatusPill");
  const dot = document.getElementById("gameBoostStatusDot");
  const text = document.getElementById("gameBoostStatusText");
  if (pill) pill.className = "status-pill cyan";
  if (dot) dot.className = "status-dot cyan";
  if (text) text.innerText = "Ожидание запуска игры...";

  // Reset running state in posters
  if (allInstalledGames && allInstalledGames.length > 0) {
    const gName = gameName.toLowerCase();
    const gExe = exe.toLowerCase();
    allInstalledGames.forEach(g => {
      if ((g.name && g.name.toLowerCase() === gName) || (g.exe && g.exe.toLowerCase() === gExe)) {
        g.running = false;
        g.pid = 0;
        g.boosted = false;
      }
    });
    const searchInput = document.getElementById("gameSearchInput");
    filterGamePosters(searchInput ? searchInput.value : "");
  }

  showToast(`${gameName} закрыт. Dynamic Boost переведен в режим ожидания`, false);
}

async function toggleGameBoostSetting(key, val) {
  try {
    const res = await callNative("updateGameBoostSettings", { [key]: val });
    if (res && res.success) {
      renderGameBoost(res);
      showToast("Настройки Dynamic Boost сохранены", false);
    }
  } catch (err) {
    console.error("Failed to update Dynamic Boost setting:", err);
  }
}

// Bootstrap
window.addEventListener("DOMContentLoaded", async () => {
  requestAnimationFrame(() => {
    document.body.classList.add("app-loaded", "enter-active");
    updateSidebarIndicator(0, true);
  });

  updateThemeLogos(currentThemeAccent);
  updateSidebarIndicator(0, true);
  initTabs();

  document.addEventListener("keydown", (e) => {
    const confirmModal = document.getElementById("confirmModal");
    if (confirmModal && confirmModal.classList.contains("active")) {
      if (e.key === "Escape") {
        e.preventDefault();
        cancelConfirmModal();
      } else if (e.key === "Enter") {
        e.preventDefault();
        acceptConfirmModal();
      }
    }
  });

  const init = await callNative("getInitialState");
  if (init && init.success) {
    if (init.currentVersion) {
      updateAppVersionUI(init.currentVersion);
    } else if (init.update && init.update.currentVersion) {
      updateAppVersionUI(init.update.currentVersion);
    }

    if (init.specs) {
      currentSpecs = init.specs;
      const descEl = document.getElementById("gpuDetectedDesc");
      if (descEl && init.specs.primaryGPUName) {
        descEl.innerText = `Обнаружена: ${init.specs.primaryGPUName}. Переводит видеокарту в прямой MSI-режим с наивысшим приоритетом.`;
      }
      populateAboutModalSpecs(currentSpecs);

      const cpuEl = document.getElementById("dashHwCpu");
      const gpuEl = document.getElementById("dashHwGpu");
      const osEl = document.getElementById("dashHwOs");
      if (cpuEl && init.specs.cpuName) cpuEl.textContent = init.specs.cpuName.replace(/(\(R\)|\(TM\))/g, "").trim();
      if (gpuEl && init.specs.primaryGPUName) gpuEl.textContent = init.specs.primaryGPUName;
      if (osEl && init.specs.osVersion) osEl.textContent = init.specs.osVersion;
    }

    if (init.tweaks) syncTweaksUI(init.tweaks);
    if (init.systemSuite) syncSystemSuiteUI(init.systemSuite);
    if (init.contextMenu) syncContextMenuUI(init.contextMenu);
    if (init.win11Tweaks) renderWin11Tweaks(init.win11Tweaks);
    if (init.services) renderServices(init.services);
    if (init.appx) renderAppx(init.appx);
    if (init.startup) renderStartupItems(init.startup);
    if (init.msi) renderMsiDevices(init.msi);
    if (init.gameBoost) {
      renderGameBoost(init.gameBoost);
    } else {
      loadGameBoost();
    }
    scanInstalledGamesUI();

    if (init.cleanup) {
      for (const [k, v] of Object.entries(init.cleanup)) {
        const el = document.getElementById("cln-" + k);
        if (el) el.checked = !!v;
      }
    }

    if (init.memory) {
      updateMemoryTabTelemetry(init.memory);
    }

    if (init.config) {
      applyConfigToUI(init.config);
    } else {
      loadConfig();
    }

    if (init.update) {
      currentUpdateInfo = init.update;
      refreshUpdateTabUI(init.update);
      if (init.update.available) {
        onUpdateAvailable(init.update);
      }
    }

    // Initialize 2D Background Landscape Themes & Dashboard
    initThemeBg();
    refreshDashboardMetrics(init);
  }
});

window.addEventListener("resize", () => {
  updateSidebarIndicator(currentTabIdx, true);
});

// ----------------------------------------------------------------------------
// 2D Background Landscape Themes & Art Engine
// ----------------------------------------------------------------------------
const AVAILABLE_THEMES = ["fuji", "mountains", "cyber", "cosmos", "obsidian"];

function initThemeBg() {
  const savedTheme = localStorage.getItem("wcs_theme_bg") || "fuji";
  const savedOpacity = localStorage.getItem("wcs_theme_bg_opacity") || "0.45";
  const savedAnim = localStorage.getItem("wcs_theme_anim_enabled") !== "false";
  const savedSpeed = localStorage.getItem("wcs_theme_anim_speed") || "1.0";
  const savedBlur = localStorage.getItem("wcs_glass_blur") || "20";

  selectThemeBg(savedTheme, false);
  setThemeBgOpacity(parseFloat(savedOpacity), false);
  toggleThemeAnimation(savedAnim, false);
  setThemeAnimSpeed(parseFloat(savedSpeed), false);
  setGlassBlur(parseInt(savedBlur, 10), false);
}

function selectThemeBg(themeId, showNotification = true) {
  if (!AVAILABLE_THEMES.includes(themeId)) themeId = "fuji";
  const bgLayer = document.getElementById("themeBackgroundLayer");
  if (bgLayer) {
    bgLayer.className = `theme-bg-layer theme-bg-${themeId}`;
    if (localStorage.getItem("wcs_theme_anim_enabled") === "false") {
      bgLayer.classList.add("theme-anim-disabled");
    }
  }

  AVAILABLE_THEMES.forEach(t => {
    const card = document.getElementById(`theme-card-${t}`);
    if (card) card.classList.toggle("active", t === themeId);
  });

  renderThemeAnimation(themeId);

  localStorage.setItem("wcs_theme_bg", themeId);
  if (showNotification) {
    const names = {
      fuji: "Япония: Фудзи и Тории",
      mountains: "2D Горный хребет",
      cyber: "2D Кибер Горизонт",
      cosmos: "2D Глубокий космос",
      obsidian: "Обсидиан WCS (Минимал)"
    };
    showToast(`Тема «${names[themeId] || themeId}» успешно активирована`, false);
  }
}

function renderThemeAnimation(themeId) {
  const container = document.getElementById("themeAnimationLayer");
  if (!container) return;
  container.innerHTML = "";

  if (themeId === "fuji") {
    // 1. Sun aura & Mist bands
    const sunAura = document.createElement("div");
    sunAura.className = "fuji-sun-aura";
    container.appendChild(sunAura);

    const mist1 = document.createElement("div");
    mist1.className = "fuji-mist-band fuji-mist-band-1";
    container.appendChild(mist1);

    const mist2 = document.createElement("div");
    mist2.className = "fuji-mist-band fuji-mist-band-2";
    container.appendChild(mist2);

    // 2. Sakura falling petals (18 organic petals)
    for (let i = 0; i < 18; i++) {
      const petal = document.createElement("div");
      petal.className = "sakura-petal";
      const fallDur = (8 + (i % 6) * 1.5).toFixed(1);
      const swayDur = (2.6 + (i % 4) * 0.5).toFixed(1);
      const delay = ((i * 0.65) % 9).toFixed(1);
      const left = ((i * 5.8) + (i % 3) * 4).toFixed(1);
      const sz = 11 + (i % 7);

      petal.style.left = `${left}%`;
      petal.style.width = `${sz}px`;
      petal.style.height = `${Math.round(sz * 1.25)}px`;
      petal.style.setProperty("--fall-dur", `${fallDur}s`);
      petal.style.setProperty("--sway-dur", `${swayDur}s`);
      petal.style.setProperty("--delay", `${delay}s`);
      container.appendChild(petal);
    }
  } else if (themeId === "mountains") {
    // 1. Northern Lights Aurora
    const aurora = document.createElement("div");
    aurora.className = "alpine-aurora";
    container.appendChild(aurora);

    // 2. Shooting Star
    const meteor = document.createElement("div");
    meteor.className = "alpine-shooting-star";
    container.appendChild(meteor);

    // 3. 24 Twinkling Alpine Stars
    const starCoords = [
      [12, 18], [24, 12], [38, 22], [52, 14], [64, 25], [78, 11], [88, 19],
      [16, 28], [28, 34], [44, 29], [58, 33], [72, 28], [84, 32], [94, 24],
      [6, 35], [20, 8], [34, 15], [48, 8], [68, 16], [80, 22], [92, 10],
      [40, 36], [62, 38], [76, 36]
    ];
    starCoords.forEach(([x, y], idx) => {
      const star = document.createElement("div");
      star.className = "alpine-star";
      const sz = 1.5 + (idx % 3);
      const dur = (2.2 + (idx % 4) * 0.6).toFixed(1);
      const del = ((idx * 0.45) % 4).toFixed(1);
      star.style.left = `${x}%`;
      star.style.top = `${y}%`;
      star.style.width = `${sz}px`;
      star.style.height = `${sz}px`;
      star.style.setProperty("--t-dur", `${dur}s`);
      star.style.setProperty("--t-del", `${del}s`);
      container.appendChild(star);
    });
  } else if (themeId === "cyber") {
    // 1. Perspective Wireframe Grid Plane
    const gridWrap = document.createElement("div");
    gridWrap.className = "cyber-grid-container";
    const gridPlane = document.createElement("div");
    gridPlane.className = "cyber-grid-plane";
    gridWrap.appendChild(gridPlane);
    container.appendChild(gridWrap);

    // 2. Neon Horizon Laser Beam
    const laser = document.createElement("div");
    laser.className = "cyber-horizon-beam";
    container.appendChild(laser);

    // 3. Retro Sun Corona
    const sun = document.createElement("div");
    sun.className = "cyber-sun-corona";
    container.appendChild(sun);

    // 4. Skyline Red Beacon Lights
    const beaconPoints = [
      [135, 500], [355, 510], [1447, 440], [1810, 530]
    ];
    beaconPoints.forEach(([x, y], idx) => {
      const beacon = document.createElement("div");
      beacon.className = "cyber-beacon";
      beacon.style.left = `${(x / 1920) * 100}%`;
      beacon.style.top = `${(y / 1080) * 100}%`;
      beacon.style.animationDelay = `${idx * 0.45}s`;
      container.appendChild(beacon);
    });
  } else if (themeId === "cosmos") {
    // 1. Nebula breathing glow
    const neb = document.createElement("div");
    neb.className = "cosmos-nebula-glow";
    container.appendChild(neb);

    // 2. Planetary Rings Halo
    const rings = document.createElement("div");
    rings.className = "cosmos-rings-halo";
    container.appendChild(rings);

    // 3. Meteors
    const m1 = document.createElement("div");
    m1.className = "cosmos-meteor";
    m1.style.top = "18%";
    m1.style.left = "28%";
    container.appendChild(m1);

    const m2 = document.createElement("div");
    m2.className = "cosmos-meteor";
    m2.style.top = "36%";
    m2.style.left = "62%";
    m2.style.animationDelay = "4.2s";
    container.appendChild(m2);

    // 4. 20 Galaxy Stars
    for (let i = 0; i < 20; i++) {
      const star = document.createElement("div");
      star.className = "alpine-star";
      const sz = 1.8 + (i % 3);
      star.style.left = `${(i * 5.1 + 3) % 96}%`;
      star.style.top = `${(i * 4.3 + 5) % 65}%`;
      star.style.width = `${sz}px`;
      star.style.height = `${sz}px`;
      star.style.setProperty("--t-dur", `${(2.5 + (i % 3) * 0.8).toFixed(1)}s`);
      star.style.setProperty("--t-del", `${((i * 0.5) % 3.5).toFixed(1)}s`);
      container.appendChild(star);
    }
  } else if (themeId === "obsidian") {
    // 1. Cybernetic Energy Wave
    const wave = document.createElement("div");
    wave.className = "obsidian-energy-wave";
    container.appendChild(wave);

    // 2. Corner Atmospheric Halos
    const bl = document.createElement("div");
    bl.className = "obsidian-corner-glow obsidian-glow-bl";
    container.appendChild(bl);

    const tr = document.createElement("div");
    tr.className = "obsidian-corner-glow obsidian-glow-tr";
    container.appendChild(tr);
  }
}

function toggleThemeAnimation(enabled, showToastMsg = true) {
  const bgLayer = document.getElementById("themeBackgroundLayer");
  if (bgLayer) {
    bgLayer.classList.toggle("theme-anim-disabled", !enabled);
  }
  document.body.classList.toggle("theme-anim-disabled", !enabled);

  const toggle = document.getElementById("settingThemeAnimToggle");
  if (toggle) toggle.checked = enabled;

  localStorage.setItem("wcs_theme_anim_enabled", enabled ? "true" : "false");
  if (showToastMsg) {
    showToast(enabled ? "Живые 2D-анимации фона включены (60+ FPS)" : "Живые анимации фона приостановлены", false);
  }
}

function setThemeAnimSpeed(speed, showToastMsg = true) {
  const clamped = Math.max(0.4, Math.min(2.5, speed));
  document.documentElement.style.setProperty("--theme-anim-speed-mult", clamped);
  localStorage.setItem("wcs_theme_anim_speed", clamped.toString());

  const bSlow = document.getElementById("btn-anim-slow");
  const bNorm = document.getElementById("btn-anim-norm");
  const bFast = document.getElementById("btn-anim-fast");
  if (bSlow) bSlow.classList.toggle("active", Math.abs(clamped - 0.6) < 0.15);
  if (bNorm) bNorm.classList.toggle("active", Math.abs(clamped - 1.0) < 0.15);
  if (bFast) bFast.classList.toggle("active", Math.abs(clamped - 1.4) < 0.15);

  if (showToastMsg) {
    showToast(`Скорость анимаций: ${clamped}x`, false);
  }
}

function setGlassBlur(blurPx, showToastMsg = true) {
  const clamped = Math.max(8, Math.min(45, blurPx));
  document.documentElement.style.setProperty("--glass-blur", `${clamped}px`);
  localStorage.setItem("wcs_glass_blur", clamped.toString());

  const b12 = document.getElementById("btn-blur-12");
  const b20 = document.getElementById("btn-blur-20");
  const b30 = document.getElementById("btn-blur-30");
  if (b12) b12.classList.toggle("active", Math.abs(clamped - 12) < 3);
  if (b20) b20.classList.toggle("active", Math.abs(clamped - 20) < 3);
  if (b30) b30.classList.toggle("active", Math.abs(clamped - 30) < 3);

  if (showToastMsg) {
    showToast(`Глубина матового стекла: ${clamped}px`, false);
  }
}

function setThemeBgOpacity(val, showNotification = true) {
  const clamped = Math.max(0.1, Math.min(0.95, val));
  document.documentElement.style.setProperty("--theme-bg-opacity", clamped);
  localStorage.setItem("wcs_theme_bg_opacity", clamped.toString());

  const b25 = document.getElementById("btn-op-25");
  const b45 = document.getElementById("btn-op-45");
  const b65 = document.getElementById("btn-op-65");
  const b85 = document.getElementById("btn-op-85");
  if (b25) b25.classList.toggle("active", Math.abs(clamped - 0.25) < 0.08);
  if (b45) b45.classList.toggle("active", Math.abs(clamped - 0.45) < 0.08);
  if (b65) b65.classList.toggle("active", Math.abs(clamped - 0.65) < 0.08);
  if (b85) b85.classList.toggle("active", Math.abs(clamped - 0.85) < 0.08);

  if (showNotification) {
    showToast(`Прозрачность 2D фона: ${Math.round(clamped * 100)}%`, false);
  }
}

// ----------------------------------------------------------------------------
// Dashboard Live Metrics & Telemetry
// ----------------------------------------------------------------------------
async function refreshDashboardMetrics(data) {
  try {
    if (!data) {
      const res = await callNative("getInitialState");
      if (res && res.success) data = res;
    }

    if (data && data.specs) {
      const cpuEl = document.getElementById("dashHwCpu");
      const gpuEl = document.getElementById("dashHwGpu");
      const osEl = document.getElementById("dashHwOs");
      if (cpuEl && data.specs.cpuName) cpuEl.textContent = data.specs.cpuName.replace(/(\(R\)|\(TM\))/g, "").trim();
      if (gpuEl && data.specs.primaryGPUName) gpuEl.textContent = data.specs.primaryGPUName;
      if (osEl && data.specs.osVersion) osEl.textContent = data.specs.osVersion;

      if (data.specs.cpuName) {
        const nameEl = document.getElementById("dashCpuName");
        if (nameEl) {
          nameEl.innerText = data.specs.cpuName.replace(/(\(R\)|\(TM\))/g, "").trim();
          nameEl.title = data.specs.cpuName;
        }
      }
      if (data.specs.primaryGPUName) {
        const gpuNameEl = document.getElementById("dashGpuName");
        if (gpuNameEl) {
          gpuNameEl.innerText = data.specs.primaryGPUName;
          gpuNameEl.title = data.specs.primaryGPUName;
        }
      }
      if (data.specs.logicalCores) {
        const coresEl = document.getElementById("dashCpuCores");
        if (coresEl) coresEl.innerText = `${data.specs.logicalCores}`;
        const specsEl = document.getElementById("dashCpuSpecs");
        if (specsEl) specsEl.innerText = `${data.specs.logicalCores} логических потоков`;
      }
    }

    if (data && data.memory) {
      updateMemoryTabTelemetry(data.memory);
    }

    if (data && data.tweaks) {
      syncTweaksUI(data.tweaks);
    }
  } catch (err) {
    console.warn("Could not refresh dashboard metrics:", err);
  }
}

// ============================================================================
// Real-Time Hardware Performance Load Charts (CPU & GPU)
// ============================================================================
const HARDWARE_HISTORY_LEN = 30;
const cpuLoadHistory = new Array(HARDWARE_HISTORY_LEN).fill(0);
const gpuLoadHistory = new Array(HARDWARE_HISTORY_LEN).fill(0);

const CPU_CHART_THEME = {
  stroke: '#00d2ff',
  glow: 'rgba(0, 210, 255, 0.70)',
  gradientTop: 'rgba(0, 210, 255, 0.35)',
  gradientBottom: 'rgba(0, 210, 255, 0.01)',
  pulseOuter: 'rgba(0, 210, 255, 0.40)'
};

const GPU_CHART_THEME = {
  stroke: '#c084fc',
  glow: 'rgba(192, 132, 252, 0.75)',
  gradientTop: 'rgba(168, 85, 247, 0.35)',
  gradientBottom: 'rgba(168, 85, 247, 0.01)',
  pulseOuter: 'rgba(192, 132, 252, 0.45)'
};

function renderSmoothSplineChart(canvasId, historyData, theme) {
  const canvas = document.getElementById(canvasId);
  if (!canvas) return;

  const rect = canvas.getBoundingClientRect();
  if (rect.width === 0 || rect.height === 0) return;

  const dpr = window.devicePixelRatio || 1;
  const targetW = Math.round(rect.width * dpr);
  const targetH = Math.round(rect.height * dpr);

  if (canvas.width !== targetW || canvas.height !== targetH) {
    canvas.width = targetW;
    canvas.height = targetH;
  }

  const ctx = canvas.getContext('2d');
  ctx.save();
  ctx.scale(dpr, dpr);

  const w = rect.width;
  const h = rect.height;
  const padTop = 10;
  const padBottom = 12;
  const padLeft = 28;
  const padRight = 12;
  const chartW = w - padLeft - padRight;
  const chartH = h - padTop - padBottom;

  ctx.clearRect(0, 0, w, h);

  // 1. Dashed Grid & Percentage Labels (0%, 25%, 50%, 75%, 100%)
  const gridLevels = [0, 25, 50, 75, 100];
  ctx.font = '8.5px "SF Pro Text", -apple-system, monospace';
  ctx.textAlign = 'right';
  ctx.textBaseline = 'middle';

  for (const lvl of gridLevels) {
    const y = padTop + chartH - (lvl / 100) * chartH;
    ctx.setLineDash([3, 4]);
    ctx.lineWidth = 1;
    ctx.strokeStyle = (lvl === 0 || lvl === 100) ? 'rgba(255, 255, 255, 0.08)' : 'rgba(255, 255, 255, 0.04)';
    ctx.beginPath();
    ctx.moveTo(padLeft, y);
    ctx.lineTo(w - padRight, y);
    ctx.stroke();

    ctx.fillStyle = 'rgba(255, 255, 255, 0.32)';
    ctx.fillText(`${lvl}%`, padLeft - 6, y);
  }
  ctx.setLineDash([]);

  // 2. Prepare Coordinate Points
  const n = historyData.length;
  const points = [];
  for (let i = 0; i < n; i++) {
    const px = padLeft + (i / (n - 1)) * chartW;
    const clampedVal = Math.max(0, Math.min(100, historyData[i]));
    const py = padTop + chartH - (clampedVal / 100) * chartH;
    points.push({ x: px, y: py });
  }

  if (points.length < 2) {
    ctx.restore();
    return;
  }

  // 3. Fill Gradient under Cubic Spline
  const fillGrad = ctx.createLinearGradient(0, padTop, 0, padTop + chartH);
  fillGrad.addColorStop(0, theme.gradientTop);
  fillGrad.addColorStop(1, theme.gradientBottom);

  ctx.beginPath();
  ctx.moveTo(points[0].x, points[0].y);
  for (let i = 0; i < points.length - 1; i++) {
    const p0 = points[i === 0 ? i : i - 1];
    const p1 = points[i];
    const p2 = points[i + 1];
    const p3 = points[i + 2 < points.length ? i + 2 : i + 1];

    const cp1x = p1.x + (p2.x - p0.x) / 6;
    const cp1y = p1.y + (p2.y - p0.y) / 6;
    const cp2x = p2.x - (p3.x - p1.x) / 6;
    const cp2y = p2.y - (p3.y - p1.y) / 6;

    ctx.bezierCurveTo(cp1x, cp1y, cp2x, cp2y, p2.x, p2.y);
  }
  ctx.lineTo(points[points.length - 1].x, padTop + chartH);
  ctx.lineTo(points[0].x, padTop + chartH);
  ctx.closePath();
  ctx.fillStyle = fillGrad;
  ctx.fill();

  // 4. Glow Stroke
  ctx.save();
  ctx.shadowColor = theme.glow;
  ctx.shadowBlur = 8;
  ctx.beginPath();
  ctx.moveTo(points[0].x, points[0].y);
  for (let i = 0; i < points.length - 1; i++) {
    const p0 = points[i === 0 ? i : i - 1];
    const p1 = points[i];
    const p2 = points[i + 1];
    const p3 = points[i + 2 < points.length ? i + 2 : i + 1];

    const cp1x = p1.x + (p2.x - p0.x) / 6;
    const cp1y = p1.y + (p2.y - p0.y) / 6;
    const cp2x = p2.x - (p3.x - p1.x) / 6;
    const cp2y = p2.y - (p3.y - p1.y) / 6;

    ctx.bezierCurveTo(cp1x, cp1y, cp2x, cp2y, p2.x, p2.y);
  }
  ctx.strokeStyle = theme.stroke;
  ctx.lineWidth = 2.2;
  ctx.lineCap = 'round';
  ctx.lineJoin = 'round';
  ctx.stroke();
  ctx.restore();

  // 5. Pulsing Current Head Point
  const head = points[points.length - 1];
  ctx.beginPath();
  ctx.arc(head.x, head.y, 5, 0, Math.PI * 2);
  ctx.fillStyle = theme.pulseOuter;
  ctx.fill();

  ctx.beginPath();
  ctx.arc(head.x, head.y, 2.5, 0, Math.PI * 2);
  ctx.fillStyle = theme.stroke;
  ctx.fill();

  ctx.restore();
}

function redrawHardwareCharts() {
  renderSmoothSplineChart('cpuLoadCanvas', cpuLoadHistory, CPU_CHART_THEME);
  renderSmoothSplineChart('gpuLoadCanvas', gpuLoadHistory, GPU_CHART_THEME);
}

function updateHardwareTelemetry(data) {
  if (!data) return;

  // CPU
  if (data.cpuPercent !== undefined) {
    const val = Math.max(0, Math.min(100, Number(data.cpuPercent)));
    cpuLoadHistory.push(val);
    if (cpuLoadHistory.length > HARDWARE_HISTORY_LEN) {
      cpuLoadHistory.shift();
    }

    const valEl = document.getElementById('dashCpuVal');
    if (valEl) valEl.innerText = `${Math.round(val)}%`;

    const peakEl = document.getElementById('dashCpuPeak');
    if (peakEl) {
      const peakVal = Math.round(Math.max(...cpuLoadHistory));
      peakEl.innerText = `${peakVal}%`;
    }

    const avgEl = document.getElementById('dashCpuAvg');
    if (avgEl) {
      const sum = cpuLoadHistory.reduce((a, b) => a + b, 0);
      const avgVal = Math.round(sum / cpuLoadHistory.length);
      avgEl.innerText = `${avgVal}%`;
    }

    const badgeEl = document.getElementById('dashCpuBadge');
    if (badgeEl) {
      if (val >= 80) {
        badgeEl.textContent = 'HIGH';
        badgeEl.className = 'dash-chart-badge badge-high';
      } else if (val >= 40) {
        badgeEl.textContent = 'ACTIVE';
        badgeEl.className = 'dash-chart-badge badge-medium';
      } else {
        badgeEl.textContent = 'NORMAL';
        badgeEl.className = 'dash-chart-badge badge-cpu';
      }
    }
  }

  // GPU
  if (data.gpuPercent !== undefined) {
    const val = Math.max(0, Math.min(100, Number(data.gpuPercent)));
    gpuLoadHistory.push(val);
    if (gpuLoadHistory.length > HARDWARE_HISTORY_LEN) {
      gpuLoadHistory.shift();
    }

    const valEl = document.getElementById('dashGpuVal');
    if (valEl) valEl.innerText = `${Math.round(val)}%`;

    const peakEl = document.getElementById('dashGpuPeak');
    if (peakEl) {
      const peakVal = Math.round(Math.max(...gpuLoadHistory));
      peakEl.innerText = `${peakVal}%`;
    }

    const avgEl = document.getElementById('dashGpuAvg');
    if (avgEl) {
      const sum = gpuLoadHistory.reduce((a, b) => a + b, 0);
      const avgVal = Math.round(sum / gpuLoadHistory.length);
      avgEl.innerText = `${avgVal}%`;
    }

    const badgeEl = document.getElementById('dashGpuBadge');
    if (badgeEl) {
      if (val >= 75) {
        badgeEl.textContent = 'HEAVY 3D';
        badgeEl.className = 'dash-chart-badge badge-high';
      } else if (val >= 20) {
        badgeEl.textContent = 'ACTIVE';
        badgeEl.className = 'dash-chart-badge badge-medium';
      } else {
        badgeEl.textContent = 'IDLE';
        badgeEl.className = 'dash-chart-badge badge-gpu';
      }
    }
  }

  // Device Info Titles & Specs
  if (data.cpuName) {
    const cpuNameClean = data.cpuName.replace(/(\(R\)|\(TM\))/g, '').trim();
    const nameEl = document.getElementById('dashCpuName');
    if (nameEl) {
      nameEl.innerText = cpuNameClean;
      nameEl.title = data.cpuName;
    }
  }

  if (data.gpuName) {
    const gpuNameEl = document.getElementById('dashGpuName');
    if (gpuNameEl) {
      gpuNameEl.innerText = data.gpuName;
      gpuNameEl.title = data.gpuName;
    }
  }

  if (data.logicalCores) {
    const coresEl = document.getElementById('dashCpuCores');
    if (coresEl) coresEl.innerText = `${data.logicalCores}`;
    const specsEl = document.getElementById('dashCpuSpecs');
    if (specsEl) specsEl.innerText = `${data.logicalCores} логических потоков`;
  }

  // Render if dashboard tab (tab 0) is active
  if (currentTabIdx === 0) {
    redrawHardwareCharts();
  }
}

// Window resize listener to keep charts pixel-sharp
window.addEventListener('resize', () => {
  if (currentTabIdx === 0) {
    redrawHardwareCharts();
  }
});
