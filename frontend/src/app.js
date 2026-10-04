(function () {
  'use strict';
  const isFileDownload = pkg => Boolean(pkg && (pkg.action === 'download' || pkg.action_type === 'download_file'));
  const isDubbedGame = game => Boolean(game && (game.localization_type === 'dubbed' ||
    (Array.isArray(game.packages) && game.packages.some(pkg => pkg.localization_type === 'dubbed'))));
  const isTurkishGame = game => Boolean(game && (game.turkish === true || isDubbedGame(game)));
  const CATEGORIES = [
    { id: 'ps5', label: 'PS5' },
    { id: 'ps4', label: 'PS4' },
    { id: 'ps2', label: 'PS2' },
    { id: 'turkish', label: 'Türkçe Oyunlar' }
  ];
  const state = { method: 'ph', games: [], category: 'turkish', searchQuery: '', catalogVersion: null, selected: null, page: 'games', info: null, shortcutReady: false };
  const $ = (selector) => document.querySelector(selector);
  const grid = $('#game-grid');
  const tabs = $('#category-tabs');
  const searchInput = $('#catalog-search-input');
  const searchClear = $('#catalog-search-clear');
  const toast = $('#toast');
  let toastTimer = 0;
  let lastCard = null;
  const debug = window.PHSTORE_DEBUG_BUILD === true;
  let catalogRetryTimer = 0;
  let searchTimer = 0;
  let previousCatalogState = '';
  let initialCatalogRefreshObserved = false;
  let installPollTimer = 0;
  let selectedInstallPackage = '';
  let installInFlight = false;
  let installStatusPending = false;
  let installStatusWake = false;
  let installSubmitting = false;
  let installStatusEpoch = 0;
  let lastInstallStatus = null;
  let lastTerminalKey = '';
  const imageCache = new Map();
  let cacheStatusPending = false;
  let installPanel = null;
  const installedStateRequests = new Map();
  let filteredGames = [];
  let renderedWindow = '';
  let virtualRowHeight = 470;
  let renderFrame = 0;
  let lastCardIndex = -1;
  let gridGames = null;
  const mountedGameCards = new Map();
  let gridMetrics = null;
  let installedRefreshTimer = 0;
  let installedRefreshEpoch = 0;
  let installedRefreshRunning = false;
  const topSpacer = document.createElement('div');
  const bottomSpacer = document.createElement('div');
  [topSpacer, bottomSpacer].forEach((node) => {
    node.className = 'virtual-spacer'; node.setAttribute('aria-hidden', 'true');
  });

  function formatSize(bytes) {
    const value = Number(bytes);
    if (!Number.isFinite(value) || value <= 0) return 'Boyut belirtilmemiş';
    const units = ['B', 'KB', 'MB', 'GB', 'TB'];
    let size = value, unit = 0;
    while (size >= 1024 && unit < units.length - 1) { size /= 1024; unit++; }
    return (unit >= 3 ? size.toFixed(1) : Math.round(size).toString()) + ' ' + units[unit];
  }

  function formatKnownSize(bytes) {
    const value = Number(bytes);
    if (!Number.isFinite(value) || value < 0) return '—';
    if (value === 0) return '0 B';
    return formatSize(value);
  }

  function safeImageUrl(value) {
    if (typeof value !== 'string' || !value || /[\u0000-\u001f"'<>]/.test(value)) return '';
    try {
      const url = new URL(value, window.location.origin);
      if (url.origin !== window.location.origin && url.protocol !== 'https:') return '';
      return imageCache.get(url.href) || url.href;
    } catch (_) { return ''; }
  }

  function normalizeGames(input) {
    if (!Array.isArray(input)) return [];
    return input.filter((game) => game && typeof game.id === 'string' && typeof game.title === 'string' &&
      ['ps5', 'ps4', 'ps2'].includes(game.platform) && Array.isArray(game.packages));
  }

  function showToast(message) {
    toast.textContent = message;
    toast.classList.add('visible');
    window.clearTimeout(toastTimer);
    toastTimer = window.setTimeout(() => toast.classList.remove('visible'), 2600);
  }

  function methodGames() {
    return state.games.filter(game => (game.catalog_source || 'ph') === state.method);
  }
  function updateMethodCaption() {
    $('#catalog-caption').textContent = methodGames().length + ' oyun · ' + (state.method === 'ph' ? 'Method1 (PH)' : 'Method2 (sp)');
  }
  function selectMethod(method) {
    closeDetails(); state.method = method;
    $('#method-ph').checked = method === 'ph'; $('#method-sp').checked = method === 'sp';
    const available = methodGames();
    if (!available.some(game => state.category === 'turkish' ? isTurkishGame(game) : game.platform === state.category)) {
      state.category = method === 'ph' && available.some(isTurkishGame) ? 'turkish' : available[0]?.platform || 'ps5';
    }
    state.searchQuery = ''; searchInput.value = ''; searchClear.hidden = true;
    filteredGames = matchingGames(); resetGridPosition(); makeCategoryTabs(); renderGames(); updateMethodCaption();
  }
  $('#method-ph').addEventListener('change', () => selectMethod('ph'));
  $('#method-sp').addEventListener('change', () => selectMethod('sp'));
  function matchingGames() {
    if (state.searchQuery) {
      return methodGames().filter((game) => {
        const title = typeof game.title === 'string' ? game.title.toLocaleLowerCase() : '';
        const titleId = typeof game.title_id === 'string' ? game.title_id.toLocaleLowerCase() : '';
        return title.includes(state.searchQuery) || titleId.includes(state.searchQuery);
      });
    }
    return methodGames().filter((game) => state.category === 'turkish' ? isTurkishGame(game) : game.platform === state.category);
  }

  function setSearch(value, immediate) {
    const next = String(value || '').trim().toLocaleLowerCase();
    window.clearTimeout(searchTimer);
    searchClear.hidden = !searchInput.value;
    if (immediate) {
      state.searchQuery = next;
      filteredGames = matchingGames();
      resetGridPosition();
      renderGames();
      return;
    }
    searchTimer = window.setTimeout(() => {
      state.searchQuery = next;
      filteredGames = matchingGames();
      resetGridPosition();
      renderGames();
    }, 140);
  }

  function makeCategoryTabs() {
    tabs.replaceChildren();
    CATEGORIES.forEach((category) => {
      const button = document.createElement('button');
      button.type = 'button';
      button.className = 'category-tab';
      button.setAttribute('role', 'tab');
      button.dataset.category = category.id;
      button.dataset.focusRow = 'categories';
      button.setAttribute('aria-selected', category.id === state.category ? 'true' : 'false');
      button.textContent = category.label;
      button.addEventListener('click', () => selectCategory(category.id, true));
      tabs.append(button);
    });
    syncTabs();
  }

  function syncTabs() {
    tabs.querySelectorAll('.category-tab').forEach((button) => {
      const selected = button.dataset.category === state.category;
      button.classList.toggle('selected', selected);
      button.setAttribute('aria-selected', selected ? 'true' : 'false');
    });
  }

  function selectCategory(id, moveFocus) {
    state.category = id;
    syncTabs();
    filteredGames = matchingGames();
    resetGridPosition();
    renderGames();
    if (moveFocus) {
      const card = grid.querySelector('.game-card');
      if (card) card.focus();
    }
  }

  function needsSourceWarning(game) {
    if (game.catalog_source === 'sp') return false;
    return game.packages.some((pkg) => {
      if (!pkg || pkg.installable === false) return false;
      // Only our known Google Drive streaming groups/routes are exempt.
      if (pkg.source_type !== 'direct_http' &&
          ['PS2Games', 'PS4Games', 'PS5Games'].includes(pkg.source_group)) return false;
      try {
        const url = new URL(pkg.download_url);
        if (!['http:', 'https:'].includes(url.protocol)) return true;
        const ownDriveStream = url.protocol === 'http:' &&
          url.hostname === '148.135.181.3' && !url.port && !url.username && !url.password &&
          /^\/(ps2|ps4|ps5)\/[^/]+$/.test(url.pathname);
        // /phstore/pkg/ is an external-source relay, not a trusted Drive stream.
        return !ownDriveStream;
      } catch (_) {
        return true;
      }
    });
  }

  function makeCard(game, index) {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'game-card';
    button.dataset.focusRow = 'games';
    button.setAttribute('aria-label', game.title + ', ' + game.platform.toUpperCase() + ', oyun ayrıntılarını aç');
    const wrap = document.createElement('span');
    wrap.className = 'cover-wrap';
    const fallback = document.createElement('span');
    fallback.className = 'cover-fallback';
    fallback.textContent = 'PH';
    fallback.setAttribute('aria-hidden', 'true');
    const cover = document.createElement('img');
    cover.alt = game.title + ' kapak görseli';
    cover.loading = 'lazy';
    cover.decoding = 'async';
    const source = safeImageUrl(game.cover);
    if (source) {
      cover.src = source;
      cover.addEventListener('load', () => { fallback.hidden = true; }, { once: true });
      cover.addEventListener('error', () => {
        if (cover.src.includes('/resimler/') && game.cover) {
          imageCache.delete(new URL(game.cover, window.location.origin).href);
          cover.src = safeImageUrl(game.cover);
        } else { cover.hidden = true; fallback.hidden = false; }
      });
    } else {
      cover.hidden = true;
      fallback.hidden = false;
    }
    fallback.hidden = Boolean(source);
    const gradient = document.createElement('span');
    gradient.className = 'cover-gradient';
    gradient.setAttribute('aria-hidden', 'true');
    const installedBadge = document.createElement('span');
    installedBadge.className = 'installed-badge';
    installedBadge.textContent = 'Zaten kurulu';
    installedBadge.hidden = true;
    installedBadge.setAttribute('aria-hidden', 'true');
    wrap.append(fallback, cover, gradient, installedBadge);
    const meta = document.createElement('span');
    meta.className = 'game-meta';
    const sourceNote = document.createElement('span');
    sourceNote.className = 'source-warning';
    if (needsSourceWarning(game)) {
      sourceNote.textContent = 'Unstable/Slow Source';
      button.setAttribute('aria-label', button.getAttribute('aria-label') + ', Unstable/Slow Source');
    } else {
      sourceNote.setAttribute('aria-hidden', 'true');
    }
    meta.append(sourceNote);
    const title = document.createElement('span');
    title.className = 'game-title';
    title.textContent = game.title;
    const tags = document.createElement('span');
    tags.className = 'game-tags';
    const platform = document.createElement('span');
    platform.className = 'platform-tag';
    platform.textContent = game.platform.toUpperCase();
    const packageInfo = game.packages[0] || {};
    const size = document.createElement('span');
    size.textContent = formatSize(packageInfo.size_bytes || game.size_bytes);
    tags.append(platform, size);
    if (isDubbedGame(game)) {
      const ribbon = document.createElement('span');
      ribbon.className = 'dubbed-ribbon';
      ribbon.textContent = 'DUBLAJ';
      ribbon.setAttribute('aria-label', 'Türkçe dublaj');
      wrap.append(ribbon);
    } else if (game.turkish === true) {
      const language = document.createElement('span');
      language.className = 'turkish-tag';
      language.textContent = 'TR';
      language.title = 'Türkçe içerik';
      tags.append(language);
    }
    meta.append(title, tags);
    const secondary = [game.region, game.version].filter(Boolean).join(' · ');
    const extra = document.createElement('span');
    extra.className = 'game-secondary';
    extra.textContent = secondary;
    meta.append(extra);
    button.append(wrap, meta);
    button.addEventListener('click', () => openDetails(game, button));
    button.dataset.index = String(index);
    if (game.installed === true) {
      installedBadge.hidden = false;
      if (game.installed_version) installedBadge.title = 'Yüklü sürüm: ' + game.installed_version;
    }
    return button;
  }

  function renderGames() {
    const games = filteredGames;
    if (gridGames !== games) {
      gridGames = games; mountedGameCards.clear();
      grid.replaceChildren(topSpacer, bottomSpacer); gridMetrics = null;
      renderedWindow = '';
    }
    if (!gridMetrics) {
      const style = getComputedStyle(grid);
      gridMetrics = {
        columns: window.PHStoreVirtualGrid.columnsFromGridTemplate(style.gridTemplateColumns),
        rowGap: parseFloat(style.rowGap) || 0,
        top: grid.getBoundingClientRect().top + window.scrollY
      };
    }
    const columns = gridMetrics.columns;
    const rowGap = gridMetrics.rowGap;
    const gridTop = grid.getBoundingClientRect().top + window.scrollY;
    const scrollTop = Math.max(0, window.scrollY - gridTop);
    let range = window.PHStoreVirtualGrid.windowFor(games.length, columns, virtualRowHeight,
      scrollTop, window.innerHeight, 2);
    const windowKey = [range.firstIndex, range.endIndex, columns, games.length].join(':');
    if (windowKey === renderedWindow) return;
    const focused = document.activeElement && document.activeElement.classList &&
      document.activeElement.classList.contains('game-card') ? Number(document.activeElement.dataset.index) : -1;
    const empty = $('#empty-state');
    empty.hidden = games.length > 0;
    empty.querySelector('strong').textContent = state.searchQuery ? 'Sonuç bulunamadı' : 'Bu kategoride oyun yok';
    empty.querySelector('p').textContent = state.searchQuery ?
      'Oyun adı veya Title ID ile tekrar deneyin.' : 'Katalog güncellendiğinde oyunlar burada görünecek.';
    const summary = $('#search-summary');
    summary.hidden = !state.searchQuery;
    summary.textContent = state.searchQuery ? 'Arama Sonuçları · ' + games.length + ' sonuç' : '';
    topSpacer.hidden = range.top <= 0;
    topSpacer.style.height = Math.max(0, range.top - rowGap) + 'px';
    bottomSpacer.hidden = range.bottom <= 0;
    bottomSpacer.style.height = Math.max(0, range.bottom - rowGap) + 'px';
    for (const [index, card] of mountedGameCards) {
      if (index < range.firstIndex || index >= range.endIndex) {
        card.remove(); mountedGameCards.delete(index);
      }
    }
    // Keep overlapping cards (including decoded covers and focus) in place.
    let cursor = topSpacer.nextSibling;
    for (let index = range.firstIndex; index < range.endIndex; index++) {
      let card = mountedGameCards.get(index);
      if (!card) { card = makeCard(games[index], index); mountedGameCards.set(index, card); }
      if (card !== cursor) grid.insertBefore(card, cursor || bottomSpacer);
      cursor = card.nextSibling;
    }
    scheduleInstalledBadges();
    renderedWindow = windowKey;
    const mountedCards = grid.querySelectorAll('.game-card').length;
    grid.dataset.totalGames = String(games.length);
    grid.dataset.renderedCards = String(mountedCards);
    window.PHSTORE_PERF = {
      totalFilteredGames: games.length,
      mountedCards,
      windowStart: range.firstIndex,
      windowEnd: range.endIndex,
      columns,
      rowHeight: virtualRowHeight
    };
    const card = grid.querySelector('.game-card');
    if (card) {
      const measured = card.getBoundingClientRect().height + rowGap;
      if (Math.abs(measured - virtualRowHeight) > 2) {
        virtualRowHeight = measured;
        renderedWindow = '';
        renderGames();
        return;
      }
    }
    if (focused >= range.firstIndex && focused < range.endIndex) {
      const focusTarget = grid.querySelector('[data-index="' + focused + '"]');
      if (focusTarget) focusTarget.focus({ preventScroll: true });
    }
  }

  function resetGridPosition() {
    renderedWindow = '';
    const top = grid.getBoundingClientRect().top + window.scrollY;
    if (Math.abs(window.scrollY - top) > 2) window.scrollTo(0, Math.max(0, top - 12));
  }

  function focusVirtualGame(index) {
    if (index < 0 || index >= filteredGames.length) return false;
    const columns = window.PHStoreVirtualGrid.columnsFromGridTemplate(getComputedStyle(grid).gridTemplateColumns);
    const rowTop = grid.getBoundingClientRect().top + window.scrollY + Math.floor(index / columns) * virtualRowHeight;
    if (rowTop < window.scrollY || rowTop + virtualRowHeight > window.scrollY + window.innerHeight)
      window.scrollTo(0, Math.max(0, rowTop - (window.innerHeight - virtualRowHeight) / 2));
    renderedWindow = '';
    renderGames();
    const target = grid.querySelector('[data-index="' + index + '"]');
    if (target) target.focus({ preventScroll: true });
    return Boolean(target);
  }

  function moveVirtualCard(direction) {
    const active = document.activeElement;
    if (!active || !active.classList || !active.classList.contains('game-card')) return false;
    const columns = window.PHStoreVirtualGrid.columnsFromGridTemplate(getComputedStyle(grid).gridTemplateColumns);
    const target = window.PHStoreVirtualGrid.adjacentIndex(Number(active.dataset.index), direction,
      columns, filteredGames.length);
    if (target < 0) return false;
    return focusVirtualGame(target);
  }

  function openDetails(game, card) {
    lastCard = card;
    lastCardIndex = Number(card && card.dataset.index);
    state.selected = game;
    $('#games-page').hidden = true;
    $('#settings-page').hidden = true;
    $('#downloads-page').hidden = true;
    const detail = $('#detail-page');
    detail.hidden = false;
    $('#detail-title').textContent = game.title;
    let sourceWarning = $('#detail-source-warning');
    if (!sourceWarning) {
      sourceWarning = document.createElement('p');
      sourceWarning.id = 'detail-source-warning';
      sourceWarning.className = 'detail-source-warning';
      const label = document.createElement('strong');
      label.textContent = 'Unstable/Slow Source';
      const explanation = document.createElement('span');
      explanation.textContent = 'Bu kaynaktan indirmeler daha yavaş olabilir ve indirme sırasında sorun yaşanabilir.';
      sourceWarning.append(label, explanation);
      $('#detail-title').insertAdjacentElement('afterend', sourceWarning);
    }
    sourceWarning.hidden = !needsSourceWarning(game);
    $('#detail-platform').textContent = game.platform.toUpperCase();
    const localization = isDubbedGame(game) ? 'Türkçe Dublaj' :
      game.localization_type === 'text' ? 'Türkçe Metin' :
      game.localization_type === 'interface' ? 'Türkçe Arayüz' :
      game.turkish ? 'Türkçe' : 'Türkçe desteği yok';
    const localizationNode = $('#detail-localization');
    localizationNode.textContent = localization;
    localizationNode.classList.toggle('dubbed', isDubbedGame(game));
    const description = $('#detail-description');
    description.textContent = game.description || '';
    description.hidden = !game.description;
    const cover = $('#detail-cover');
    const coverUrl = safeImageUrl(game.cover);
    cover.hidden = !coverUrl;
    cover.src = coverUrl;
    cover.alt = game.title + ' kapak görseli';
    cover.onerror = () => {
      if (cover.src.includes('/resimler/') && game.cover) {
        imageCache.delete(new URL(game.cover, window.location.origin).href);
        cover.src = safeImageUrl(game.cover);
      } else cover.hidden = true;
    };
    const backdropUrl = safeImageUrl(game.background || game.cover);
    $('#detail-backdrop').style.backgroundImage = backdropUrl ? 'url("' + backdropUrl + '")' : 'none';
    const packages = $('#detail-packages');
    packages.replaceChildren();
    game.packages.forEach((item) => {
      const chip = document.createElement('div');
      chip.className = 'package-chip';
      const name = document.createElement('strong');
      name.textContent = item.type === 'base' ? 'Ana oyun' : item.type.toUpperCase();
      const info = document.createElement('span');
      const packageParts = [];
      if (item.version) packageParts.push('Sürüm ' + item.version);
      if (Number(item.size_bytes) > 0) packageParts.push(formatSize(item.size_bytes));
      if (item.source_group) packageParts.push(item.source_group);
      if (item.installable === false) packageParts.push('Yalnızca katalog kaydı');
      info.textContent = packageParts.join(' · ');
      chip.append(name, info);
      packages.append(chip);
    });
    const fields = $('#detail-fields');
    fields.replaceChildren();
    const detailFields = [
      ['Title ID', game.title_id], ['Bölge', game.region], ['Sürüm', game.version],
      ['Yayın', game.release || game.release_year], ['Minimum firmware', game.min_fw],
      ['Boyut', game.size_bytes ? formatSize(game.size_bytes) : ''],
      ['Geliştirici', game.developer], ['Yayıncı', game.publisher]
    ];
    detailFields.forEach(([label, value]) => {
      if (value === undefined || value === null || value === '') return;
      const row = document.createElement('div');
      const term = document.createElement('dt'); term.textContent = label;
      const descriptionNode = document.createElement('dd'); descriptionNode.textContent = String(value);
      row.append(term, descriptionNode); fields.append(row);
    });
    fields.hidden = fields.childElementCount === 0;
    const installablePackages = game.packages.filter((pkg) => pkg && pkg.installable !== false && typeof pkg.id === 'string');
    let selectedPackage = installablePackages.find((pkg) => pkg.type === 'base') || installablePackages[0];
    state.detailPackageId = selectedPackage ? selectedPackage.id : '';
    const installable = Boolean(selectedPackage);
    const installButton = $('#install-button');
    let diskDownload = Boolean(selectedPackage && isFileDownload(selectedPackage));
    installButton.disabled = !installable || Boolean(!diskDownload && game.title_id && typeof game.installed !== 'boolean');
    installButton.textContent = diskDownload ? (selectedPackage.action_type==='download_file'?'İNDİR':'İNDİR / DEVAM ET') : installable ? 'YÜKLE' : 'SADECE BİLGİ';
    $('#install-status').textContent = installable ? 'Kurulacak paket: ' + selectedPackage.filename + ' · ' + formatSize(selectedPackage.size_bytes) :
      'Bu oyun katalog bilgisi olarak listeleniyor; indirilebilir paket sunulmuyor.';
    installButton.onclick = () => installCatalogPackage(selectedPackage);
    if (installablePackages.length > 1) {
      const selector = document.createElement('select'); selector.className = 'package-selector';
      selector.setAttribute('aria-label','İndirilecek paket veya backport');selector.dataset.focusRow='detail';
      installablePackages.forEach(pkg => { const option=document.createElement('option');option.value=pkg.id;
        option.textContent=(pkg.variant_label || pkg.type.toUpperCase())+' · '+pkg.filename;selector.append(option); });
      selector.value=selectedPackage.id;
      selector.addEventListener('change',()=>{
        selectedPackage=installablePackages.find(pkg=>pkg.id===selector.value);
        state.detailPackageId=selectedPackage.id;
        diskDownload=isFileDownload(selectedPackage);
        installButton.disabled=installInFlight || spectrumActive;
        installButton.textContent=diskDownload?(selectedPackage.action_type==='download_file'?'İNDİR':'İNDİR / DEVAM ET'):'YÜKLE';
        $('#install-status').textContent=selectedPackage.filename+' · '+formatSize(selectedPackage.size_bytes);
      });
      packages.append(selector);
    }
    $('#installed-version-note').textContent = '';
    if (!diskDownload && (game.title_id || selectedPackage)) {
      installButton.disabled = true;
      installButton.textContent = 'KONTROL EDİLİYOR…';
      queryInstalledState(game);
    } else if (!diskDownload && typeof game.installed === 'boolean') applyInstalledState(game, game.installed, game.installed_version || '');
    $('#back-button').focus();
  }

  async function queryInstalledState(game) {
    try {
      const result = await loadInstalledState(game.title_id, true, installedPackageId(game));
      if (state.selected !== game || (game.title_id && result.title_id !== game.title_id)) return;
      game.title_id = result.title_id;
      game.installed = result.is_installed === true;
      game.installed_version = result.installed_version || '';
      game.installed_checked_at = Date.now();
      applyInstalledState(game, game.installed, game.installed_version);
    } catch (_) {
      if (state.selected === game) {
        const button = $('#install-button');
        button.disabled = true;
        button.textContent = 'DURUM BİLİNMİYOR';
        $('#installed-version-note').textContent = 'Yüklü oyun durumu kontrol edilemedi; tekrar açarak yeniden deneyin.';
      }
    }
  }

  function installedPackageId(game) {
    const pkg = game.packages.find((item) => item.type === 'base' && item.installable !== false) ||
      game.packages.find((item) => item.installable !== false);
    return pkg ? pkg.id : '';
  }

  function loadInstalledState(titleId, force, packageId) {
    const key = titleId || 'package:' + packageId;
    const cached = installedStateRequests.get(key);
    if (cached && (!cached.finished || (!force && Date.now() - cached.time < 30000))) return cached.promise;
    const entry = { finished: false, time: Date.now(), promise: null };
    entry.promise = fetch('/api/store/installed?' + (titleId ? 'title_id=' + encodeURIComponent(titleId) : 'package_id=' + encodeURIComponent(packageId)), { cache: 'no-store' })
      .then(async (response) => {
        const result = await response.json();
        if (!response.ok || result.ok !== true || !result.title_id || (titleId && result.title_id !== titleId))
          throw new Error(result.error || 'installed_state_unavailable');
        entry.finished = true; entry.time = Date.now();
        return result;
      }).catch((error) => {
        if (installedStateRequests.get(key) === entry) installedStateRequests.delete(key);
        throw error;
      });
    installedStateRequests.set(key, entry);
    return entry.promise;
  }

  function scheduleInstalledBadges() {
    installedRefreshEpoch++;
    window.clearTimeout(installedRefreshTimer);
    installedRefreshTimer = window.setTimeout(refreshVisibleInstalled, 180);
  }

  async function refreshVisibleInstalled() {
    if (installedRefreshRunning || installInFlight || state.page !== 'games' || state.selected || document.hidden) return;
    installedRefreshRunning = true;
    const epoch = installedRefreshEpoch;
    try {
      // One database request at a time, after scrolling has settled.
      for (const [index, card] of mountedGameCards) {
        if (epoch !== installedRefreshEpoch || state.selected) break;
        const game = filteredGames[index];
        if (!game || !card.isConnected || (!game.title_id && !installedPackageId(game))) continue;
        if (game.packages.length && game.packages.every(pkg=>isFileDownload(pkg))) continue;
        if (Date.now() - (game.installed_checked_at || 0) < 30000) continue;
        try {
          const result = await loadInstalledState(game.title_id, false, installedPackageId(game));
          game.title_id = result.title_id;
          game.installed = result.is_installed === true;
          game.installed_version = result.installed_version || '';
          game.installed_checked_at = Date.now();
          const badge = card.querySelector('.installed-badge');
          badge.hidden = !game.installed;
          badge.title = game.installed_version ? 'Yüklü sürüm: ' + game.installed_version : 'Bu oyun konsolda yüklü.';
        } catch (_) { /* Detection failure remains unknown and can be retried. */ }
      }
    } finally {
      installedRefreshRunning = false;
      if (epoch !== installedRefreshEpoch) scheduleInstalledBadges();
    }
  }

  function markPackageInstalled(packageId) {
    for (const game of state.games) {
      if (!game.packages.some((pkg) => pkg.id === packageId)) continue;
      game.installed = true; game.installed_checked_at = Date.now();
      if (game.title_id) installedStateRequests.delete(game.title_id);
      installedStateRequests.delete('package:' + packageId);
      if (state.selected === game) applyInstalledState(game, true, game.installed_version || '');
    }
    for (const [index, card] of mountedGameCards) {
      if (filteredGames[index] && filteredGames[index].installed === true)
        card.querySelector('.installed-badge').hidden = false;
    }
  }

  function applyInstalledState(game, installed, version) {
    const installable = game.packages.some((pkg) => pkg && pkg.installable !== false && typeof pkg.id === 'string');
    const button = $('#install-button');
    const selected = game.packages.find(pkg=>pkg.id===state.detailPackageId);
    if (selected && isFileDownload(selected)) {
      button.disabled = installInFlight || spectrumActive || selected.installable===false;
      button.textContent=selected.action_type==='download_file'?'İNDİR':'İNDİR / DEVAM ET';$('#installed-version-note').textContent='';return;
    }
    if (installInFlight) { button.disabled = true; button.textContent = 'KURULUM SÜRÜYOR'; return; }
    if (installed) {
      button.disabled = true;
      button.textContent = 'Zaten kurulu';
      $('#installed-version-note').textContent = version ? 'Konsolda yüklü sürüm: ' + version : 'Bu oyun konsolda yüklü.';
    } else {
      button.disabled = !installable;
      button.textContent = installable ? 'YÜKLE' : 'SADECE BİLGİ';
      $('#installed-version-note').textContent = '';
    }
  }

  async function installCatalogPackage(pkg) {
    if (!pkg || typeof pkg.id !== 'string' || installInFlight || spectrumActive) return;
    if (isFileDownload(pkg)) { await startSpectrumDownload(pkg); return; }
    installInFlight = true;
    installSubmitting = true;
    installStatusEpoch++;
    const button = $('#install-button');
    const status = $('#install-status');
    button.disabled = true;
    status.textContent = 'Kurulum başlatılıyor…';
    openInstallPanel(pkg);
    try {
      const response = await fetch('/api/store/install', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ package_id: pkg.id }), cache: 'no-store'
      });
      const result = await response.json();
      if (!response.ok || result.ok !== true) throw new Error(result.error || 'install_start_failed');
      selectedInstallPackage = pkg.id;
      installSubmitting = false;
      status.textContent = 'Kurulum isteği sıraya alındı…';
      window.clearTimeout(installPollTimer);
      await pollInstallStatus();
    } catch (error) {
      const message = 'Kurulum başlatılamadı: ' + (error.message || 'hata');
      status.textContent = message;
      const panel = ensureInstallPanel();
      panel.querySelector('[data-install-phase-label]').textContent = 'İstek reddedildi';
      panel.querySelector('[data-install-error]').textContent = message;
      panel.querySelector('[data-install-cancel]').disabled = true;
      selectedInstallPackage = '';
      installInFlight = false;
      window.clearTimeout(installPollTimer);
      showToast(message);
      button.disabled = false;
    } finally {
      installSubmitting = false;
      // A lost POST response does not mean the daemon rejected the install.
      pollInstallStatus();
    }
  }

  const downloadHistory = new Map();
  try { for (const item of JSON.parse(localStorage.getItem('phstore2-download-history') || '[]')) {
    if(item && typeof item.package_id === 'string' && typeof item.state === 'string' && ['completed','deleted','failed'].includes(item.state)) downloadHistory.set(item.package_id,item);
  }} catch(_) {}
  let currentDownloadId = '';
  function rememberDownload(result, installed) {
    if(!result || !result.package_id || result.state === 'idle') return;
    const identity = installed ? installIdentity(result) : null;
    const item = {package_id:result.package_id,filename:result.filename || (identity && (identity.game.title || identity.pkg.filename)) || result.package_id,state:result.state,updated_at:Date.now()};
    const previous=downloadHistory.get(item.package_id);
    downloadHistory.set(item.package_id,item);
    if(!previous || previous.state!==item.state) {
      try { localStorage.setItem('phstore2-download-history',JSON.stringify(Array.from(downloadHistory.values()).filter(x=>['completed','deleted','failed'].includes(x.state)||x.state.startsWith('error:')).slice(-50))); } catch(_) {}
    }
    renderDownloadHistory();
  }
  function renderDownloadHistory() {
    const list=$('#downloads-history');list.textContent='';let cards=0;
    for(const item of Array.from(downloadHistory.values()).reverse()) {
      if(!['completed','deleted','failed'].includes(item.state) && !item.state.startsWith('error:'))continue;
      const card=document.createElement('section');card.className='settings-card download-history-card';
      const name=document.createElement('strong');name.textContent=item.filename;
      const label=document.createElement('p');label.textContent=item.state==='completed'?'İndirme tamamlandı':item.state==='deleted'?'İptal edildi; indirilen veriler silindi':('Hata: '+item.state);
      card.append(name,label);list.append(card);cards++;
    }
    $('#downloads-empty').hidden = cards>0 || spectrumActive || installInFlight || Boolean(currentDownloadId);
  }
  let spectrumActive = false;
  let nativeDriveActive = false;
  async function startSpectrumDownload(pkg) {
    try {
      const response = await fetch('/api/store/download', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({package_id:pkg.id})});
      const result = await response.json();
      if (!response.ok || !result.ok) throw new Error(result.error || 'download_start_failed');
      spectrumActive = true; await pollSpectrum();
      const homebrew = /\.(exfat|ffpfsc|ffpkg|ffpfs)$/i.test(pkg.filename || '');
      showToast('İndirme başlatıldı; hedef ' + (homebrew ? '/data/homebrew' : '/data/phstore2/gdrive'));
    } catch(error) { showToast('İndirme: ' + error.message); }
  }
  async function pollSpectrum() {
    try {
      const response = await fetch('/api/store/spectrum/status', {cache:'no-store'});
      if (!response.ok) return;
      const result = await response.json(); spectrumActive = result.active === true;
      currentDownloadId=result.package_id || '';rememberDownload(result,false);
      nativeDriveActive = result.native_gdrive === true;
      if(state.selected && state.selected.packages.some(pkg=>pkg.id===state.detailPackageId && isFileDownload(pkg))) {
        applyInstalledState(state.selected,false,'');
      }
      $('#spectrum-download').hidden = !result.package_id || ['completed','deleted'].includes(result.state);
      $('#spectrum-download-name').textContent = result.filename || '';
      const labels = {paused:'Duraklatıldı', deleted:'İptal edildi ve silindi', idle:'Hazır', starting:'Başlatılıyor', manifest:'Manifest alınıyor', downloading:'İndiriliyor', verifying:'Dosya doğrulanıyor', completed:'Tamamlandı', cancelled:nativeDriveActive?'İptal edildi':'Duraklatıldı', 'error:gdrive_tls_trust':'Google Drive sertifikası doğrulanamadı', 'error:sha256_mismatch':'Dosya doğrulaması başarısız', helper_launch_unconfirmed:'Helper başlangıcı doğrulanamadı'};
      $('#spectrum-download-state').textContent = (labels[result.state] || result.state || '') + ' · ' + formatSize(result.downloaded_bytes) + ' / ' + formatSize(result.total_bytes) + ' · ' + formatSize(result.speed_bytes) + '/sn';
      $('#spectrum-download-progress').value = Number(result.percent) || 0;
      const paused=result.state==='paused' || result.state==='cancelled';
      $('#spectrum-pause').disabled = !spectrumActive || paused;
      $('#spectrum-resume').disabled = !paused || (spectrumActive && !nativeDriveActive);
      $('#spectrum-cancel').disabled = ['completed','deleted'].includes(result.state);
      const remaining=Math.max(0,Number(result.total_bytes)-Number(result.downloaded_bytes));
      $('#spectrum-download-eta').textContent='Tahmini kalan süre: '+(paused?'Duraklatıldı':result.state==='completed'?'0 sn':Number(result.speed_bytes)>0?formatDuration(remaining/Number(result.speed_bytes)):'Hesaplanıyor…');
    } catch(_) { /* A transport timeout does not imply the helper stopped. */ }
  }
  async function downloadControl(action) {
    try {
      const r=await fetch('/api/store/spectrum/'+action,{method:'POST'});
      const result=await r.json();if(!r.ok || !result.ok)throw new Error(result.error || 'download_control_failed');
      showToast(action==='pause'?'Duraklatma istendi.':action==='resume'?'İndirme devam ediyor.':'İptal ve silme istendi.');
      await pollSpectrum();
    } catch(e){showToast(e.message);}
  }
  $('#spectrum-pause').addEventListener('click',()=>downloadControl('pause'));
  $('#spectrum-resume').addEventListener('click',()=>downloadControl('resume'));
  $('#spectrum-cancel').addEventListener('click',()=>downloadControl('cancel'));
  window.setInterval(() => { if (!document.hidden) pollSpectrum(); }, 1500);
  pollSpectrum();
  function terminalInstall(result) {
    return ['completed', 'failed', 'cancelled', 'diagnostic_probe_ok', 'diagnostic_probe_failed',
      'diagnostic_relay_ok', 'diagnostic_relay_failed', 'diagnostic_helper_ok', 'diagnostic_helper_failed'].includes(result.state);
  }

  function installIdentity(result) {
    const game = state.games.find((item) => item.packages.some((pkg) => pkg.id === result.package_id));
    const pkg = game && game.packages.find((item) => item.id === result.package_id);
    return { game: game || {}, pkg: pkg || { id: result.package_id } };
  }

  function updateInstallIdentity(result) {
    const panel = ensureInstallPanel();
    const { game, pkg } = installIdentity(result);
    panel.querySelector('[data-install-title]').textContent = game.title || result.package_id || 'PH Store paketi';
    panel.querySelector('[data-install-file]').textContent = pkg.filename || pkg.id || '';
    const cover = panel.querySelector('[data-install-cover]');
    const url = safeImageUrl(game.cover);
    cover.hidden = !url;
    cover.alt = game.title || '';
    if (url && cover.src !== url) cover.src = url;
    panel.querySelector('[data-install-identifiers]').textContent =
      (game.title_id ? 'Title ID ' + game.title_id : '') +
      (result.install_returned_content_id ? ' · Content ID ' + result.install_returned_content_id : '');
  }

  function renderActiveDownload(stale) {
    const bar = $('#active-download');
    const wasHidden = bar.hidden;
    const result = lastInstallStatus;
    rememberDownload(result,true);
    bar.hidden = state.page !== 'downloads' || !result || !result.package_id || result.state === 'idle' || terminalInstall(result);
    if (!bar.hidden) {
      const { game, pkg } = installIdentity(result);
      const percent = Math.max(0, Math.min(100, Number(result.progress_percent) || 0));
      $('#active-download-title').textContent = game.title || pkg.filename || result.package_id;
      $('#active-download-fill').style.width = percent.toFixed(2) + '%';
      $('#active-download-track').setAttribute('aria-valuenow', percent.toFixed(1));
      $('#active-download-percent').textContent = percent.toFixed(1) + '%';
      $('#active-download-bytes').textContent = formatKnownSize(result.appinst_downloaded_bytes || result.downloaded_bytes || 0) + ' / ' + formatKnownSize(result.appinst_total_bytes || result.total_bytes || 0);
      $('#active-download-speed').textContent = stale ? 'Hız: —' : 'Hız: ' + formatSpeed(result.current_speed_bps);
      $('#active-download-eta').textContent = stale ? '' : 'Kalan: ' + (result.eta_seconds == null ? 'Hesaplanıyor' : formatDuration(result.eta_seconds));
      $('#active-download-phase').textContent = stale ? 'Bağlantı bekleniyor · son alınan ilerleme gösteriliyor.' :
        result.state === 'starting' ? 'Kurulum hazırlanıyor…' : result.state === 'cancelling' ? 'İptal ediliyor…' :
        result.phase === 'promoting' || percent >= 100 ? 'Kurulum tamamlanıyor…' : 'İndirme ve kurulum sürüyor';
    }
    if (wasHidden !== bar.hidden) {
      gridMetrics = null; renderedWindow = '';
      if (!state.selected && state.page === 'games') renderGames();
    }
  }

  async function pollInstallStatus() {
    window.clearTimeout(installPollTimer);
    if (document.hidden) return;
    if (installStatusPending) { installStatusWake = true; return; }
    if (installSubmitting) { installPollTimer = window.setTimeout(pollInstallStatus, 1000); return; }
    installStatusPending = true;
    const epoch = installStatusEpoch;
    const controller = new AbortController();
    const timeout = window.setTimeout(() => controller.abort(), 6000);
    try {
      const response = await fetch('/api/store/install/status', { cache: 'no-store', signal: controller.signal });
      const result = await response.json();
      if (!response.ok || result.ok !== true || typeof result.state !== 'string') throw new Error('status_unavailable');
      if (epoch !== installStatusEpoch || installSubmitting) return;
      lastInstallStatus = result;
      const terminal = terminalInstall(result);
      installInFlight = Boolean(result.package_id && result.state !== 'idle' && !terminal);
      selectedInstallPackage = result.package_id || '';
      if (result.package_id && result.state !== 'idle') {
        updateInstallIdentity(result);
        renderInstallPanel(result);
        const key = result.package_id + ':' + result.generation + ':' + result.state;
        if (terminal && key !== lastTerminalKey) {
          lastTerminalKey = key;
          if (result.state === 'completed') markPackageInstalled(result.package_id);
        }
        if (state.selected) {
          const isSelected = state.selected.packages.some((pkg) => pkg.id === result.package_id);
          if (isSelected) $('#install-status').textContent = terminal ?
            (result.state === 'completed' ? 'Kurulum tamamlandı.' : result.state === 'cancelled' ? 'Kurulum iptal edildi.' : 'Kurulum sonucu: ' + (result.error || result.state)) :
            'Kurulum sürüyor · ' + (Number(result.progress_percent) || 0).toFixed(1) + '%';
          if (installInFlight) $('#install-button').disabled = true;
          else if (typeof state.selected.installed === 'boolean') applyInstalledState(state.selected, state.selected.installed, state.selected.installed_version || '');
        }
      } else {
        if (installPanel) installPanel.hidden = true;
        if (state.selected && typeof state.selected.installed === 'boolean') applyInstalledState(state.selected, state.selected.installed, state.selected.installed_version || '');
      }
      renderActiveDownload(false);
    } catch (_) {
      renderActiveDownload(true);
      if (installPanel && !installPanel.hidden) {
        installPanel.querySelector('[data-install-phase-label]').textContent = 'Bağlantı bekleniyor · son alınan ilerleme gösteriliyor.';
        installPanel.querySelector('[data-install-speed]').textContent = '—';
      }
    } finally {
      window.clearTimeout(timeout);
      installStatusPending = false;
      const delay = installStatusWake ? 0 : installInFlight ? 1000 : 5000;
      installStatusWake = false;
      if (!document.hidden) installPollTimer = window.setTimeout(pollInstallStatus, delay);
    }
  }

  function reopenActiveDownload() {
    if (!lastInstallStatus || !installInFlight) { pollInstallStatus(); return; }
    updateInstallIdentity(lastInstallStatus);
    renderInstallPanel(lastInstallStatus);
    installPanel.hidden = false;
    installPanel.querySelector('.install-panel-close').focus();
  }

  function ensureInstallPanel() {
    if (installPanel) return installPanel;
    const overlay = document.createElement('section');
    overlay.className = 'install-overlay';
    overlay.hidden = true;
    overlay.setAttribute('role', 'dialog');
    overlay.setAttribute('aria-modal', 'true');
    overlay.setAttribute('aria-label', 'Paket kurulumu');
    overlay.innerHTML = '<div class="install-panel"><button class="install-panel-close" type="button" aria-label="Paneli kapat">×</button>' +
      '<p class="eyebrow">PH STORE · KURULUM</p><div class="install-identity"><img data-install-cover alt=""><div><h2 data-install-title></h2>' +
      '<p data-install-file></p><p class="install-identifiers" data-install-identifiers></p></div></div>' +
      '<p class="install-phase" data-install-phase><span class="install-status-chip" data-install-chip>HAZIRLANIYOR</span><span data-install-phase-label>PS5 yükleyicisi hazırlanıyor…</span></p>' +
      '<div class="install-progress-track"><div class="install-progress-fill" data-install-fill></div></div>' +
      '<div class="install-progress-head"><strong data-install-percent>0.0%</strong><span data-install-bytes>0 B / 0 B</span></div>' +
      '<dl class="install-metrics"><div><dt>Anlık hız</dt><dd data-install-speed>—</dd></div><div><dt>Ortalama</dt><dd data-install-average>—</dd></div>' +
      '<div><dt>Tahmini süre</dt><dd data-install-eta>—</dd></div><div><dt>Geçen süre</dt><dd data-install-elapsed>00:00</dd></div>' +
      '<div><dt>Relay’den sunulan</dt><dd data-install-served>0 B</dd></div><div><dt>Upstream alınan</dt><dd data-install-upstream>0 B</dd></div></dl>' +
      '<p class="install-source" data-install-source></p><p class="install-error" data-install-error></p>' +
      '<button class="install-cancel-button" data-install-cancel type="button">Kurulumu İptal Et</button></div>';
    document.body.append(overlay);
    overlay.querySelector('.install-panel-close').addEventListener('click', () => {
      overlay.hidden = true;
      if (!$('#active-download').hidden) $('#active-download-open').focus();
    });
    overlay.querySelector('[data-install-cancel]').addEventListener('click', async () => {
      const button = overlay.querySelector('[data-install-cancel]');
      button.disabled = true;
      overlay.querySelector('[data-install-phase-label]').textContent = 'İptal isteği gönderiliyor…';
      try { await fetch('/api/store/install/cancel', { method: 'POST', cache: 'no-store' }); }
      catch (_) { overlay.querySelector('[data-install-phase-label]').textContent = 'İptal durumu bekleniyor…'; }
    });
    installPanel = overlay;
    return overlay;
  }

  function openInstallPanel(pkg) {
    const panel = ensureInstallPanel();
    panel.hidden = false;
    panel.querySelector('[data-install-title]').textContent = state.selected && state.selected.title || 'PH Store paketi';
    panel.querySelector('[data-install-file]').textContent = pkg.filename || pkg.id;
    const game = state.selected || {};
    const cover = panel.querySelector('[data-install-cover]');
    const coverUrl = safeImageUrl(game.cover);
    cover.hidden = !coverUrl;
    cover.alt = coverUrl ? (game.title || 'PH Store') + ' kapak görseli' : '';
    if (coverUrl) cover.src = coverUrl;
    panel.querySelector('[data-install-identifiers]').textContent =
      (game.title_id ? 'Title ID ' + game.title_id : '') + (pkg.content_id ? ' · Content ID ' + pkg.content_id : '');
    panel.querySelector('[data-install-phase-label]').textContent = 'PS5 yükleyicisi hazırlanıyor…';
    panel.querySelector('[data-install-error]').textContent = '';
    panel.querySelector('[data-install-cancel]').disabled = false;
  }

  function formatDuration(seconds) {
    if (!Number.isFinite(Number(seconds)) || Number(seconds) < 0) return '—';
    const value = Math.floor(Number(seconds));
    return String(Math.floor(value / 60)).padStart(2, '0') + ':' + String(value % 60).padStart(2, '0');
  }

  function formatSpeed(bytesPerSecond) {
    const value = Number(bytesPerSecond);
    if (!Number.isFinite(value) || value <= 0) return '—';
    return formatSize(value) + '/sn';
  }

  function renderInstallPanel(result) {
    const panel = ensureInstallPanel();
    const phaseLabels = {
      resolving_source: 'Paket kaynağı çözülüyor…', probing_upstream: 'Sunucu ve Range desteği kontrol ediliyor…',
      native_drive_downloading: 'Google Drive’dan indiriliyor…', native_drive_verifying: 'İndirilen PKG okunup doğrulanıyor…', native_drive_completed: 'PKG indirildi; kurulum hazırlanıyor…',
      probe_started: 'Upstream probe başladı…', DNS_started: 'Sunucu adresi çözülüyor…', DNS_completed: 'Sunucu adresi bulundu…',
      connect_started: 'Sunucuya bağlanılıyor…', connected: 'Sunucu bağlantısı hazır…', request_send: 'HTTP isteği gönderiliyor…',
      response_headers: 'Sunucu yanıtı bekleniyor…', response_headers_failed: 'Sunucu başlık yanıtı alınamadı.',
      HEAD_started: 'Sunucu HEAD isteği bekleniyor…', HEAD_completed: 'Sunucu boyutu alındı…',
      range_probe_started: 'Range desteği doğrulanıyor…', range_probe_completed: 'Range desteği doğrulandı…',
      sceNetPoolCreate: 'TLS ağ havuzu hazırlanıyor…', sceSslInit: 'TLS altyapısı hazırlanıyor…',
      sceHttp2Init: 'HTTPS istemcisi hazırlanıyor…', sceHttp2CreateTemplate: 'HTTPS şablonu hazırlanıyor…',
      sceHttp2CreateRequestWithURL: 'HTTPS isteği hazırlanıyor…', sceHttp2SendRequest: 'HTTPS isteği gönderiliyor…',
      sceHttp2GetStatusCode: 'HTTPS yanıt durumu bekleniyor…', sceHttp2GetAllResponseHeaders: 'HTTPS başlıkları okunuyor…',
      sceHttp2ReadData: 'HTTPS verisi okunuyor…',
      source_resolving: 'Paket kaynağı çözülüyor…', probing_upstream: 'Paket kaynağı kontrol ediliyor…',
      starting_relay: 'Yerel aktarım hazırlanıyor…', relay_starting: 'Yerel aktarım sunucusu hazırlanıyor…', relay_ready: 'Yerel aktarım hazır; PS5 yükleyicisi başlatılıyor…',
      starting_helper: 'PS5 yükleyicisi hazırlanıyor…', helper_starting: 'PS5 yükleyicisi hazırlanıyor…', initializing_appinst: 'PS5 yükleyicisi başlatılıyor…', appinst_initializing: 'AppInstUtil başlatılıyor…',
      calling_install_by_package: 'PS5 yükleyicisi paketi açıyor…', appinst_submitting: 'Paket PS5 yükleyicisine gönderiliyor…', waiting_for_first_range: 'İlk veri isteği bekleniyor…', waiting_first_range: 'PS5’in ilk veri isteği bekleniyor…', waiting_retry_range: 'Yeniden denenen yükleyicinin veri isteği bekleniyor…',
      first_range: 'İlk veri aralığı alındı…', installing: 'Paket aktarılıyor…', finalizing: 'Kurulum tamamlanıyor…',
      appinst_retry_wait: 'Yükleyici hazır değil, yeniden deneme bekleniyor…',
      cancelling: 'İptal işlemi sürüyor…', cancelled: 'Kurulum iptal edildi.', failed: 'Kurulum başarısız oldu.', completed: 'Kurulum tamamlandı.'
    };
    phaseLabels.diagnostic_worker_enter = 'Tanılama worker’ı başladı…';
    phaseLabels.source_resolving = 'Paket kaynağı çözülüyor…';
    phaseLabels.source_resolved = 'Paket kaynağı doğrulandı…';
    phaseLabels.head_begin = 'HTTP HEAD isteği gönderiliyor…';
    phaseLabels.head_complete = 'HTTP HEAD yanıtı doğrulandı…';
    phaseLabels.range_probe_begin = 'HTTP Range doğrulanıyor…';
    phaseLabels.http_dns = 'Kaynak sunucunun adresi çözülüyor…';
    phaseLabels.http_connect = 'Kaynak sunucuya TCP bağlantısı kuruluyor…';
    phaseLabels.http_range_connect = 'Range isteği için TCP bağlantısı kuruluyor…';
    phaseLabels.http_head_send = 'HTTP HEAD isteği gönderiliyor…';
    phaseLabels.http_head_headers = 'HTTP HEAD yanıt başlıkları bekleniyor…';
    phaseLabels.http_head_complete = 'HTTP HEAD yanıtı alındı…';
    phaseLabels.http_range_send = 'HTTP Range isteği gönderiliyor…';
    phaseLabels.http_range_headers = 'HTTP Range başlıkları bekleniyor…';
    phaseLabels.http_range_body = '1024 bayt Range yanıtı okunuyor…';
    phaseLabels.range_probe_complete = 'HTTP Range yanıtı doğrulandı…';
    phaseLabels.diagnostic_probe_ok = 'Kaynak ve HTTP probe başarılı.';
    phaseLabels.diagnostic_probe_failed = 'HTTP tanılama başarısız.';
    phaseLabels.relay_starting = 'Loopback Range relay başlatılıyor…';
    phaseLabels.relay_ready = 'Loopback Range relay hazır…';
    phaseLabels.relay_selftest_head = 'Relay HEAD yanıtı sınanıyor…';
    phaseLabels.relay_selftest_range_0 = 'Relay ilk 64 KiB Range isteğini sınıyor…';
    phaseLabels.relay_selftest_repeat_1 = 'Relay tekrarlanan Range isteğini sınıyor…';
    phaseLabels.relay_selftest_repeat_2 = 'Relay ikinci tekrarlanan Range isteğini sınıyor…';
    phaseLabels.relay_selftest_range_nonzero = 'Relay farklı offset Range isteğini sınıyor…';
    phaseLabels.relay_selftest_tail = 'Relay dosyanın son Range isteğini sınıyor…';
    phaseLabels.relay_selftest_404 = 'Bilinmeyen relay yolu 404 olarak sınanıyor…';
    phaseLabels.diagnostic_relay_ok = 'Loopback relay testleri başarılı.';
    phaseLabels.diagnostic_relay_failed = 'Loopback relay testi başarısız.';
    phaseLabels.helper_listener_start = 'Helper IPC listener başlatılıyor…';
    phaseLabels.helper_listener_ready = '127.0.0.1:1924 hazır…';
    phaseLabels.elfldr_connect_start = 'elfldr 127.0.0.1:9021 bağlantısı kuruluyor…';
    phaseLabels.elfldr_connected = 'elfldr bağlantısı kuruldu…';
    phaseLabels.helper_upload_start = 'Gömülü helper ELF yükleniyor…';
    phaseLabels.helper_upload_complete = 'Helper ELF yüklendi…';
    phaseLabels.helper_wait_callback = 'Helper IPC callback bekleniyor…';
    phaseLabels.helper_connected = 'Helper callback alındı…';
    phaseLabels.helper_wait_ready = 'AppInstUtil READY yanıtı bekleniyor…';
    phaseLabels.appinst_initializing = 'AppInstUtil başlatılıyor…';
    phaseLabels.helper_ready = 'AppInstUtil READY…';
    phaseLabels.helper_close_requested = 'Helper CLOSE gönderiliyor…';
    phaseLabels.appinst_terminating = 'AppInstUtil sonlandırılıyor…';
    phaseLabels.helper_exit_wait = 'Helper sürecinin çıkışı bekleniyor…';
    phaseLabels.helper_reaped = 'Helper süreci temizlendi…';
    phaseLabels.diagnostic_helper_ok = 'Helper ve AppInstUtil yaşam döngüsü başarılı.';
    phaseLabels.diagnostic_helper_failed = 'Helper tanılama başarısız.';
    const percent = Math.max(0, Math.min(100, Number(result.progress_percent) || 0));
    const probeStage = result.phase === 'probing_upstream' && (result.tls_stage || result.upstream_stage);
    let phaseText = probeStage ? phaseLabels[probeStage] || ('Probe: ' + probeStage) : phaseLabels[result.phase];
    if (result.phase === 'appinst_retry_wait') {
      phaseText = 'Yükleyici hazır değil, ' + Number(result.retry_delay_seconds || 0) + ' sn sonra tekrar deneniyor...';
    } else if (result.phase === 'helper_starting' && Number(result.retry_attempt) > 1) {
      phaseText = 'PS5 yükleyicisi yeniden hazırlanıyor... (' + Number(result.retry_attempt) + '/3)';
    } else if (result.first_range_at_ms && !result.bulk_transfer_started && ['installing', 'first_range'].includes(result.phase)) {
      phaseText = 'PKG doğrulanıyor...';
    }
    panel.querySelector('[data-install-phase-label]').textContent = phaseText || ('Aşama: ' + (result.phase || result.state));
    const installIds = panel.querySelector('[data-install-identifiers]');
    if (result.install_returned_content_id) {
      const game = installIdentity(result).game;
      installIds.textContent = (game.title_id ? 'Title ID ' + game.title_id + ' · ' : '') +
        'Content ID ' + result.install_returned_content_id;
    }
    const chip = panel.querySelector('[data-install-chip]');
    chip.textContent = result.state === 'completed' ? 'TAMAMLANDI' : result.state === 'failed' ? 'HATA' :
      result.state === 'cancelled' ? 'İPTAL EDİLDİ' : result.state === 'cancelling' ? 'İPTAL EDİLİYOR' :
      result.state === 'starting' ? 'HAZIRLANIYOR' : 'KURULUYOR';
    chip.dataset.state = result.state || 'starting';
    panel.querySelector('[data-install-fill]').style.width = percent.toFixed(2) + '%';
    panel.querySelector('[data-install-percent]').textContent = percent.toFixed(1) + '%';
    panel.querySelector('[data-install-bytes]').textContent = formatKnownSize(result.appinst_downloaded_bytes || result.downloaded_bytes || 0) + ' / ' + formatKnownSize(result.appinst_total_bytes || result.total_bytes || 0);
    panel.querySelector('[data-install-speed]').textContent = formatSpeed(result.current_speed_bps);
    panel.querySelector('[data-install-average]').textContent = formatSpeed(result.average_speed_bps);
    panel.querySelector('[data-install-eta]').textContent = result.eta_seconds == null ? 'Bekleniyor' : formatDuration(result.eta_seconds);
    panel.querySelector('[data-install-elapsed]').textContent = formatDuration(result.elapsed_seconds || 0);
    panel.querySelector('[data-install-served]').textContent = formatKnownSize(result.stream_served_bytes);
    panel.querySelector('[data-install-upstream]').textContent = formatKnownSize(result.upstream_received_bytes);
    panel.querySelector('[data-install-source]').textContent = result.source_type === 'source_group' ? 'Kaynak: PH Store Server' : 'Kaynak: Direct HTTP';
    panel.querySelector('[data-install-error]').textContent = result.error ? 'Hata: ' + result.error + (result.appinst_error_description ? ' · ' + result.appinst_error_description : '') : '';
    panel.querySelector('[data-install-cancel]').disabled = ['completed', 'failed', 'cancelled', 'diagnostic_probe_ok', 'diagnostic_probe_failed', 'diagnostic_relay_ok', 'diagnostic_relay_failed', 'diagnostic_helper_ok', 'diagnostic_helper_failed'].includes(result.state);
  }

  function closeDetails() {
    if (!state.selected) return;
    state.selected = null;
    $('#detail-page').hidden = true;
    if (state.page === 'settings') showPage('settings');
    else showPage('games');
    if (lastCard && document.body.contains(lastCard)) lastCard.focus();
    else if (lastCardIndex >= 0) focusVirtualGame(lastCardIndex);
    else if (grid.querySelector('.game-card')) grid.querySelector('.game-card').focus();
  }

  function showPage(page) {
    state.page = page;
    state.selected = null;
    $('#games-page').hidden = page !== 'games';
    $('#settings-page').hidden = page !== 'settings';
    $('#downloads-page').hidden = page !== 'downloads';
    $('#detail-page').hidden = true;
    document.querySelectorAll('.nav-button').forEach((button) => {
      const active = button.dataset.page === page;
      button.classList.toggle('active', active);
      button.setAttribute('aria-current', active ? 'page' : 'false');
    });
    renderActiveDownload(false);
    if (page === 'settings') { loadSettings(); refreshImageCacheStatus(); }
    else if(page === 'downloads'){pollSpectrum();pollInstallStatus();renderDownloadHistory();}
    else { gridMetrics = null; renderedWindow = ''; renderGames(); }
  }

  async function loadSettings() {
    const target = $('#settings-list');
    const rows = [['PH Store sürümü', '0.1.0'], ['Build türü', debug ? 'Debug' : 'Release']];
    try {
      const response = await fetch('/api/store/info', { cache: 'no-store' });
      if (!response.ok) throw new Error('API ' + response.status);
      state.info = await response.json();
      previousCatalogState = state.info.catalog_state || '';
      rows[0][1] = state.info.version || '0.1.0';
      rows.push(['Yerel sunucu durumu', state.info.local_server === 'ready' ? 'Hazır' : 'Bağlı değil']);
      rows.push(['Katalog durumu', catalogLabel(state.info)]);
    } catch (_) {
      rows.push(['Yerel sunucu durumu', 'Bağlı değil']);
      rows.push(['Katalog durumu', 'Kullanılamıyor']);
    }
    target.replaceChildren();
    rows.forEach(([label, value], index) => {
      const row = document.createElement('div');
      const term = document.createElement('dt'); term.textContent = label;
      const description = document.createElement('dd'); description.textContent = value;
      if (index === 2 && value === 'Hazır') description.className = 'status-good';
      else if (value !== 'Kullanılamıyor') description.className = 'status-muted';
      row.append(term, description); target.append(row);
    });
    updateCatalogRefreshButton(state.info);
    {
      try {
        const response = await fetch('/api/store/shortcut/status', { cache: 'no-store' });
        const shortcut = await response.json();
        if (response.ok && shortcut.ok === true) {
          state.shortcutReady = shortcut.installed === true;
          $('#shortcut-status').textContent = state.shortcutReady ? 'Kısayol hazır.' : 'Kısayol henüz kurulu değil.';
        }
      } catch (_) { $('#shortcut-status').textContent = 'Kısayol durumu alınamadı.'; }
    }
  }

  function catalogLabel(info) {
    if (!info) return 'Katalog yükleniyor';
    if (info.catalog_state === 'loading') return 'Katalog yükleniyor';
    if (info.catalog_state === 'error') return 'Katalog kullanılamıyor';
    const version = Number(info.catalog_version) || 0;
    const count = Number(info.game_count) || 0;
    const details = version > 0 ? 'Sürüm ' + version + ' • ' + count + ' oyun' : '';
    if (info.catalog_state === 'refreshing') return 'Bağlı • Güncelleniyor' + (details ? ' • ' + details : '');
    return (info.last_error ? 'Bağlı • Son yenileme başarısız' : 'Bağlı') + (details ? ' • ' + details : '');
  }

  function updateCatalogRefreshButton(info) {
    const button = $('#catalog-refresh-button');
    button.disabled = Boolean(info && ['loading', 'refreshing'].includes(info.catalog_state));
  }

  async function refreshSettingsCatalog() {
    try {
      const response = await fetch('/api/store/info', { cache: 'no-store' });
      if (!response.ok) return;
      const oldVersion = Number(state.info && state.info.catalog_version) || 0;
      const next = await response.json();
      const row = Array.from($('#settings-list').querySelectorAll('dt')).find((term) => term.textContent === 'Katalog durumu');
      if (row) row.nextElementSibling.textContent = catalogLabel(next);
      updateCatalogRefreshButton(next);
      const initialReady=next.catalog_state === 'ready' && !initialCatalogRefreshObserved;
      if(initialReady)initialCatalogRefreshObserved=true;
      if (initialReady || (previousCatalogState === 'refreshing' && next.catalog_state === 'ready') ||
          (oldVersion > 0 && Number(next.catalog_version) > oldVersion)) loadCatalog();
      previousCatalogState = next.catalog_state || '';
      state.info = next;
    } catch (_) { /* settings remains usable if only catalog status is unavailable */ }
  }

  async function loadImageCacheIndex() {
    const controller = new AbortController();
    const timeout = window.setTimeout(() => controller.abort(), 4000);
    try {
      const response = await fetch('/api/store/images/index', { cache: 'no-store', signal: controller.signal });
      const result = await response.json();
      if (!response.ok || result.ok !== true || !Array.isArray(result.items)) return;
      imageCache.clear();
      for (const item of result.items) {
        if (typeof item.url !== 'string' || !/^\/resimler\/[0-9a-f]{32}$/.test(item.path)) continue;
        const original = new URL(item.url, window.location.origin);
        if (original.protocol === 'https:' || original.origin === window.location.origin)
          imageCache.set(original.href, new URL(item.path, window.location.origin).href);
      }
      // The PS5 shell can resume the same document without a full page load.
      document.querySelectorAll('img').forEach((image) => {
        const local = imageCache.get(image.src);
        if (local) image.src = local;
      });
    } catch (_) { /* Original image URLs remain usable when cache is absent. */ }
    finally { window.clearTimeout(timeout); }
  }

  async function refreshImageCacheStatus() {
    if (cacheStatusPending) return;
    cacheStatusPending = true;
    try {
      const response = await fetch('/api/store/images/status', { cache: 'no-store' });
      const result = await response.json();
      if (!response.ok || result.ok !== true) return;
      $('#image-cache-button').disabled = result.running;
      const done = result.downloaded + result.existing + result.failed;
      $('#image-cache-status').textContent = result.running ?
        'Kapaklar hazırlanıyor: ' + done + ' / ' + result.total + ' · Hata: ' + result.failed :
        result.state === 'ready' ? 'Cache hazır, kapatıp açın.' :
        result.state === 'partial' || result.state === 'error' ?
        'Cache tamamlanamadı. Hazır: ' + (result.downloaded + result.existing) + ' · Hata: ' + result.failed + '. Cache al ile tekrar deneyin.' :
        'Yalnızca kapak görselleri cihazda saklanır. Katalog yenilenince eksik kapaklar otomatik alınır.';
      if (result.failed > 0 || result.state === 'error') {
        const code = '0x' + (Number(result.native_result) >>> 0).toString(16).toUpperCase().padStart(8, '0');
        $('#image-cache-status').textContent += ' Son kod: ' + code + ' · ' + (result.last_stage || 'bilinmiyor');
      }
    } catch (_) { $('#image-cache-status').textContent = 'Cache durumuna ulaşılamadı.'; }
    finally { cacheStatusPending = false; }
  }

  async function requestImageCache() {
    $('#image-cache-button').disabled = true;
    try {
      const response = await fetch('/api/store/images/cache', { method: 'POST', cache: 'no-store' });
      const result = await response.json();
      if (!response.ok && response.status !== 409) throw new Error(result.error || 'cache_unavailable');
      showToast(response.status === 409 ? 'Cache zaten hazırlanıyor.' : 'Kapak görselleri indiriliyor…');
    } catch (_) {
      $('#image-cache-status').textContent = 'Cache başlatılamadı. Katalog hazır olduğunda tekrar deneyin.';
      $('#image-cache-button').disabled = false;
      return;
    }
    await refreshImageCacheStatus();
  }

  async function requestCatalogRefresh() {
    const button = $('#catalog-refresh-button');
    button.disabled = true;
    try {
      const response = await fetch('/api/store/catalog/refresh', { method: 'POST', cache: 'no-store' });
      const result = await response.json();
      if (!response.ok && response.status !== 409) throw new Error(result.error || 'catalog_refresh_failed');
      showToast(response.status === 409 ? 'Katalog yenilemesi zaten sürüyor.' : 'Katalog yenileniyor…');
      await refreshSettingsCatalog();
    } catch (error) {
      showToast('Katalog yenilenemedi: ' + (error.message || 'sunucu hatası'));
      button.disabled = false;
    }
  }

  async function installShortcut() {
    const button = $('#shortcut-install-button');
    const status = $('#shortcut-status');
    button.disabled = true;
    button.textContent = 'Kuruluyor…';
    status.textContent = 'Ana ekran kısayolu kuruluyor…';
    try {
      const response = await fetch('/api/store/shortcut/install', { method: 'POST', cache: 'no-store' });
      const result = await response.json();
      if (!response.ok || result.ok !== true) {
        const code = result && result.code ? ' (' + result.code + ')' : '';
        throw new Error(((result && result.error) || 'shortcut_install_failed') + code);
      }
      state.shortcutReady = true;
      status.textContent = 'Kısayol hazır.';
      showToast('Kısayol hazır.');
    } catch (error) {
      state.shortcutReady = false;
      status.textContent = 'Kısayol kurulamadı: ' + (error.message || 'bağlantı hatası');
      showToast(status.textContent);
    } finally {
      button.disabled = false;
      button.textContent = 'Kısayolu Kur / Yeniden Kur';
    }
  }

  function moveFocus(direction) {
    const current = document.activeElement;
    if (!current || !current.getBoundingClientRect) return false;
    const selectors = 'button:not([disabled]),a[href],summary,input[type="search"]';
    const nodes = Array.from(document.querySelectorAll(selectors)).filter((node) => !node.closest('[hidden]') && node.getClientRects().length);
    const rect = current.getBoundingClientRect();
    const cx = rect.left + rect.width / 2, cy = rect.top + rect.height / 2;
    let best = null, bestScore = Infinity;
    nodes.forEach((node) => {
      if (node === current) return;
      const r = node.getBoundingClientRect(), x = r.left + r.width / 2, y = r.top + r.height / 2;
      const dx = x - cx, dy = y - cy;
      const primary = direction === 'ArrowLeft' ? -dx : direction === 'ArrowRight' ? dx : direction === 'ArrowUp' ? -dy : dy;
      if (primary <= 2) return;
      const secondary = direction === 'ArrowLeft' || direction === 'ArrowRight' ? Math.abs(dy) : Math.abs(dx);
      const score = primary + secondary * 1.8;
      if (score < bestScore) { best = node; bestScore = score; }
    });
    if (!best) return false;
    best.focus();
    best.scrollIntoView({ block: 'nearest', inline: 'nearest' });
    return true;
  }

  function logInput(event) {
    if (!debug) return;
    const line = 'key=' + event.key + ' code=' + event.code + ' keyCode=' + event.keyCode;
    const output = $('#debug-output');
    const lines = (output.textContent ? output.textContent.split('\n') : []).concat(line).slice(-50);
    output.textContent = lines.join('\n');
  }

  document.addEventListener('keydown', (event) => {
    logInput(event);
    if (state.selected && ['Escape', 'Backspace', 'BrowserBack', 'GoBack'].includes(event.key)) {
      event.preventDefault(); closeDetails(); return;
    }
    if (['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown'].includes(event.key) && moveVirtualCard(event.key)) {
      event.preventDefault(); return;
    }
    if (event.target === searchInput && ['ArrowLeft', 'ArrowRight'].includes(event.key)) return;
    if (['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown'].includes(event.key) && moveFocus(event.key)) event.preventDefault();
  });
  window.addEventListener('scroll', () => {
    scheduleInstalledBadges();
    if (state.page !== 'games' || $('#games-page').hidden || renderFrame) return;
    renderFrame = window.requestAnimationFrame(() => { renderFrame = 0; renderGames(); });
  }, { passive: true });
  window.addEventListener('resize', () => {
    gridMetrics = null;
    renderedWindow = '';
    if (state.page === 'games') renderGames();
  }, { passive: true });
  window.addEventListener('focus', () => {
    pollInstallStatus();
    loadImageCacheIndex();
    if (state.selected && (state.selected.title_id || installedPackageId(state.selected))) queryInstalledState(state.selected);
    else scheduleInstalledBadges();
  });
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) window.clearTimeout(installPollTimer);
    if (!document.hidden) {
      pollInstallStatus();
      loadImageCacheIndex().then(() => {
        gridGames = null; renderedWindow = '';
        if (!state.selected && state.page === 'games') renderGames();
      });
      if (state.selected && (state.selected.title_id || installedPackageId(state.selected))) queryInstalledState(state.selected);
      else scheduleInstalledBadges();
    }
  });
  searchInput.addEventListener('input', () => setSearch(searchInput.value, false));
  searchClear.addEventListener('click', () => {
    window.clearTimeout(searchTimer);
    searchInput.value = '';
    searchClear.hidden = true;
    setSearch('', true);
    searchInput.focus();
  });
  document.querySelectorAll('.nav-button').forEach((button) => button.addEventListener('click', () => showPage(button.dataset.page)));
  $('#back-button').addEventListener('click', closeDetails);
  $('#shortcut-install-button').addEventListener('click', installShortcut);
  $('#active-download-open').addEventListener('click', reopenActiveDownload);
  $('#image-cache-button').addEventListener('click', requestImageCache);
  $('#catalog-refresh-button').addEventListener('click', requestCatalogRefresh);
  window.setInterval(() => {
    if (!document.hidden) refreshSettingsCatalog();
    if (state.page === 'settings') refreshImageCacheStatus();
  }, 2000);
  if (debug) $('#debug-panel').hidden = false;

  async function loadCatalog() {
    try {
      const response = await fetch('/api/store/catalog', { cache: 'no-store' });
      const result = await response.json();
      if (!response.ok && response.status === 503 && result.state === 'loading') {
        $('#catalog-caption').textContent = 'Katalog yükleniyor…';
        makeCategoryTabs();
        renderGames();
        window.clearTimeout(catalogRetryTimer);
        catalogRetryTimer = window.setTimeout(loadCatalog, 1600);
        return;
      }
      if (!response.ok || Number(result.schema_version) !== 1 || !Array.isArray(result.games)) {
        $('#catalog-caption').textContent = result.state === 'error' ? 'Katalog kullanılamıyor' : 'Katalog yanıtı geçersiz';
        state.games = [];
      } else {
        window.clearTimeout(catalogRetryTimer);
        await loadImageCacheIndex();
        state.games = normalizeGames(result.games);
        if (lastInstallStatus && lastInstallStatus.package_id) {
          updateInstallIdentity(lastInstallStatus); renderActiveDownload(false);
        }
        filteredGames = matchingGames();
        renderedWindow = '';
        state.catalogVersion = Number(result.catalog_version) || null;
        updateMethodCaption();
      }
    } catch (_) {
      $('#catalog-caption').textContent = 'Yerel sunucuya ulaşılamıyor';
      if (!state.games.length) {
        window.clearTimeout(catalogRetryTimer);
        catalogRetryTimer = window.setTimeout(loadCatalog, 1800);
      }
    }
    makeCategoryTabs();
    filteredGames = matchingGames();
    renderedWindow = '';
    renderGames();
  }
  loadCatalog();
  pollInstallStatus();
  previousCatalogState = state.info ? state.info.catalog_state : '';
  if (debug) {
    const initial = document.createElement('button');
    initial.type = 'button'; initial.className = 'nav-button'; initial.dataset.page = 'games';
  }
})();
