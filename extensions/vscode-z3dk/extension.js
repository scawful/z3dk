const fs = require('fs');
const os = require('os');
const path = require('path');
const { exec } = require('child_process');
const vscode = require('vscode');

let LanguageClient;

let client;
let clientStartPromise;
let outputChannel;
let terminal;
let commandProvider;
let dashboardProvider;
let labelSearchProvider;
let romMapProvider;
let statusItems = [];
let extensionContext;
let lspActive = false;
let languageClientLoadError;
const labelIndexCache = new Map();
let labelIndexRefreshTimer;
let labelIndexInProgress = false;
let labelIndexQueued = false;
const LABEL_SORT_LIMIT = 50000;
const LARGE_LABEL_MIN_CHARS = 2;
const LARGE_LABEL_PREVIEW_LIMIT = 80;
const DEFAULT_LANGUAGE_IDS = [
  '65816-assembly',
  'asar',
  'asm',
  '65816',
  '65c816',
  'wla-65816',
  'snes-asm',
  'snesasm',
  'super-nes-asm'
];

function workspaceRoot() {
  const folders = vscode.workspace.workspaceFolders;
  if (!folders || folders.length === 0) {
    return undefined;
  }
  return folders[0].uri.fsPath;
}

function findWorkspaceFolder(name) {
  const folders = vscode.workspace.workspaceFolders;
  if (!folders) {
    return undefined;
  }
  return folders.find(folder => path.basename(folder.uri.fsPath) === name);
}

function expandHome(inputPath) {
  if (!inputPath || typeof inputPath !== 'string') {
    return inputPath;
  }
  let output = inputPath;
  const root = workspaceRoot();
  if (root) {
    output = output.replace(/\$\{workspaceFolder\}/g, root);
    output = output.replace(/\$\{workspaceRoot\}/g, root);
  }
  if (output.startsWith('~')) {
    return path.join(os.homedir(), output.slice(1));
  }
  return output;
}

const z3dkTomlCache = new Map();

function stripTomlComment(line) {
  let inString = false;
  let escape = false;
  for (let i = 0; i < line.length; i += 1) {
    const c = line[i];
    if (escape) {
      escape = false;
      continue;
    }
    if (c === '\\') {
      escape = true;
      continue;
    }
    if (c === '"') {
      inString = !inString;
      continue;
    }
    if (!inString && c === '#') {
      return line.slice(0, i);
    }
  }
  return line;
}

function unescapeTomlString(value) {
  let output = '';
  let escape = false;
  for (const c of value) {
    if (escape) {
      switch (c) {
        case 'n':
          output += '\n';
          break;
        case 't':
          output += '\t';
          break;
        case 'r':
          output += '\r';
          break;
        case '\\':
        case '"':
          output += c;
          break;
        default:
          output += c;
          break;
      }
      escape = false;
    } else if (c === '\\') {
      escape = true;
    } else {
      output += c;
    }
  }
  return output;
}

function parseTomlString(value) {
  const trimmed = value.trim();
  if (!trimmed) {
    return '';
  }
  if (trimmed.startsWith('"') && trimmed.endsWith('"') && trimmed.length >= 2) {
    return unescapeTomlString(trimmed.slice(1, -1));
  }
  return trimmed;
}

function parseTomlStringArray(value) {
  const trimmed = value.trim();
  if (!trimmed) {
    return [];
  }
  if (!trimmed.startsWith('[') || !trimmed.endsWith(']')) {
    const single = parseTomlString(trimmed);
    return single ? [single] : [];
  }
  const inner = trimmed.slice(1, -1);
  const result = [];
  let current = '';
  let inString = false;
  let escape = false;
  for (const c of inner) {
    if (escape) {
      current += c;
      escape = false;
      continue;
    }
    if (c === '\\') {
      escape = true;
      continue;
    }
    if (c === '"') {
      inString = !inString;
      continue;
    }
    if (!inString && c === ',') {
      const token = current.trim();
      if (token) {
        const valueToken = parseTomlString(token);
        if (valueToken) {
          result.push(valueToken);
        }
      }
      current = '';
      continue;
    }
    current += c;
  }
  const token = current.trim();
  if (token) {
    const valueToken = parseTomlString(token);
    if (valueToken) {
      result.push(valueToken);
    }
  }
  return result;
}

function resolveProjectRoot() {
  const config = vscode.workspace.getConfiguration('z3dk');
  const configuredRoot = normalizeProjectRoot(expandHome(config.get('projectRoot')));
  if (configuredRoot) {
    return configuredRoot;
  }
  const folders = vscode.workspace.workspaceFolders || [];
  const candidates = folders.filter(folder => {
    const tomlPath = path.join(folder.uri.fsPath, 'z3dk.toml');
    return fs.existsSync(tomlPath);
  });
  if (candidates.length === 1) {
    return candidates[0].uri.fsPath;
  }
  const activePath = vscode.window.activeTextEditor
    ? vscode.window.activeTextEditor.document.uri.fsPath
    : '';
  if (activePath) {
    const match = candidates.find(folder => activePath.startsWith(folder.uri.fsPath));
    if (match) {
      return match.uri.fsPath;
    }
    const ancestor = findProjectRootForPath(activePath);
    if (ancestor) {
      return ancestor;
    }
  }
  const root = workspaceRoot();
  if (root) {
    const parent = path.dirname(root);
    const sibling = path.join(parent, 'oracle-of-secrets');
    if (fs.existsSync(path.join(sibling, 'z3dk.toml'))) {
      return sibling;
    }
  }
  return root;
}

function normalizeProjectRoot(input) {
  if (!input) {
    return '';
  }
  let resolved = input;
  try {
    if (fs.existsSync(resolved) && fs.statSync(resolved).isFile() && resolved.endsWith('z3dk.toml')) {
      resolved = path.dirname(resolved);
    }
  } catch {
    // ignore
  }
  if (resolved && fs.existsSync(resolved)) {
    return resolved;
  }
  return '';
}

function findProjectRootForPath(startPath) {
  if (!startPath) {
    return '';
  }
  let dir = startPath;
  try {
    if (fs.existsSync(dir) && fs.statSync(dir).isFile()) {
      dir = path.dirname(dir);
    }
  } catch {
    return '';
  }
  for (let i = 0; i < 10; i += 1) {
    if (fs.existsSync(path.join(dir, 'z3dk.toml'))) {
      return dir;
    }
    const parent = path.dirname(dir);
    if (parent === dir) {
      break;
    }
    dir = parent;
  }
  return '';
}

function loadZ3dkToml(rootDir) {
  if (!rootDir) {
    return null;
  }
  const configPath = path.join(rootDir, 'z3dk.toml');
  if (!fs.existsSync(configPath)) {
    return null;
  }
  let stat;
  try {
    stat = fs.statSync(configPath);
  } catch {
    return null;
  }
  const cacheKey = configPath;
  const cached = z3dkTomlCache.get(cacheKey);
  if (cached && cached.mtimeMs === stat.mtimeMs) {
    return cached.data;
  }

  let content = '';
  try {
    content = fs.readFileSync(configPath, 'utf8');
  } catch {
    return null;
  }

  const data = {
    path: configPath,
    dir: path.dirname(configPath),
    rom_path: '',
    main_files: [],
    symbols: '',
    symbols_path: ''
  };

  const lines = content.split(/\r?\n/);
  for (const rawLine of lines) {
    const stripped = stripTomlComment(rawLine);
    const trimmed = stripped.trim();
    if (!trimmed) {
      continue;
    }
    const equals = trimmed.indexOf('=');
    if (equals === -1) {
      continue;
    }
    const key = trimmed.slice(0, equals).trim();
    const value = trimmed.slice(equals + 1).trim();
    if (key === 'rom_path' || key === 'rom') {
      data.rom_path = parseTomlString(value);
    } else if (key === 'main' || key === 'main_file' || key === 'main_files' ||
               key === 'entry' || key === 'entry_files') {
      data.main_files = parseTomlStringArray(value);
    } else if (key === 'symbols') {
      data.symbols = parseTomlString(value);
    } else if (key === 'symbols_path') {
      data.symbols_path = parseTomlString(value);
    }
  }

  z3dkTomlCache.set(cacheKey, { mtimeMs: stat.mtimeMs, data });
  return data;
}

function workspaceSibling(name) {
  const root = workspaceRoot();
  if (!root) {
    return undefined;
  }
  return path.join(path.dirname(root), name);
}

function resolveRepoPath(name, configKey, config) {
  const configured = expandHome(config.get(configKey));
  if (configured) {
    return configured;
  }
  const folder = findWorkspaceFolder(name);
  if (folder) {
    return folder.uri.fsPath;
  }
  return workspaceSibling(name);
}

function resolveConfigPath(config, key, fallbackPaths) {
  const configured = expandHome(config.get(key));
  if (configured) {
    return configured;
  }
  for (const fallback of fallbackPaths) {
    if (fallback) {
      return fallback;
    }
  }
  return '';
}

function resolveTomlPath(tomlConfig, rawPath) {
  if (!tomlConfig || !rawPath) {
    return '';
  }
  const expanded = expandHome(rawPath);
  if (path.isAbsolute(expanded)) {
    return expanded;
  }
  const baseDir = tomlConfig.dir || '';
  if (baseDir) {
    return path.join(baseDir, expanded);
  }
  return expanded;
}

function resolveSymbolFormat(config, tomlConfig) {
  const configured = config.get('symbolFormat');
  if (configured) {
    return configured;
  }
  if (tomlConfig && tomlConfig.symbols) {
    return tomlConfig.symbols;
  }
  return 'mesen';
}

function resolveSymbolsPath(config, romPath, tomlConfig) {
  const configured = expandHome(config.get('symbolsPath'));
  if (configured) {
    return configured;
  }
  if (tomlConfig && tomlConfig.symbols_path) {
    return resolveTomlPath(tomlConfig, tomlConfig.symbols_path);
  }
  if (!romPath) {
    return '';
  }
  const format = resolveSymbolFormat(config, tomlConfig);
  return format === 'mesen' ? `${romPath}.mlb` : `${romPath}.sym`;
}

function autoDetectRom(rootDir) {
  if (!rootDir) {
    return '';
  }
  const candidates = [];
  scanRomDir(rootDir, candidates);
  scanRomDir(path.join(rootDir, 'Roms'), candidates);
  scanRomDir(path.join(rootDir, 'roms'), candidates);
  scanRomDir(path.join(rootDir, 'ROMs'), candidates);
  scanRomDir(path.join(rootDir, 'RomsBackup'), candidates);
  if (candidates.length === 0) {
    return '';
  }
  if (candidates.length === 1) {
    return candidates[0];
  }
  let newest = candidates[0];
  let newestTime = 0;
  for (const candidate of candidates) {
    try {
      const stat = fs.statSync(candidate);
      if (stat.mtimeMs > newestTime) {
        newestTime = stat.mtimeMs;
        newest = candidate;
      }
    } catch {
      continue;
    }
  }
  return newest;
}

function resolveRomInfo(config) {
  const rootDir = resolveProjectRoot();
  const tomlConfig = loadZ3dkToml(rootDir);
  const configured = expandHome(config.get('romPath'));
  if (configured) {
    return { path: configured, source: 'config', toml: tomlConfig };
  }
  if (tomlConfig && tomlConfig.rom_path) {
    const resolved = resolveTomlPath(tomlConfig, tomlConfig.rom_path);
    if (resolved && fs.existsSync(resolved)) {
      return { path: resolved, source: 'toml', toml: tomlConfig };
    }
  }
  const autoPath = autoDetectRom(rootDir);
  if (autoPath) {
    return { path: autoPath, source: 'auto', toml: tomlConfig };
  }
  return { path: '', source: 'unset', toml: tomlConfig };
}

function resolveMainAsmPath(tomlConfig) {
  if (!tomlConfig) {
    return '';
  }
  if (Array.isArray(tomlConfig.main_files) && tomlConfig.main_files.length > 0) {
    for (const entry of tomlConfig.main_files) {
      const resolved = resolveTomlPath(tomlConfig, entry);
      if (resolved && fs.existsSync(resolved)) {
        return resolved;
      }
    }
  }
  const rootDir = tomlConfig.dir || '';
  if (!rootDir) {
    return '';
  }
  const direct = path.join(rootDir, 'main.asm');
  if (fs.existsSync(direct)) {
    return direct;
  }
  const candidates = fs.readdirSync(rootDir, { withFileTypes: true })
    .filter(entry => entry.isFile())
    .map(entry => entry.name)
    .filter(name => name.toLowerCase().endsWith('_main.asm'))
    .sort();
  if (candidates.length > 0) {
    return path.join(rootDir, candidates[0]);
  }
  return '';
}

function formatBytes(size) {
  if (!Number.isFinite(size) || size <= 0) {
    return '';
  }
  if (size >= 1024 * 1024) {
    return `${(size / (1024 * 1024)).toFixed(2)} MB`;
  }
  if (size >= 1024) {
    return `${(size / 1024).toFixed(1)} KB`;
  }
  return `${size} B`;
}

function scanRomDir(dir, out) {
  if (!dir || !fs.existsSync(dir)) {
    return;
  }
  let entries;
  try {
    entries = fs.readdirSync(dir, { withFileTypes: true });
  } catch {
    return;
  }
  for (const entry of entries) {
    if (!entry.isFile()) {
      continue;
    }
    const ext = path.extname(entry.name).toLowerCase();
    if (ext !== '.sfc' && ext !== '.smc') {
      continue;
    }
    out.push(path.join(dir, entry.name));
  }
}

function collectRomCandidates(rootDir, tomlConfig, config) {
  const candidates = [];
  const seen = new Set();
  const addCandidate = (romPath, source) => {
    if (!romPath) {
      return;
    }
    const expanded = expandHome(romPath);
    const resolved = path.isAbsolute(expanded)
      ? expanded
      : (rootDir ? path.join(rootDir, expanded) : path.resolve(expanded));
    if (seen.has(resolved) || !fs.existsSync(resolved)) {
      return;
    }
    let stat;
    try {
      stat = fs.statSync(resolved);
    } catch {
      return;
    }
    if (!stat.isFile()) {
      return;
    }
    seen.add(resolved);
    candidates.push({
      path: resolved,
      source,
      mtimeMs: stat.mtimeMs,
      size: stat.size
    });
  };

  const configured = expandHome(config.get('romPath'));
  if (configured) {
    addCandidate(configured, 'config');
  }
  if (tomlConfig && tomlConfig.rom_path) {
    addCandidate(resolveTomlPath(tomlConfig, tomlConfig.rom_path), 'toml');
  }

  if (rootDir) {
    const autoCandidates = [];
    scanRomDir(rootDir, autoCandidates);
    scanRomDir(path.join(rootDir, 'Roms'), autoCandidates);
    scanRomDir(path.join(rootDir, 'roms'), autoCandidates);
    scanRomDir(path.join(rootDir, 'ROMs'), autoCandidates);
    scanRomDir(path.join(rootDir, 'RomsBackup'), autoCandidates);
    for (const entry of autoCandidates) {
      addCandidate(entry, 'auto');
    }
  }

  candidates.sort((a, b) => b.mtimeMs - a.mtimeMs);
  return candidates;
}

async function updateRomSetting(config, romPath) {
  await config.update('romPath', romPath, vscode.ConfigurationTarget.Workspace);
  if (extensionContext) {
    updateStatusBar(extensionContext);
  }
  if (dashboardProvider) {
    dashboardProvider.refresh();
  }
}

async function promptForRomSelection(config, options = {}) {
  const rootDir = resolveProjectRoot();
  const tomlConfig = loadZ3dkToml(rootDir);
  const candidates = collectRomCandidates(rootDir, tomlConfig, config);
  const items = candidates.map(candidate => {
    const relative = rootDir ? path.relative(rootDir, candidate.path) : candidate.path;
    const detailParts = [];
    if (candidate.source) {
      detailParts.push(candidate.source);
    }
    const sizeLabel = formatBytes(candidate.size);
    if (sizeLabel) {
      detailParts.push(sizeLabel);
    }
    if (candidate.mtimeMs) {
      detailParts.push(new Date(candidate.mtimeMs).toLocaleString());
    }
    return {
      label: path.basename(candidate.path),
      description: relative || candidate.path,
      detail: detailParts.join(' • '),
      romPath: candidate.path
    };
  });

  items.push({
    label: 'Browse...',
    description: 'Select a ROM file from disk',
    action: 'browse'
  });

  const pick = await vscode.window.showQuickPick(items, {
    placeHolder: options.placeHolder || 'Select ROM',
    matchOnDescription: true,
    matchOnDetail: true,
    ignoreFocusOut: true
  });

  if (!pick) {
    return null;
  }

  if (pick.action === 'browse') {
    const selection = await vscode.window.showOpenDialog({
      canSelectMany: false,
      openLabel: 'Select ROM',
      filters: {
        'SNES ROMs': ['sfc', 'smc'],
        'All files': ['*']
      }
    });
    if (!selection || !selection.length) {
      return null;
    }
    const chosen = selection[0].fsPath;
    if (options.updateConfig !== false) {
      await updateRomSetting(config, chosen);
    }
    return { path: chosen, source: 'picked', toml: tomlConfig };
  }

  if (pick.romPath) {
    if (options.updateConfig !== false) {
      await updateRomSetting(config, pick.romPath);
    }
    return { path: pick.romPath, source: 'picked', toml: tomlConfig };
  }
  return null;
}

async function ensureRomInfo(config, options = {}) {
  const current = resolveRomInfo(config);
  if (!options.forcePick && current.path && fs.existsSync(current.path)) {
    return current;
  }

  const rootDir = resolveProjectRoot();
  const tomlConfig = current.toml || loadZ3dkToml(rootDir);
  const candidates = collectRomCandidates(rootDir, tomlConfig, config);
  if (!options.forcePick && candidates.length === 1) {
    return { path: candidates[0].path, source: candidates[0].source || 'auto', toml: tomlConfig };
  }

  const picked = await promptForRomSelection(config, {
    placeHolder: options.placeHolder || 'Select ROM for this command',
    updateConfig: options.updateConfig
  });
  if (picked) {
    return picked;
  }
  return current;
}

function collectProjectRootCandidates() {
  const candidates = [];
  const seen = new Set();
  const add = (root, source) => {
    if (!root) {
      return;
    }
    const resolved = normalizeProjectRoot(root) || root;
    if (!resolved || seen.has(resolved)) {
      return;
    }
    seen.add(resolved);
    const hasToml = fs.existsSync(path.join(resolved, 'z3dk.toml'));
    candidates.push({
      root: resolved,
      source,
      hasToml
    });
  };

  const config = vscode.workspace.getConfiguration('z3dk');
  const configured = normalizeProjectRoot(expandHome(config.get('projectRoot')));
  if (configured) {
    add(configured, 'config');
  }

  const folders = vscode.workspace.workspaceFolders || [];
  for (const folder of folders) {
    if (fs.existsSync(path.join(folder.uri.fsPath, 'z3dk.toml'))) {
      add(folder.uri.fsPath, 'workspace');
    }
  }

  const activePath = vscode.window.activeTextEditor
    ? vscode.window.activeTextEditor.document.uri.fsPath
    : '';
  const activeRoot = findProjectRootForPath(activePath);
  if (activeRoot) {
    add(activeRoot, 'active');
  }

  const root = workspaceRoot();
  if (root) {
    const parent = path.dirname(root);
    const sibling = path.join(parent, 'oracle-of-secrets');
    if (fs.existsSync(path.join(sibling, 'z3dk.toml'))) {
      add(sibling, 'sibling');
    }
  }

  return candidates;
}

async function updateProjectRootSetting(config, rootPath) {
  await config.update('projectRoot', rootPath, vscode.ConfigurationTarget.Workspace);
  if (extensionContext) {
    updateStatusBar(extensionContext);
  }
  if (dashboardProvider) {
    dashboardProvider.refresh();
  }
  if (romMapProvider) {
    romMapProvider.refresh();
  }
}

async function promptForProjectRootSelection() {
  const config = vscode.workspace.getConfiguration('z3dk');
  const candidates = collectProjectRootCandidates();
  const items = candidates.map(candidate => {
    const label = path.basename(candidate.root);
    const detail = candidate.root;
    const description = candidate.hasToml ? `z3dk.toml (${candidate.source})` : candidate.source;
    return {
      label,
      description,
      detail,
      root: candidate.root
    };
  });

  items.push({
    label: 'Browse...',
    description: 'Select a project folder or z3dk.toml',
    action: 'browse'
  });

  const pick = await vscode.window.showQuickPick(items, {
    placeHolder: 'Select Z3DK project root',
    matchOnDescription: true,
    matchOnDetail: true,
    ignoreFocusOut: true
  });

  if (!pick) {
    return null;
  }

  if (pick.action === 'browse') {
    const selection = await vscode.window.showOpenDialog({
      canSelectMany: false,
      canSelectFolders: true,
      canSelectFiles: true,
      openLabel: 'Select Project',
      filters: {
        'Z3DK Config': ['toml'],
        'All files': ['*']
      }
    });
    if (!selection || !selection.length) {
      return null;
    }
    let chosen = selection[0].fsPath;
    if (chosen.endsWith('z3dk.toml')) {
      chosen = path.dirname(chosen);
    }
    await updateProjectRootSetting(config, chosen);
    return chosen;
  }

  if (pick.root) {
    await updateProjectRootSetting(config, pick.root);
    return pick.root;
  }
  return null;
}

function resolveLabelIndexPath(config, context) {
  const configured = expandHome(config.get('labelIndexPath'));
  if (configured) {
    return configured;
  }
  const root = z3dkRoot(context) || workspaceRoot();
  if (!root) {
    return '';
  }
  return path.join(root, '.context', 'knowledge', 'label_index_all.csv');
}

function resolveLabelMapPath(config, context) {
  const configured = expandHome(config.get('labelMapPath'));
  if (configured) {
    return configured;
  }
  const root = z3dkRoot(context) || workspaceRoot();
  if (!root) {
    return '';
  }
  return path.join(root, '.context', 'knowledge', 'labels_merged.csv');
}

function resolveLabelIndexCandidates(config, context) {
  const configured = expandHome(config.get('labelIndexPath'));
  if (configured) {
    return [configured];
  }
  const root = z3dkRoot(context) || workspaceRoot();
  if (!root) {
    return [];
  }
  return [
    path.join(root, '.context', 'knowledge', 'label_index_all.csv'),
    path.join(root, '.context', 'knowledge', 'label_index.csv')
  ];
}

function parseCsvLine(line) {
  const result = [];
  let current = '';
  let inQuotes = false;
  for (let i = 0; i < line.length; i += 1) {
    const char = line[i];
    if (char === '"') {
      if (inQuotes && line[i + 1] === '"') {
        current += '"';
        i += 1;
        continue;
      }
      inQuotes = !inQuotes;
      continue;
    }
    if (char === ',' && !inQuotes) {
      result.push(current);
      current = '';
      continue;
    }
    current += char;
  }
  result.push(current);
  return result;
}

function parseCsv(text) {
  const lines = text.split(/\r?\n/).filter(line => line.trim().length > 0);
  if (lines.length === 0) {
    return [];
  }
  const headers = parseCsvLine(lines[0]);
  const rows = [];
  for (const line of lines.slice(1)) {
    const values = parseCsvLine(line);
    const row = {};
    headers.forEach((header, index) => {
      row[header] = values[index] || '';
    });
    rows.push(row);
  }
  return rows;
}

function loadLabelIndexFile(indexPath) {
  if (!fs.existsSync(indexPath)) {
    return [];
  }
  const stats = fs.statSync(indexPath);
  const cached = labelIndexCache.get(indexPath);
  if (cached && cached.mtimeMs === stats.mtimeMs) {
    return cached.rows;
  }
  const text = fs.readFileSync(indexPath, 'utf8');
  const rows = parseCsv(text);
  rows.forEach(row => {
    row._label = (row.label || '').toLowerCase();
    row._file = (row.file || '').toLowerCase();
    row._address = (row.address || '').toLowerCase();
    row._source = (row.source_repo || '').toLowerCase();
  });
  const sortedRows = rows.length <= LABEL_SORT_LIMIT
    ? rows.slice(0).sort((a, b) => (a.label || '').localeCompare(b.label || ''))
    : null;
  labelIndexCache.set(indexPath, { mtimeMs: stats.mtimeMs, rows, sortedRows });
  return rows;
}

function getLabelIndexCache(indexPath) {
  if (!indexPath || !fs.existsSync(indexPath)) {
    return null;
  }
  const stats = fs.statSync(indexPath);
  const cached = labelIndexCache.get(indexPath);
  if (cached && cached.mtimeMs === stats.mtimeMs) {
    return cached;
  }
  loadLabelIndexFile(indexPath);
  return labelIndexCache.get(indexPath) || null;
}

function resolveLabelIndex(config, context) {
  const candidates = resolveLabelIndexCandidates(config, context);
  for (const candidate of candidates) {
    if (candidate && fs.existsSync(candidate)) {
      return candidate;
    }
  }
  return resolveLabelIndexPath(config, context);
}

function filterEntriesByScope(entries, scope) {
  if (!scope || scope === 'all') {
    return entries;
  }
  const target = scope === 'hack' ? 'oracle-of-secrets' : scope;
  return entries.filter(entry => entry._source === target);
}

function searchLabelEntries(entries, query, scope, limit) {
  const trimmed = (query || '').trim().toLowerCase();
  if (!trimmed || trimmed.length < 1) {
    return [];
  }
  const buckets = new Map([
    [0, []],
    [1, []],
    [5, []],
    [10, []],
    [15, []]
  ]);
  for (const entry of entries) {
    if (scope && scope !== 'all') {
      if (scope === 'hack' && entry._source !== 'oracle-of-secrets') {
        continue;
      }
      if (scope === 'usdasm' && entry._source !== 'usdasm') {
        continue;
      }
    }
    const label = entry._label || '';
    const file = entry._file || '';
    const address = entry._address || '';
    let score = 100;
    let matched = false;
    if (label === trimmed) {
      score = 0;
      matched = true;
    } else if (label.startsWith(trimmed)) {
      score = 1;
      matched = true;
    } else if (label.includes(trimmed)) {
      score = 5;
      matched = true;
    } else if (file.includes(trimmed)) {
      score = 10;
      matched = true;
    } else if (address.includes(trimmed)) {
      score = 15;
      matched = true;
    }
    if (!matched) {
      continue;
    }
    const bucket = buckets.get(score);
    if (bucket) {
      bucket.push(entry);
    }
  }
  const orderedScores = [0, 1, 5, 10, 15];
  const results = [];
  for (const score of orderedScores) {
    const bucket = buckets.get(score) || [];
    if (bucket.length > 1) {
      bucket.sort((a, b) => (a.label || '').localeCompare(b.label || ''));
    }
    for (const entry of bucket) {
      results.push(entry);
      if (results.length >= limit) {
        return results;
      }
    }
  }
  return results;
}

function getSupportedLanguageIds(config) {
  const configured = config ? config.get('languageIds') : undefined;
  if (Array.isArray(configured)) {
    const cleaned = configured
      .map(entry => (typeof entry === 'string' ? entry.trim() : ''))
      .filter(Boolean);
    if (cleaned.length > 0) {
      return cleaned;
    }
  }
  return [...DEFAULT_LANGUAGE_IDS];
}

function isSupportedLanguage(languageId, config) {
  if (!languageId) {
    return false;
  }
  const supported = getSupportedLanguageIds(config);
  return supported.includes(languageId);
}

function getEditorInfo() {
  const name = vscode.env.appName || 'VS Code';
  const host = vscode.env.appHost || '';
  return {
    name,
    host,
    label: host ? `${name} (${host})` : name
  };
}

function updateLanguageContext(editor) {
  const config = vscode.workspace.getConfiguration('z3dk');
  const activeId = editor ? editor.document.languageId : '';
  const supported = isSupportedLanguage(activeId, config);
  vscode.commands.executeCommand('setContext', 'z3dk.isAsmLanguage', supported);
}

function z3dkRoot(context) {
  const z3dkFolder = findWorkspaceFolder('z3dk');
  if (z3dkFolder) {
    return z3dkFolder.uri.fsPath;
  }
  const extensionRoot = path.resolve(context.extensionPath, '..', '..');
  if (fs.existsSync(path.join(extensionRoot, 'README.md'))) {
    return extensionRoot;
  }
  const sibling = workspaceSibling('z3dk');
  if (sibling && fs.existsSync(path.join(sibling, 'README.md'))) {
    return sibling;
  }
  return workspaceRoot();
}

function resolveModelCatalogPath(config) {
  return resolveConfigPath(config, 'modelCatalogPath', [
    path.join(os.homedir(), 'src', 'docs', 'MODEL_CATALOG.md')
  ]);
}

function resolveModelPortfolioPath(config) {
  return resolveConfigPath(config, 'modelPortfolioPath', [
    path.join(os.homedir(), 'src', 'lab', 'afs-scawful', 'docs', 'MODEL_PORTFOLIO.md')
  ]);
}

function resolveContinueConfigPath(config) {
  return resolveConfigPath(config, 'continueConfigPath', [
    path.join(os.homedir(), '.continue', 'config.yaml')
  ]);
}

function resolveContinueConfigTsPath(config) {
  return resolveConfigPath(config, 'continueConfigTsPath', [
    path.join(os.homedir(), '.continue', 'config.ts')
  ]);
}

function resolveServerPath(config, rootDir) {
  const explicit = expandHome(config.get('serverPath'));
  if (explicit && fs.existsSync(explicit)) {
    return explicit;
  }
  if (process.env.Z3LSP_PATH) {
    return process.env.Z3LSP_PATH;
  }
  if (!rootDir) {
    return 'z3lsp';
  }

  const candidates = [
    path.join(rootDir, 'build', 'z3lsp', 'z3lsp'),
    path.join(rootDir, 'build', 'src', 'z3lsp', 'z3lsp'),
    path.join(rootDir, 'build-z3dk-foundation', 'z3lsp', 'z3lsp'),
    path.join(rootDir, 'build-z3dk-foundation', 'src', 'z3lsp', 'z3lsp'),
    path.join(rootDir, 'build-z3dk-asan', 'z3lsp', 'z3lsp'),
    path.join(rootDir, 'build-z3dk-asan', 'src', 'z3lsp', 'z3lsp'),
    path.join(rootDir, 'build', 'bin', 'z3lsp')
  ];

  for (const candidate of candidates) {
    if (fs.existsSync(candidate)) {
      return candidate;
    }
  }

  return 'z3lsp';
}

function resolveDisasmExecutable(rootDir) {
  if (!rootDir) {
    return '';
  }
  const candidates = [
    path.join(rootDir, 'build', 'z3disasm', 'z3disasm'),
    path.join(rootDir, 'build', 'src', 'z3disasm', 'z3disasm'),
    path.join(rootDir, 'build-z3dk-foundation', 'z3disasm', 'z3disasm'),
    path.join(rootDir, 'build-z3dk-foundation', 'src', 'z3disasm', 'z3disasm'),
    path.join(rootDir, 'build-z3dk-asan', 'z3disasm', 'z3disasm'),
    path.join(rootDir, 'build-z3dk-asan', 'src', 'z3disasm', 'z3disasm'),
    path.join(rootDir, 'build', 'bin', 'z3disasm'),
    path.join(rootDir, 'build-z3dk-foundation', 'bin', 'z3disasm')
  ];
  for (const candidate of candidates) {
    if (fs.existsSync(candidate)) {
      return candidate;
    }
  }
  return '';
}

function ensureOutputChannel() {
  if (!outputChannel) {
    outputChannel = vscode.window.createOutputChannel('Z3DK');
  }
  return outputChannel;
}

function describeClient(value) {
  if (!value) {
    return 'none';
  }
  const type = typeof value;
  const ctor = value && value.constructor ? value.constructor.name : 'unknown';
  const keys = [];
  try {
    keys.push(...Object.keys(value).slice(0, 10));
  } catch (_) {
    // ignore
  }
  return `${type} ${ctor} keys=[${keys.join(', ')}]`;
}

async function resolveClientValue(value) {
  if (value && typeof value.then === 'function') {
    try {
      return await value;
    } catch (err) {
      ensureOutputChannel().appendLine(`[Z3DK] Failed to resolve client promise: ${err}`);
      return undefined;
    }
  }
  return value;
}

function loadLanguageClient() {
  if (LanguageClient) {
    return LanguageClient;
  }
  if (languageClientLoadError) {
    return null;
  }
  try {
    ({ LanguageClient } = require('vscode-languageclient/node'));
    return LanguageClient;
  } catch (err) {
    languageClientLoadError = err;
    const message = 'Z3DK: vscode-languageclient not found. Run npm install in extensions/vscode-z3dk.';
    ensureOutputChannel().appendLine(message);
    ensureOutputChannel().appendLine(String(err));
    vscode.window.showWarningMessage(message);
    return null;
  }
}

function ensureTerminal() {
  if (!terminal) {
    terminal = vscode.window.createTerminal('Z3DK');
  }
  return terminal;
}

function runInTerminal(command, cwd) {
  const term = ensureTerminal();
  if (cwd) {
    term.sendText(`cd "${cwd}"`);
  }
  term.sendText(command);
  term.show(true);
}

function pathStatus(value) {
  if (!value) {
    return { value: 'Not set', exists: false };
  }
  const expanded = expandHome(value);
  if (!path.isAbsolute(expanded)) {
    return { value: `${expanded} (PATH)`, exists: true };
  }
  return { value: expanded, exists: fs.existsSync(expanded) };
}

function ensureWorkspaceFolder(folderPath) {
  if (!folderPath) {
    return false;
  }
  const expanded = expandHome(folderPath);
  const folders = vscode.workspace.workspaceFolders || [];
  const exists = folders.some(folder => folder.uri.fsPath === expanded);
  if (!exists) {
    vscode.workspace.updateWorkspaceFolders(folders.length, null, { uri: vscode.Uri.file(expanded) });
  }
  return true;
}

async function revealFolder(folderPath) {
  const expanded = expandHome(folderPath);
  if (!expanded || !fs.existsSync(expanded)) {
    vscode.window.showWarningMessage(`Path not found: ${expanded || 'Unknown path'}`);
    return;
  }
  ensureWorkspaceFolder(expanded);
  await vscode.commands.executeCommand('revealInExplorer', vscode.Uri.file(expanded));
}

async function openFilePath(filePath) {
  const expanded = expandHome(filePath);
  if (!expanded || !fs.existsSync(expanded)) {
    vscode.window.showWarningMessage(`File not found: ${expanded || 'Unknown file'}`);
    return;
  }
  const doc = await vscode.workspace.openTextDocument(expanded);
  await vscode.window.showTextDocument(doc, { preview: false });
}

async function openLabelEntry(entry, config, context) {
  const rawFile = entry.file || '';
  let filePath = rawFile;
  if (!path.isAbsolute(rawFile)) {
    let root = '';
    if (entry.source_repo === 'usdasm') {
      root = resolveUsdasmRoot(config);
    } else if (entry.source_repo === 'oracle-of-secrets') {
      root = resolveRepoPath('oracle-of-secrets', 'oracleRoot', config);
    } else {
      root = z3dkRoot(context) || workspaceRoot() || '';
    }
    if (root) {
      filePath = path.join(root, rawFile);
    }
  }
  if (!filePath || !fs.existsSync(filePath)) {
    vscode.window.showWarningMessage(`Label file not found: ${filePath || rawFile}`);
    return;
  }
  const doc = await vscode.workspace.openTextDocument(filePath);
  const editor = await vscode.window.showTextDocument(doc, { preview: false });
  const line = Number.parseInt(entry.line || '0', 10);
  if (!Number.isNaN(line) && line > 0) {
    const position = new vscode.Position(line - 1, 0);
    editor.selection = new vscode.Selection(position, position);
    editor.revealRange(new vscode.Range(position, position), vscode.TextEditorRevealType.InCenter);
  }
}

function escapeHtml(value) {
  return String(value)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

function applyTemplate(value, replacements) {
  if (!value) {
    return '';
  }
  let output = value;
  for (const [key, replacement] of Object.entries(replacements)) {
    const safeValue = replacement || '';
    output = output.replace(new RegExp(`\\$\\{${key}\\}`, 'g'), safeValue);
  }
  return output;
}

function expandArgs(args, replacements) {
  if (!Array.isArray(args)) {
    return [];
  }
  return args.map(arg => applyTemplate(arg, replacements));
}

function buildLaunchCommand(executable, args) {
  const expanded = expandHome(executable || '');
  const quotedArgs = args
    .filter(arg => arg !== undefined && arg !== null && arg !== '')
    .map(arg => `"${arg}"`)
    .join(' ');
  if (!expanded) {
    return '';
  }
  if (expanded.endsWith('.app')) {
    return `open -a "${expanded}"${quotedArgs ? ` --args ${quotedArgs}` : ''}`;
  }
  const escapedExecutable = expanded.includes(' ') ? `"${expanded}"` : expanded;
  return `${escapedExecutable}${quotedArgs ? ` ${quotedArgs}` : ''}`;
}

function findExecutableInPath(name) {
  const pathEnv = process.env.PATH || '';
  if (!pathEnv) {
    return '';
  }
  const candidates = [name];
  if (process.platform === 'win32') {
    candidates.push(`${name}.exe`, `${name}.cmd`, `${name}.bat`);
  }
  for (const dir of pathEnv.split(path.delimiter)) {
    if (!dir) {
      continue;
    }
    for (const candidate of candidates) {
      const fullPath = path.join(dir, candidate);
      try {
        const stat = fs.statSync(fullPath);
        if (stat.isFile()) {
          return fullPath;
        }
      } catch {
        continue;
      }
    }
  }
  return '';
}

function resolveUsdasmRoot(config) {
  const configured = expandHome(config.get('usdasmRoot'));
  if (configured) {
    return configured;
  }
  const candidates = ['usdasm', 'alttp-usdasm', 'zelda3', 'alttp-disasm'];
  for (const name of candidates) {
    const folder = findWorkspaceFolder(name);
    if (folder) {
      return folder.uri.fsPath;
    }
  }
  for (const name of candidates) {
    const sibling = workspaceSibling(name);
    if (sibling && fs.existsSync(sibling)) {
      return sibling;
    }
  }
  const root = workspaceRoot();
  if (root) {
    const gigaleakPath = path.join(path.dirname(root), 'alttp-gigaleak', 'DISASM', 'usdasm');
    if (fs.existsSync(gigaleakPath)) {
      return gigaleakPath;
    }
  }
  return '';
}

function resolveYazeExecutable(config) {
  const configured = expandHome(config.get('yazePath'));
  if (configured) {
    return configured;
  }
  const pathCandidate = findExecutableInPath('yaze-nightly') || findExecutableInPath('yaze');
  if (pathCandidate) {
    return pathCandidate;
  }
  const yazeRoot = resolveRepoPath('yaze', 'yazeRoot', config);
  if (!yazeRoot) {
    return '';
  }
  const candidates = [
    path.join('build-nightly', 'bin', 'yaze.app'),
    path.join('build-nightly', 'bin', 'yaze.app', 'Contents', 'MacOS', 'yaze'),
    path.join('build_ai', 'bin', 'Debug', 'yaze.app'),
    path.join('build_ai', 'bin', 'Debug', 'yaze.app', 'Contents', 'MacOS', 'yaze'),
    path.join('build', 'bin', 'yaze.app'),
    path.join('build', 'bin', 'yaze.app', 'Contents', 'MacOS', 'yaze'),
    path.join('build', 'bin', 'yaze')
  ];
  for (const candidate of candidates) {
    const fullPath = path.join(yazeRoot, candidate);
    if (fs.existsSync(fullPath)) {
      return fullPath;
    }
  }
  return '';
}

function resolveMesenExecutable(config) {
  const configured = expandHome(config.get('mesenPath'));
  if (configured) {
    return configured;
  }
  const mesenRoot = resolveRepoPath('mesen2-oos', 'mesenRoot', config);
  if (!mesenRoot) {
    return '';
  }
  const candidates = [
    'Mesen2.app',
    'Mesen2-OOS.app',
    'Mesen.app',
    path.join('build', 'Mesen2.app'),
    path.join('build', 'Mesen2-OOS.app'),
    path.join('build', 'Mesen.app')
  ];
  for (const candidate of candidates) {
    const fullPath = path.join(mesenRoot, candidate);
    if (fs.existsSync(fullPath)) {
      return fullPath;
    }
  }
  return '';
}

function buildDisassemblyCommand(config, context, romInfo) {
  const resolvedRomInfo = romInfo || resolveRomInfo(config);
  const romPath = resolvedRomInfo.path || '';
  const tomlConfig = resolvedRomInfo.toml;
  const symbolsPath = resolveSymbolsPath(config, romPath, tomlConfig);
  const outputPath = expandHome(config.get('disasmOutputPath')) || '';
  const usdasmRoot = resolveUsdasmRoot(config);
  const labelMapPath = resolveLabelMapPath(config, context);
  const command = config.get('disasmCommand');
  if (command) {
    return applyTemplate(command, {
      rom: romPath,
      symbols: symbolsPath,
      output: outputPath,
      usdasm: usdasmRoot,
      labels: labelMapPath
    });
  }
  const rootDir = z3dkRoot(context) || workspaceRoot();
  const disasmExe = resolveDisasmExecutable(rootDir);
  if (!disasmExe || !romPath) {
    return '';
  }
  const fallbackOut = outputPath || (rootDir ? path.join(rootDir, '.cache', 'z3disasm') : '');
  const args = ['--rom', romPath, '--out', fallbackOut];
  if (symbolsPath) {
    args.push('--symbols', symbolsPath);
  }
  if (labelMapPath && fs.existsSync(labelMapPath)) {
    args.push('--labels', labelMapPath);
  }
  return buildLaunchCommand(disasmExe, args);
}

function resolveLabelIndexCommand(config, context) {
  const rootDir = z3dkRoot(context) || workspaceRoot();
  const configured = expandHome(config.get('labelIndexCommand'));
  if (configured) {
    return applyTemplate(configured, {
      workspaceFolder: rootDir || '',
      workspaceRoot: rootDir || '',
      z3dk: rootDir || ''
    });
  }
  if (!rootDir) {
    return '';
  }
  const scriptPath = path.join(rootDir, 'scripts', 'generate_label_indexes.py');
  if (!fs.existsSync(scriptPath)) {
    return '';
  }
  return `python3 "${scriptPath}"`;
}

function runLabelIndexCommand(context, reason) {
  const config = vscode.workspace.getConfiguration('z3dk');
  const command = resolveLabelIndexCommand(config, context);
  if (!command) {
    ensureOutputChannel().appendLine('[Z3DK] Label index command not configured.');
    return;
  }
  if (labelIndexInProgress) {
    labelIndexQueued = true;
    return;
  }
  labelIndexInProgress = true;
  const rootDir = z3dkRoot(context) || workspaceRoot();
  const output = ensureOutputChannel();
  output.appendLine(`[Z3DK] Generating label index${reason ? ` (${reason})` : ''}...`);
  exec(command, { cwd: rootDir || undefined }, (error, stdout, stderr) => {
    if (stdout) {
      output.appendLine(stdout.trim());
    }
    if (stderr) {
      output.appendLine(stderr.trim());
    }
    if (error) {
      output.appendLine(`[Z3DK] Label index generation failed: ${error.message}`);
    } else {
      output.appendLine('[Z3DK] Label index generation complete.');
    }
    labelIndexInProgress = false;
    labelIndexCache.clear();
    if (labelSearchProvider) {
      labelSearchProvider.refresh();
    }
    if (dashboardProvider) {
      dashboardProvider.refresh();
    }
    updateStatusBar(context);
    if (labelIndexQueued) {
      labelIndexQueued = false;
      scheduleLabelIndexRefresh(context, 'queued');
    }
  });
}

function scheduleLabelIndexRefresh(context, reason) {
  const config = vscode.workspace.getConfiguration('z3dk');
  if (!config.get('autoGenerateLabelIndex')) {
    return;
  }
  if (!resolveLabelIndexCommand(config, context)) {
    return;
  }
  const debounce = Math.max(0, Number(config.get('labelIndexDebounceMs')) || 8000);
  if (labelIndexRefreshTimer) {
    clearTimeout(labelIndexRefreshTimer);
  }
  labelIndexRefreshTimer = setTimeout(() => {
    runLabelIndexCommand(context, reason);
  }, debounce);
}

function resolveYazeLogPath(config) {
  const configured = expandHome(config.get('yazeLogPath'));
  if (configured) {
    return configured;
  }
  const root = z3dkRoot(extensionContext) || workspaceRoot();
  if (root) {
    return path.join(root, '.cache', 'z3dk', 'yaze.log');
  }
  return path.join(os.homedir(), '.cache', 'z3dk', 'yaze.log');
}

function wrapYazeCommand(command, config) {
  if (!config.get('yazeQuiet')) {
    return command;
  }
  const logPath = resolveYazeLogPath(config);
  if (logPath) {
    try {
      fs.mkdirSync(path.dirname(logPath), { recursive: true });
    } catch (err) {
      ensureOutputChannel().appendLine(`[Z3DK] Unable to create yaze log dir: ${err}`);
      return command;
    }
    return `${command} > "${logPath}" 2>&1`;
  }
  return command;
}

function resolveDisassemblyFolder(config) {
  const outputPath = expandHome(config.get('disasmOutputPath'));
  if (!outputPath || !fs.existsSync(outputPath)) {
    return '';
  }
  const stats = fs.statSync(outputPath);
  return stats.isDirectory() ? outputPath : path.dirname(outputPath);
}

function ensureStatusBar(context) {
  if (statusItems.length > 0) {
    return;
  }
  const main = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 100);
  const lsp = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 99);
  const rom = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 98);

  main.command = 'z3dk.openDashboard';
  lsp.command = 'z3dk.restartServer';
  rom.command = 'z3dk.openRomFolder';

  statusItems = [main, lsp, rom];
  statusItems.forEach(item => context.subscriptions.push(item));
}

function updateStatusBar(context) {
  ensureStatusBar(context);
  const config = vscode.workspace.getConfiguration('z3dk');
  const romInfo = resolveRomInfo(config);
  const romPath = romInfo.path || '';
  const symbolsPath = resolveSymbolsPath(config, romPath, romInfo.toml);
  const lspLabel = client ? 'on' : 'off';
  const editorInfo = getEditorInfo();

  const [main, lsp, rom] = statusItems;
  main.text = '$(triangle-up) Z3DK';
  const mainAsm = resolveMainAsmPath(romInfo.toml);
  main.tooltip = mainAsm
    ? `Open Z3DK dashboard\nMain: ${mainAsm}\nEditor: ${editorInfo.label}`
    : `Open Z3DK dashboard\nEditor: ${editorInfo.label}`;
  main.show();

  lsp.text = `$(pulse) LSP ${lspLabel}`;
  lsp.tooltip = 'Restart z3lsp';
  lsp.show();

  if (romPath) {
    const romName = path.basename(romPath);
    const symReady = symbolsPath && fs.existsSync(symbolsPath) ? 'ok' : 'miss';
    const sourceLabel = romInfo.source || 'config';
    rom.text = `$(circuit-board) ${romName} • ${sourceLabel} • sym ${symReady}`;
    rom.tooltip = `ROM: ${romPath}\nSource: ${sourceLabel}\nSymbols: ${symbolsPath || 'unset'}`;
  } else {
    rom.text = '$(circuit-board) ROM unset';
    rom.tooltip = 'Set z3dk.romPath or z3dk.toml rom_path';
  }
  rom.show();
}

function setLspActive(active) {
  lspActive = active;
  vscode.commands.executeCommand('setContext', 'z3dk.lspActive', active);
}

class CommandItem extends vscode.TreeItem {
  constructor(label, command, tooltip, iconId, kind) {
    super(label, vscode.TreeItemCollapsibleState.None);
    this.command = command;
    this.tooltip = tooltip || label;
    if (kind) {
      this.description = kind;
    }
    if (iconId) {
      this.iconPath = new vscode.ThemeIcon(iconId);
    }
  }
}

class CommandGroupItem extends vscode.TreeItem {
  constructor(label, children, iconId, tooltip) {
    super(label, vscode.TreeItemCollapsibleState.Expanded);
    this.children = children;
    if (iconId) {
      this.iconPath = new vscode.ThemeIcon(iconId);
    }
    if (tooltip) {
      this.tooltip = tooltip;
    }
  }
}

class Z3dkCommandProvider {
  constructor(context) {
    this.context = context;
    this._onDidChangeTreeData = new vscode.EventEmitter();
    this.onDidChangeTreeData = this._onDidChangeTreeData.event;
  }

  refresh() {
    this._onDidChangeTreeData.fire();
  }

  getTreeItem(element) {
    return element;
  }

  getChildren(element) {
    if (!element) {
      return this.buildItems();
    }
    if (element.children) {
      return element.children;
    }
    return [];
  }

  buildItems() {
    const makeCommand = (label, commandId, tooltip, iconId, kind) =>
      new CommandItem(label, { command: commandId, title: label }, tooltip, iconId, kind);

    const makeGroup = (label, children, iconId, tooltip) => new CommandGroupItem(label, children, iconId, tooltip);

    return [
      makeGroup('Backend (server)', [
        makeCommand(
          'Build Z3DK',
          'z3dk.build',
          'Run the configured build command',
          'tools',
          'Server'
        ),
        makeCommand(
          'Run Tests',
          'z3dk.runTests',
          'Run regression tests',
          'beaker',
          'Server'
        ),
        makeCommand(
          'Export Symbols',
          'z3dk.exportSymbols',
          'Export Mesen .mlb symbols via yaze',
          'database',
          'Server'
        ),
        makeCommand(
          'Export Hack Disasm',
          'z3dk.exportDisassembly',
          'Export USDASM-style disassembly of the hack',
          'symbol-structure',
          'Server'
        ),
        makeCommand(
          'Generate Label Index',
          'z3dk.generateLabelIndex',
          'Regenerate label indexes',
          'refresh',
          'Server'
        ),
        makeCommand(
          'Restart Language Server',
          'z3dk.restartServer',
          'Restart z3lsp',
          'pulse',
          'Server'
        )
      ], 'server', 'Commands that talk to z3lsp or run build/test tools'),
      makeGroup('ROM & Project', [
        makeCommand(
          'Select Project Root',
          'z3dk.selectProject',
          'Pick the active Z3DK project root',
          'target',
          'Select'
        ),
        makeCommand(
          'Select ROM',
          'z3dk.selectRom',
          'Pick a ROM for z3dk commands',
          'file',
          'Select'
        ),
        makeCommand(
          'Open Main ASM',
          'z3dk.openMainAsm',
          'Open the main ASM entrypoint from z3dk.toml',
          'file-code',
          'Open'
        ),
        makeCommand(
          'Open ROM Folder',
          'z3dk.openRomFolder',
          'Reveal ROM folder in Explorer',
          'file-submodule',
          'Open'
        ),
        makeCommand(
          'Open ROM Map',
          'z3dk.openRomMap',
          'Open the ROM map view',
          'map',
          'Panel'
        ),
        makeCommand(
          'Open Yaze Log',
          'z3dk.openYazeLog',
          'Open the yaze log file',
          'output',
          'Open'
        )
      ], 'settings-gear', 'Project + ROM configuration and navigation'),
      makeGroup('Disassembly & Labels', [
        makeCommand(
          'Open Label Search Panel',
          'z3dk.openLabelSearch',
          'Open the label search panel',
          'symbol-keyword',
          'Panel'
        ),
        makeCommand(
          'Search Labels',
          'z3dk.findLabel',
          'Search hack + USDASM labels',
          'search',
          'Search'
        ),
        makeCommand(
          'Search USDASM Label',
          'z3dk.findUsdasmLabel',
          'Search USDASM for a label',
          'search',
          'Search'
        ),
        makeCommand(
          'Open USDASM Root',
          'z3dk.openUsdasmRoot',
          'Open USDASM source-of-truth disassembly',
          'folder',
          'Open'
        ),
        makeCommand(
          'Open Latest Disasm',
          'z3dk.openLatestDisassemblyFile',
          'Open the most recently generated bank_XX.asm',
          'history',
          'Open'
        ),
        makeCommand(
          'Open Disasm Output',
          'z3dk.openDisassemblyOutput',
          'Open the disassembly output folder',
          'folder-opened',
          'Open'
        ),
        makeCommand(
          'Open Disasm File',
          'z3dk.openDisassemblyFile',
          'Open a disassembly file from the output folder',
          'file',
          'Open'
        )
      ], 'symbol-structure', 'Disassembly outputs and label search helpers'),
      makeGroup('Emulators & Tools', [
        makeCommand(
          'Launch Mesen2',
          'z3dk.launchMesen',
          'Launch Mesen2-OOS with optional ROM args',
          'debug-alt-small',
          'Launch'
        ),
        makeCommand(
          'Launch yaze',
          'z3dk.launchYaze',
          'Launch yaze with optional ROM args',
          'rocket',
          'Launch'
        )
      ], 'debug-alt', 'External tools and emulators'),
      makeGroup('Workspace & Resources', [
        makeCommand(
          'Open Workspace',
          'z3dk.openWorkspace',
          'Open the multi-root workspace for Oracle + Yaze + Z3DK',
          'file-directory',
          'Open'
        ),
        makeCommand(
          'Open Dashboard',
          'z3dk.openDashboard',
          'Open the Z3DK dashboard view',
          'layout',
          'Panel'
        ),
        makeCommand(
          'Open README',
          'z3dk.openReadme',
          'Open Z3DK README',
          'book',
          'Open'
        ),
        makeCommand(
          'Open Oracle Repo',
          'z3dk.openOracleRepo',
          'Open the Oracle of Secrets repo',
          'repo',
          'Open'
        ),
        makeCommand(
          'Open Yaze Repo',
          'z3dk.openYazeRepo',
          'Open the yaze repo',
          'repo',
          'Open'
        ),
        makeCommand(
          'Open Mesen2 Repo',
          'z3dk.openMesenRepo',
          'Open the Mesen2-OOS repo',
          'repo',
          'Open'
        ),
        makeCommand(
          'Open Model Catalog',
          'z3dk.openModelCatalog',
          'Open the Zelda model catalog',
          'library',
          'Open'
        ),
        makeCommand(
          'Open Model Portfolio',
          'z3dk.openModelPortfolio',
          'Open the AFS model portfolio',
          'map',
          'Open'
        ),
        makeCommand(
          'Open Continue Config (TS)',
          'z3dk.openContinueConfigTs',
          'Open Continue config.ts',
          'gear',
          'Open'
        ),
        makeCommand(
          'Open Continue Config (YAML)',
          'z3dk.openContinueConfig',
          'Open Continue config.yaml',
          'gear',
          'Open'
        ),
        makeCommand(
          'Open AFS Scratchpad',
          'z3dk.openAfsScratchpad',
          'Open the z3dk AFS scratchpad',
          'note',
          'Open'
        ),
        makeCommand(
          'Add AFS Contexts',
          'z3dk.addAfsContexts',
          'Add .context folders to the workspace',
          'root-folder',
          'Workspace'
        )
      ], 'book', 'Docs, repos, and dev resources')
    ];
  }
}

class Z3dkDashboardProvider {
  constructor(context) {
    this.context = context;
    this.view = undefined;
  }

  resolveWebviewView(webviewView) {
    this.view = webviewView;
    webviewView.webview.options = {
      enableScripts: true,
      localResourceRoots: [vscode.Uri.file(this.context.extensionPath)]
    };

    webviewView.webview.onDidReceiveMessage(message => {
      if (!message || typeof message.command !== 'string') {
        return;
      }
      const allowed = new Set([
        'z3dk.openWorkspace',
        'z3dk.openReadme',
        'z3dk.build',
        'z3dk.runTests',
        'z3dk.exportSymbols',
        'z3dk.restartServer',
        'z3dk.refreshDashboard',
        'z3dk.openDashboard',
        'z3dk.openModelCatalog',
        'z3dk.openModelPortfolio',
        'z3dk.openContinueConfig',
        'z3dk.openContinueConfigTs',
        'z3dk.openAfsScratchpad',
        'z3dk.addAfsContexts',
        'z3dk.openOracleRepo',
        'z3dk.openYazeRepo',
        'z3dk.openMesenRepo',
        'z3dk.openUsdasmRoot',
        'z3dk.openRomMap',
        'z3dk.openLabelSearch',
        'z3dk.findLabel',
        'z3dk.findUsdasmLabel',
        'z3dk.exportDisassembly',
        'z3dk.generateLabelIndex',
        'z3dk.openDisassemblyOutput',
        'z3dk.openDisassemblyFile',
        'z3dk.openLatestDisassemblyFile',
        'z3dk.openYazeLog',
        'z3dk.launchMesen',
        'z3dk.launchYaze',
        'z3dk.openRomFolder',
        'z3dk.openMainAsm',
        'z3dk.selectProject',
        'z3dk.selectRom'
      ]);
      if (allowed.has(message.command)) {
        vscode.commands.executeCommand(message.command);
      }
    });

    this.refresh();
  }

  refresh() {
    if (!this.view) {
      return;
    }
    this.view.webview.html = buildDashboardHtml(this.context);
  }
}

class Z3dkLabelSearchProvider {
  constructor(context) {
    this.context = context;
    this.view = undefined;
    this.lastQuery = '';
    this.lastScope = 'all';
  }

  resolveWebviewView(webviewView) {
    this.view = webviewView;
    webviewView.webview.options = {
      enableScripts: true,
      localResourceRoots: [vscode.Uri.file(this.context.extensionPath)]
    };
    webviewView.webview.onDidReceiveMessage(message => {
      if (!message || typeof message.command !== 'string') {
        return;
      }
      if (message.command === 'ready') {
        this.postStatus();
        this.postResults(this.lastQuery, this.lastScope);
        return;
      }
      if (message.command === 'search') {
        this.lastQuery = message.query || '';
        this.lastScope = message.scope || 'all';
        this.postResults(this.lastQuery, this.lastScope);
        return;
      }
      if (message.command === 'open') {
        const config = vscode.workspace.getConfiguration('z3dk');
        openLabelEntry(message.entry || {}, config, this.context);
        return;
      }
      if (message.command === 'refresh') {
        labelIndexCache.clear();
        this.postStatus();
        this.postResults(this.lastQuery, this.lastScope);
        return;
      }
      if (message.command === 'regen') {
        runLabelIndexCommand(this.context, 'manual');
        return;
      }
    });
    this.refresh();
  }

  refresh() {
    if (!this.view) {
      return;
    }
    this.view.webview.html = buildLabelSearchHtml(this.context);
    this.postStatus();
    this.postResults(this.lastQuery, this.lastScope);
  }

  postStatus() {
    if (!this.view) {
      return;
    }
    const config = vscode.workspace.getConfiguration('z3dk');
    const indexPath = resolveLabelIndex(config, this.context);
    const exists = indexPath && fs.existsSync(indexPath);
    const cache = exists ? getLabelIndexCache(indexPath) : null;
    const entries = cache ? cache.rows : [];
    this.view.webview.postMessage({
      command: 'status',
      indexPath: indexPath || '',
      exists,
      count: entries.length
    });
  }

  postResults(query, scope) {
    if (!this.view) {
      return;
    }
    const config = vscode.workspace.getConfiguration('z3dk');
    const indexPath = resolveLabelIndex(config, this.context);
    const cache = indexPath && fs.existsSync(indexPath) ? getLabelIndexCache(indexPath) : null;
    const entries = cache ? cache.rows : [];
    const limit = Math.max(20, Number(config.get('labelSearchLimit')) || 200);
    const trimmed = (query || '').trim().toLowerCase();
    const isLarge = entries.length > LABEL_SORT_LIMIT;
    const minChars = isLarge ? LARGE_LABEL_MIN_CHARS : 1;
    let results = [];
    let mode = 'search';
    if (!trimmed) {
      mode = isLarge ? 'defaultLarge' : 'default';
      if (cache && cache.sortedRows) {
        const scoped = filterEntriesByScope(cache.sortedRows, scope);
        results = scoped.slice(0, limit);
      } else if (entries.length > 0) {
        const previewLimit = Math.min(limit, LARGE_LABEL_PREVIEW_LIMIT);
        const scoped = filterEntriesByScope(entries, scope);
        results = scoped.slice(0, previewLimit);
      }
    } else if (trimmed.length < minChars) {
      mode = 'short';
      results = [];
    } else {
      results = searchLabelEntries(entries, query, scope, limit);
    }
    results = results.map(entry => ({
      label: entry.label || '',
      address: entry.address || '',
      file: entry.file || '',
      line: entry.line || '',
      source_repo: entry.source_repo || ''
    }));
    this.view.webview.postMessage({
      command: 'results',
      query,
      scope,
      results,
      mode
    });
  }
}

class Z3dkRomMapProvider {
  constructor(context) {
    this.context = context;
    this.view = undefined;
  }

  resolveWebviewView(webviewView) {
    this.view = webviewView;
    webviewView.webview.options = {
      enableScripts: true,
      localResourceRoots: [vscode.Uri.file(this.context.extensionPath)]
    };
    webviewView.webview.onDidReceiveMessage(async message => {
      if (!message || typeof message.command !== 'string') {
        return;
      }
      if (message.command === 'ready' || message.command === 'refresh') {
        this.updateData();
        return;
      }
      if (message.command === 'openMainAsm') {
        vscode.commands.executeCommand('z3dk.openMainAsm');
        return;
      }
      if (message.command === 'selectRom') {
        vscode.commands.executeCommand('z3dk.selectRom');
        return;
      }
    });
    this.refresh();
  }

  async updateData() {
    if (!this.view) {
      return;
    }
    let activeClient = client;
    activeClient = await resolveClientValue(activeClient);
    if (!activeClient) {
      activeClient = await startClient(this.context);
    }
    activeClient = await resolveClientValue(activeClient);
    if (!activeClient) {
      this.view.webview.postMessage({
        command: 'status',
        state: 'inactive',
        message: 'Z3LSP is not running. Enable z3dk.autoStart or start the server.'
      });
      return;
    }
    if (typeof activeClient.start !== 'function' || typeof activeClient.sendRequest !== 'function') {
      ensureOutputChannel().appendLine(`[Z3DK] LSP client is invalid; restarting. ${describeClient(activeClient)}`);
      await restartClient(this.context);
      activeClient = client;
    }
    if (!activeClient || typeof activeClient.start !== 'function' || typeof activeClient.sendRequest !== 'function') {
      ensureOutputChannel().appendLine(`[Z3DK] LSP client still invalid after restart. ${describeClient(activeClient)}`);
      this.view.webview.postMessage({
        command: 'status',
        state: 'error',
        message: 'Z3LSP client invalid. Reload the window or reinstall the extension.'
      });
      return;
    }
    try {
      await activeClient.start();
    } catch (err) {
      ensureOutputChannel().appendLine(`[Z3DK] LSP failed to start: ${err}`);
      this.view.webview.postMessage({
        command: 'status',
        state: 'error',
        message: 'Z3LSP failed to start. Check the Output panel.'
      });
      return;
    }
    try {
      const blocks = await activeClient.sendRequest('workspace/executeCommand', {
        command: 'z3dk.getBankUsage',
        arguments: []
      });
      this.view.webview.postMessage({
        command: 'data',
        blocks: blocks || []
      });
      this.view.webview.postMessage({
        command: 'status',
        state: 'ready'
      });
    } catch (err) {
      ensureOutputChannel().appendLine(`[Z3DK] Failed to get bank usage: ${err}`);
      this.view.webview.postMessage({
        command: 'status',
        state: 'error',
        message: 'Failed to load ROM map. Check the Output panel.'
      });
    }
  }

  refresh() {
    if (!this.view) {
      return;
    }
    this.view.webview.html = buildRomMapHtml(this.context);
  }
}

function buildRomMapHtml(context) {
  return `
    <!DOCTYPE html>
    <html lang="en">
    <head>
      <meta charset="UTF-8" />
      <meta name="viewport" content="width=device-width, initial-scale=1.0" />
      <style>
        :root {
          --bg: var(--vscode-editor-background, #1e1e1e);
          --panel: var(--vscode-editorWidget-background, var(--vscode-sideBar-background, #252526));
          --ink: var(--vscode-editor-foreground, #d4d4d4);
          --subtle: var(--vscode-descriptionForeground, rgba(200, 200, 200, 0.7));
          --border: var(--vscode-editorWidget-border, rgba(255, 255, 255, 0.08));
          --brand: #4da3ff;
        }

        body {
          margin: 0;
          padding: 8px;
          color: var(--ink);
          font-family: var(--vscode-font-family, sans-serif);
          font-size: 11px;
          background: var(--bg);
        }

        .header {
          display: flex;
          justify-content: space-between;
          align-items: center;
          margin-bottom: 8px;
        }

        .title {
          font-size: 12px;
          font-weight: 600;
        }

        .actions {
          display: flex;
          gap: 4px;
          flex-wrap: wrap;
        }

        .action-link {
          appearance: none;
          cursor: pointer;
          background: transparent;
          border: 1px solid transparent;
          color: var(--vscode-textLink-foreground, #4da3ff);
          padding: 2px 6px;
          border-radius: 999px;
          font-size: 9px;
          text-transform: uppercase;
          letter-spacing: 0.4px;
          display: inline-flex;
          align-items: center;
          gap: 6px;
        }

        .action-link .icon {
          border: 1px solid var(--border);
          border-radius: 4px;
          padding: 1px 4px;
          font-size: 8px;
          color: var(--ink);
          background: rgba(255, 255, 255, 0.04);
        }

        .action-link:hover {
          border-color: rgba(77, 163, 255, 0.35);
          background: rgba(77, 163, 255, 0.08);
        }

        .action-link:focus-visible {
          outline: 1px solid rgba(77, 163, 255, 0.6);
          outline-offset: 2px;
        }

        .summary {
          margin-bottom: 6px;
          color: var(--subtle);
        }

        .status {
          margin-bottom: 6px;
          padding: 4px 6px;
          border: 1px solid var(--border);
          border-radius: 4px;
          color: var(--subtle);
        }

        .status.ok {
          color: var(--ink);
          border-color: rgba(63, 185, 154, 0.3);
          background: rgba(63, 185, 154, 0.12);
        }

        .status.warn {
          color: var(--ink);
          border-color: rgba(208, 138, 74, 0.3);
          background: rgba(208, 138, 74, 0.12);
        }

        .bank-grid {
          display: grid;
          grid-template-columns: repeat(8, 1fr);
          gap: 2px;
        }

        .bank {
          aspect-ratio: 1;
          background: var(--panel);
          border: 1px solid var(--border);
          position: relative;
          cursor: help;
        }

        .bank-usage {
          position: absolute;
          bottom: 0;
          left: 0;
          right: 0;
          background: var(--brand);
          opacity: 0.6;
          transition: height 0.3s ease;
        }

        .bank-label {
          position: absolute;
          top: 50%;
          left: 50%;
          transform: translate(-50%, -50%);
          font-size: 8px;
          color: var(--ink);
          pointer-events: none;
        }

        .bank.selected {
          outline: 2px solid var(--brand);
          outline-offset: -1px;
        }

        .tooltip {
          margin-top: 12px;
          padding: 8px;
          background: var(--panel);
          border: 1px solid var(--border);
          border-radius: 4px;
          display: none;
        }

        .details {
          margin-top: 8px;
          padding: 8px;
          background: var(--panel);
          border: 1px solid var(--border);
          border-radius: 4px;
        }

        .details h3 {
          margin: 0 0 6px 0;
          font-size: 11px;
        }

        .details .meta {
          color: var(--subtle);
          margin-bottom: 6px;
        }

        .detail-row {
          display: grid;
          grid-template-columns: 70px 70px 1fr;
          gap: 6px;
          padding: 2px 0;
          border-bottom: 1px solid var(--border);
          font-family: var(--vscode-editor-font-family, monospace);
          font-size: 10px;
        }

        .detail-row:last-child {
          border-bottom: none;
        }
      </style>
    </head>
    <body>
      <div class="header">
        <div class="title">ROM Bank Usage</div>
        <div class="actions">
          <button class="action-link" id="selectRom" title="Select ROM">
            <span class="icon">ROM</span>
            <span class="label">Select</span>
          </button>
          <button class="action-link" id="openMainAsm" title="Open main ASM">
            <span class="icon">ASM</span>
            <span class="label">Open</span>
          </button>
          <button class="action-link" id="refresh" title="Refresh ROM map">
            <span class="icon">REF</span>
            <span class="label">Refresh</span>
          </button>
        </div>
      </div>
      <div class="status" id="status">Waiting for z3lsp…</div>
      <div class="summary" id="summary">No data yet.</div>
      <div class="bank-grid" id="grid">
        <!-- Banks will be generated here -->
      </div>
      <div class="tooltip" id="tooltip">
        Type to see details
      </div>
      <div class="details" id="details">
        <h3>Bank details</h3>
        <div class="meta">Select a bank to see its blocks.</div>
      </div>

      <script>
        const vscode = acquireVsCodeApi();
        const grid = document.getElementById('grid');
        const tooltip = document.getElementById('tooltip');
        const refresh = document.getElementById('refresh');
        const statusEl = document.getElementById('status');
        const summaryEl = document.getElementById('summary');
        const detailsEl = document.getElementById('details');
        const selectRomBtn = document.getElementById('selectRom');
        const openMainAsmBtn = document.getElementById('openMainAsm');
        let selectedBank = 0;
        let bankBlocks = new Map();
        let allBlocks = [];

        // Generate 64 banks (standard 2MB LoROM)
        for (let i = 0; i < 64; i++) {
          const bank = document.createElement('div');
          bank.className = 'bank';
          bank.id = 'bank-' + i;
          bank.innerHTML = '<div class="bank-usage" style="height: 0%"></div><div class="bank-label">' + i.toString(16).toUpperCase().padStart(2, '0') + '</div>';
          grid.appendChild(bank);
        }

        refresh.addEventListener('click', () => {
          vscode.postMessage({ command: 'refresh' });
        });
        selectRomBtn.addEventListener('click', () => {
          vscode.postMessage({ command: 'selectRom' });
        });
        openMainAsmBtn.addEventListener('click', () => {
          vscode.postMessage({ command: 'openMainAsm' });
        });

        function toHex(value, width) {
          if (typeof value !== 'number') {
            return '0x0';
          }
          return '0x' + value.toString(16).toUpperCase().padStart(width, '0');
        }

        function setStatus(state, message) {
          const text = message || (state === 'ready' ? 'ROM map ready.' : 'Waiting for z3lsp…');
          statusEl.textContent = text;
          statusEl.className = 'status ' + (state === 'ready' ? 'ok' : 'warn');
        }

        function rebuildIndex(blocks) {
          bankBlocks = new Map();
          blocks.forEach(block => {
            const bank = (block.snes >>> 16) & 0x3F;
            if (!bankBlocks.has(bank)) {
              bankBlocks.set(bank, []);
            }
            bankBlocks.get(bank).push(block);
          });
        }

        function updateSummary() {
          let total = 0;
          bankBlocks.forEach(entries => {
            entries.forEach(block => {
              total += block.size || 0;
            });
          });
          summaryEl.textContent = allBlocks.length
            ? ('Blocks: ' + allBlocks.length + ' • Used: ' + total + ' bytes')
            : 'No blocks yet. Build or open a main ASM file.';
        }

        function updateGrid(blocks) {
          const usageMap = new Array(64).fill(0);
          blocks.forEach(block => {
            const bank = (block.snes >>> 16) & 0x3F;
            usageMap[bank] += block.size || 0;
          });
          usageMap.forEach((used, i) => {
            const el = document.getElementById('bank-' + i);
            const bar = el.querySelector('.bank-usage');
            const percent = Math.min(100, (used / 32768) * 100);
            bar.style.height = percent + '%';
            el.dataset.used = String(used);
          });
        }

        function renderDetails(bank) {
          const blocks = bankBlocks.get(bank) || [];
          let total = 0;
          blocks.forEach(block => {
            total += block.size || 0;
          });
          const percent = Math.min(100, (total / 32768) * 100);
          const bankHex = bank.toString(16).toUpperCase().padStart(2, '0');
          let html = '<h3>Bank $' + bankHex + '</h3>';
          html += '<div class="meta">Blocks: ' + blocks.length + ' • Used: ' + total +
                  ' bytes (' + percent.toFixed(1) + '%)</div>';
          if (!blocks.length) {
            html += '<div class="meta">No blocks recorded for this bank.</div>';
          } else {
            const sorted = blocks.slice().sort((a, b) => (b.size || 0) - (a.size || 0));
            sorted.slice(0, 24).forEach(block => {
              html += '<div class="detail-row">' +
                      '<div>' + toHex(block.snes, 6) + '</div>' +
                      '<div>' + (block.size || 0) + ' B</div>' +
                      '<div>' + toHex(block.pc, 6) + '</div>' +
                      '</div>';
            });
            if (blocks.length > 24) {
              html += '<div class="meta">Showing 24 of ' + blocks.length + ' blocks.</div>';
            }
          }
          detailsEl.innerHTML = html;
        }

        function selectBank(bank) {
          const prev = document.getElementById('bank-' + selectedBank);
          if (prev) prev.classList.remove('selected');
          selectedBank = bank;
          const el = document.getElementById('bank-' + bank);
          if (el) el.classList.add('selected');
          renderDetails(bank);
        }

        window.addEventListener('message', event => {
          const message = event.data;
          if (message.command === 'status') {
            const state = message.state || 'ready';
            const text = message.message || (state === 'ready' ? 'ROM map ready.' : 'Waiting for z3lsp…');
            setStatus(state, text);
          }
          if (message.command === 'data') {
            allBlocks = Array.isArray(message.blocks) ? message.blocks : [];
            rebuildIndex(allBlocks);
            updateGrid(allBlocks);
            updateSummary();
            setStatus('ready');
            for (let i = 0; i < 64; i++) {
              const el = document.getElementById('bank-' + i);
              if (!el) continue;
              const used = Number(el.dataset.used || 0);
              const percent = Math.min(100, (used / 32768) * 100);
              el.onmouseenter = () => {
                tooltip.style.display = 'block';
                tooltip.innerHTML = '<strong>Bank ' + i.toString(16).toUpperCase().padStart(2, '0') + '</strong><br/>' + 
                                    used + ' / 32768 bytes (' + percent.toFixed(1) + '%)';
              };
              el.onclick = () => selectBank(i);
            }
            selectBank(selectedBank);
          }
        });

        vscode.postMessage({ command: 'ready' });
      </script>
    </body>
    </html>
  `;
}

function buildDashboardHtml(context) {
  try {
    const config = vscode.workspace.getConfiguration('z3dk');
    const rootDir = z3dkRoot(context) || workspaceRoot();
    const serverPath = resolveServerPath(config, rootDir);
    const projectRoot = resolveProjectRoot();
    const romInfo = resolveRomInfo(config);
    const romPath = romInfo.path || '';
    const symbolsPath = resolveSymbolsPath(config, romPath, romInfo.toml);
    const yazePath = resolveYazeExecutable(config) || '';
    const devWorkspacePath = expandHome(config.get('devWorkspacePath')) || '';
    const modelCatalogPath = resolveModelCatalogPath(config);
    const modelPortfolioPath = resolveModelPortfolioPath(config);
    const continueConfigPath = resolveContinueConfigPath(config);
    const continueConfigTsPath = resolveContinueConfigTsPath(config);
    const oracleRoot = resolveRepoPath('oracle-of-secrets', 'oracleRoot', config);
    const yazeRoot = resolveRepoPath('yaze', 'yazeRoot', config);
    const mesenRoot = resolveRepoPath('mesen2-oos', 'mesenRoot', config);
    const mesenPath = resolveMesenExecutable(config);
    const usdasmRoot = resolveUsdasmRoot(config);
    const z3dkRootPath = z3dkRoot(context);
    const afsScratchpad = z3dkRootPath
      ? path.join(z3dkRootPath, '.context', 'scratchpad', 'state.md')
      : '';

    const mainAsmPath = resolveMainAsmPath(romInfo.toml);
    const editorInfo = getEditorInfo();
    const statuses = [
      { label: 'editor', value: editorInfo.label, exists: true },
      { label: 'z3lsp', ...pathStatus(serverPath) },
      { label: 'project', ...pathStatus(projectRoot) },
      { label: 'yaze', ...pathStatus(yazePath) },
      { label: 'rom', ...pathStatus(romPath) },
      { label: 'rom source', value: romInfo.source || 'unset', exists: !!romPath },
      { label: 'main asm', ...pathStatus(mainAsmPath) },
      { label: 'symbols', ...pathStatus(symbolsPath) },
      { label: 'mesen2 app', ...pathStatus(mesenPath) },
      { label: 'usdasm', ...pathStatus(usdasmRoot) }
    ];

  const statusRows = statuses
    .map(item => {
      const statusClass = item.exists ? 'ok' : 'warn';
      return `
        <div class="status-row">
          <div class="status-label">${escapeHtml(item.label)}</div>
          <div class="status-value">${escapeHtml(item.value)}</div>
          <div class="status-pill ${statusClass}">${item.exists ? 'ready' : 'missing'}</div>
        </div>
      `;
    })
    .join('');

  const infoBadges = [
    'asar-first',
    `editor ${editorInfo.name}`,
    devWorkspacePath ? 'workspace linked' : 'workspace unset',
    usdasmRoot ? 'usdasm linked' : 'usdasm unset',
    romInfo.source ? `rom ${romInfo.source}` : 'rom unset'
  ];

  const legendItems = [
    { kind: 'server', label: 'server' },
    { kind: 'open', label: 'open' },
    { kind: 'select', label: 'select' },
    { kind: 'launch', label: 'launch' },
    { kind: 'search', label: 'search' },
    { kind: 'panel', label: 'panel' },
    { kind: 'workspace', label: 'workspace' }
  ];

  const actionGroups = [
    {
      title: 'Backend (server)',
      hint: 'Runs z3dk/z3lsp commands',
      layout: 'grid',
      open: true,
      actions: [
        { label: 'Build Z3DK', command: 'z3dk.build', kind: 'server', primary: true },
        { label: 'Run Tests', command: 'z3dk.runTests', kind: 'server' },
        { label: 'Export Symbols', command: 'z3dk.exportSymbols', kind: 'server' },
        { label: 'Export Hack Disasm', command: 'z3dk.exportDisassembly', kind: 'server', primary: true },
        { label: 'Generate Label Index', command: 'z3dk.generateLabelIndex', kind: 'server' },
        { label: 'Restart LSP', command: 'z3dk.restartServer', kind: 'server' }
      ]
    },
    {
      title: 'ROM & Project',
      hint: 'Configure project and ROM paths',
      layout: 'grid',
      actions: [
        { label: 'Select Project Root', command: 'z3dk.selectProject', kind: 'select', primary: true },
        { label: 'Select ROM', command: 'z3dk.selectRom', kind: 'select', primary: true },
        { label: 'Open Main ASM', command: 'z3dk.openMainAsm', kind: 'open' },
        { label: 'Open ROM Folder', command: 'z3dk.openRomFolder', kind: 'open' },
        { label: 'Open ROM Map', command: 'z3dk.openRomMap', kind: 'panel', primary: true }
      ]
    },
    {
      title: 'Emulators & Tools',
      hint: 'Launch external apps or logs',
      layout: 'grid',
      actions: [
        { label: 'Launch Mesen2', command: 'z3dk.launchMesen', kind: 'launch', primary: true },
        { label: 'Launch yaze', command: 'z3dk.launchYaze', kind: 'launch' },
        { label: 'Open yaze Log', command: 'z3dk.openYazeLog', kind: 'open' }
      ]
    },
    {
      title: 'Disassembly & Labels',
      hint: 'Search labels and open disassembly output',
      actions: [
        { label: 'Open Label Search Panel', command: 'z3dk.openLabelSearch', kind: 'panel', primary: true },
        { label: 'Search Labels', command: 'z3dk.findLabel', kind: 'search' },
        { label: 'Search USDASM Labels', command: 'z3dk.findUsdasmLabel', kind: 'search' },
        { label: 'Open USDASM Root', command: 'z3dk.openUsdasmRoot', kind: 'open' },
        { label: 'Open Latest Disasm', command: 'z3dk.openLatestDisassemblyFile', kind: 'open' },
        { label: 'Open Disasm Output', command: 'z3dk.openDisassemblyOutput', kind: 'open' },
        { label: 'Open Disasm File', command: 'z3dk.openDisassemblyFile', kind: 'open' }
      ]
    },
    {
      title: 'Workspace & Resources',
      hint: 'Docs, repos, and configs',
      actions: [
        { label: 'Open Workspace', command: 'z3dk.openWorkspace', kind: 'open' },
        { label: 'Open Dashboard', command: 'z3dk.openDashboard', kind: 'panel' },
        { label: 'Open README', command: 'z3dk.openReadme', kind: 'open' },
        { label: 'Open Oracle Repo', command: 'z3dk.openOracleRepo', kind: 'open' },
        { label: 'Open yaze Repo', command: 'z3dk.openYazeRepo', kind: 'open' },
        { label: 'Open Mesen2 Repo', command: 'z3dk.openMesenRepo', kind: 'open' },
        { label: 'Open Model Catalog', command: 'z3dk.openModelCatalog', kind: 'open' },
        { label: 'Open Model Portfolio', command: 'z3dk.openModelPortfolio', kind: 'open' },
        { label: 'Open Continue Config (TS)', command: 'z3dk.openContinueConfigTs', kind: 'open' },
        { label: 'Open Continue Config (YAML)', command: 'z3dk.openContinueConfig', kind: 'open' },
        { label: 'Open AFS Scratchpad', command: 'z3dk.openAfsScratchpad', kind: 'open' },
        { label: 'Add AFS Contexts', command: 'z3dk.addAfsContexts', kind: 'workspace' },
        { label: 'Refresh Dashboard', command: 'z3dk.refreshDashboard', kind: 'panel' }
      ]
    }
  ];

  const legendHtml = legendItems
    .map(item => `<span class="tag tag-${item.kind}">${escapeHtml(item.label)}</span>`)
    .join('');

  const renderAction = action => {
    const tag = action.tag || action.kind || '';
    const tagHtml = tag ? `<span class="tag tag-${action.kind}">${escapeHtml(tag)}</span>` : '';
    const kindClass = action.kind ? `kind-${action.kind}` : '';
    const primaryClass = action.primary ? 'primary' : '';
    const className = ['action', kindClass, primaryClass].filter(Boolean).join(' ');
    return `
      <button class="${className}" data-command="${action.command}" data-kind="${action.kind}">
        <span class="action-title">${escapeHtml(action.label)}</span>
        ${tagHtml}
      </button>
    `;
  };

  const actionSections = actionGroups
    .map(group => {
      const layoutClass = group.layout === 'grid' ? 'action-grid' : 'action-list';
      const hintHtml = group.hint ? `<div class="group-hint">${escapeHtml(group.hint)}</div>` : '';
      const actionsHtml = group.actions.map(renderAction).join('');
      return `
        <details class="group"${group.open ? ' open' : ''}>
          <summary>${escapeHtml(group.title)}</summary>
          <div class="group-body">
            ${hintHtml}
            <div class="${layoutClass}">
              ${actionsHtml}
            </div>
          </div>
        </details>
      `;
    })
    .join('');

    return `
    <!DOCTYPE html>
    <html lang="en">
    <head>
      <meta charset="UTF-8" />
      <meta name="viewport" content="width=device-width, initial-scale=1.0" />
      <style>
        :root {
          --bg: var(--vscode-editor-background, #1e1e1e);
          --panel: var(--vscode-editorWidget-background, var(--vscode-sideBar-background, #252526));
          --ink: var(--vscode-editor-foreground, #d4d4d4);
          --subtle: var(--vscode-descriptionForeground, rgba(200, 200, 200, 0.7));
          --border: var(--vscode-editorWidget-border, rgba(255, 255, 255, 0.08));
          --ok: #3fb99a;
          --warn: #d08a4a;
          --accent: #4da3ff;
        }

        body {
          margin: 0;
          padding: 8px;
          color: var(--ink);
          font-family: var(--vscode-font-family, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif);
          font-size: 11px;
          background: var(--bg);
          min-height: 100%;
        }

        .header {
          display: flex;
          flex-direction: column;
          gap: 2px;
          margin-bottom: 6px;
        }

        .title {
          font-size: 12px;
          font-weight: 600;
          letter-spacing: 0.2px;
        }

        .meta {
          color: var(--subtle);
          font-size: 10px;
        }

        .stack {
          display: grid;
          gap: 6px;
        }

        .legend {
          display: flex;
          flex-wrap: wrap;
          gap: 4px;
          margin-bottom: 6px;
          align-items: center;
        }

        .legend-title {
          font-size: 9px;
          text-transform: uppercase;
          letter-spacing: 0.3px;
          color: var(--subtle);
        }

        details.group {
          background: var(--panel);
          border: 1px solid var(--border);
          border-radius: 6px;
          overflow: hidden;
        }

        details.group summary {
          list-style: none;
          cursor: pointer;
          padding: 6px 8px;
          margin: 0;
          display: flex;
          align-items: center;
          justify-content: space-between;
          font-size: 10px;
          text-transform: uppercase;
          letter-spacing: 0.4px;
          color: var(--subtle);
          background: var(--vscode-sideBarSectionHeader-background, rgba(255, 255, 255, 0.04));
        }

        details.group summary::-webkit-details-marker {
          display: none;
        }

        details.group summary::after {
          content: '▸';
          font-size: 9px;
          transition: transform 0.12s ease;
        }

        details.group[open] summary {
          border-bottom: 1px solid var(--border);
        }

        details.group[open] summary::after {
          transform: rotate(90deg);
        }

        .group-body {
          padding: 6px 8px;
          display: grid;
          gap: 6px;
        }

        .group-hint {
          font-size: 9px;
          color: var(--subtle);
        }

        .action-grid {
          display: grid;
          grid-template-columns: repeat(2, minmax(0, 1fr));
          gap: 4px;
        }

        .action-list {
          display: grid;
          gap: 2px;
        }

        @media (max-width: 320px) {
          .action-grid {
            grid-template-columns: 1fr;
          }
        }

        button.action {
          appearance: none;
          background: transparent;
          border: 1px solid transparent;
          border-radius: 4px;
          padding: 4px 6px;
          font-size: 10px;
          line-height: 1.4;
          color: var(--ink);
          text-align: left;
          display: flex;
          align-items: center;
          justify-content: space-between;
          gap: 6px;
          cursor: pointer;
        }

        button.action:hover {
          background: var(--vscode-list-hoverBackground, rgba(255, 255, 255, 0.06));
        }

        button.action.primary {
          font-weight: 600;
          color: var(--vscode-textLink-foreground, #4da3ff);
          border-color: rgba(77, 163, 255, 0.2);
        }

        button.action.kind-server {
          border-left: 2px solid rgba(77, 163, 255, 0.5);
        }

        button.action.kind-open {
          border-left: 2px solid rgba(63, 185, 154, 0.5);
        }

        button.action.kind-select {
          border-left: 2px solid rgba(209, 182, 92, 0.5);
        }

        button.action.kind-launch {
          border-left: 2px solid rgba(208, 138, 74, 0.5);
        }

        button.action.kind-search {
          border-left: 2px solid rgba(107, 183, 199, 0.5);
        }

        button.action.kind-panel {
          border-left: 2px solid rgba(138, 163, 255, 0.5);
        }

        button.action.kind-workspace {
          border-left: 2px solid rgba(154, 163, 173, 0.5);
        }

        .action-title {
          flex: 1;
        }

        .tag {
          padding: 1px 5px;
          border-radius: 999px;
          font-size: 8px;
          text-transform: uppercase;
          letter-spacing: 0.3px;
          border: 1px solid transparent;
          color: var(--subtle);
          white-space: nowrap;
        }

        .tag-server {
          color: var(--accent);
          border-color: rgba(77, 163, 255, 0.4);
          background: rgba(77, 163, 255, 0.12);
        }

        .tag-open {
          color: var(--ok);
          border-color: rgba(63, 185, 154, 0.4);
          background: rgba(63, 185, 154, 0.12);
        }

        .tag-select {
          color: #d1b65c;
          border-color: rgba(209, 182, 92, 0.4);
          background: rgba(209, 182, 92, 0.12);
        }

        .tag-launch {
          color: var(--warn);
          border-color: rgba(208, 138, 74, 0.4);
          background: rgba(208, 138, 74, 0.12);
        }

        .tag-search {
          color: #6bb7c7;
          border-color: rgba(107, 183, 199, 0.4);
          background: rgba(107, 183, 199, 0.12);
        }

        .tag-panel {
          color: #8aa3ff;
          border-color: rgba(138, 163, 255, 0.4);
          background: rgba(138, 163, 255, 0.12);
        }

        .tag-workspace {
          color: #9aa3ad;
          border-color: rgba(154, 163, 173, 0.4);
          background: rgba(154, 163, 173, 0.12);
        }

        .status {
          display: flex;
          flex-direction: column;
          gap: 6px;
        }

        .status-row {
          display: grid;
          grid-template-columns: 70px 1fr auto;
          gap: 4px;
          align-items: center;
        }

        .status-label {
          font-size: 9px;
          text-transform: uppercase;
          letter-spacing: 0.4px;
          color: var(--subtle);
        }

        .status-value {
          font-family: var(--vscode-editor-font-family, "Azeret Mono", "JetBrains Mono", monospace);
          font-size: 9px;
          color: var(--ink);
          overflow-wrap: anywhere;
        }

        .status-pill {
          padding: 1px 5px;
          border-radius: 999px;
          font-size: 8px;
          text-transform: uppercase;
          letter-spacing: 0.3px;
          border: 1px solid transparent;
        }

        .status-pill.ok {
          background: rgba(63, 185, 154, 0.18);
          color: var(--ok);
          border-color: rgba(63, 185, 154, 0.3);
        }

        .status-pill.warn {
          background: rgba(208, 138, 74, 0.18);
          color: var(--warn);
          border-color: rgba(208, 138, 74, 0.3);
        }
      </style>
    </head>
    <body>
      <div class="header">
        <div class="title">Z3DK</div>
        <div class="meta">${escapeHtml(infoBadges.join(' • '))}</div>
      </div>

      <div class="stack">
        <div class="legend"><span class="legend-title">Legend</span>${legendHtml}</div>
        ${actionSections}

        <details class="group" open>
          <summary>Status</summary>
          <div class="group-body">
            <div class="status">
              ${statusRows}
            </div>
          </div>
        </details>
      </div>

      <script>
        const vscode = acquireVsCodeApi();
        document.querySelectorAll('button[data-command]').forEach(button => {
          button.addEventListener('click', () => {
            vscode.postMessage({ command: button.dataset.command });
          });
        });
      </script>
    </body>
    </html>
  `;
  } catch (error) {
    const output = ensureOutputChannel();
    output.appendLine(`[Z3DK] Dashboard render error: ${error && error.stack ? error.stack : error}`);
    const message = escapeHtml(error && error.message ? error.message : String(error));
    return `
      <!DOCTYPE html>
      <html lang="en">
      <head>
        <meta charset="UTF-8" />
        <meta name="viewport" content="width=device-width, initial-scale=1.0" />
        <style>
          body {
            font-family: "Azeret Mono", "JetBrains Mono", monospace;
            padding: 16px;
          }
          .error {
            padding: 12px;
            border-radius: 12px;
            background: rgba(200, 111, 63, 0.15);
            border: 1px solid rgba(200, 111, 63, 0.4);
          }
        </style>
      </head>
      <body>
        <div class="error">
          <strong>Z3DK Dashboard failed to render.</strong>
          <pre>${message}</pre>
        </div>
      </body>
      </html>
    `;
  }
}

function buildLabelSearchHtml(context) {
  const config = vscode.workspace.getConfiguration('z3dk');
  const indexPath = resolveLabelIndex(config, context);
  const exists = indexPath && fs.existsSync(indexPath);
  const badge = exists ? 'index ready' : 'index missing';
  const indexLabel = indexPath ? path.basename(indexPath) : 'label_index_all.csv';
  return `
    <!DOCTYPE html>
    <html lang="en">
    <head>
      <meta charset="UTF-8" />
      <meta name="viewport" content="width=device-width, initial-scale=1.0" />
      <style>
        :root {
          --bg: var(--vscode-editor-background, #1e1e1e);
          --panel: var(--vscode-editorWidget-background, var(--vscode-sideBar-background, #252526));
          --ink: var(--vscode-editor-foreground, #d4d4d4);
          --subtle: var(--vscode-descriptionForeground, rgba(200, 200, 200, 0.7));
          --border: var(--vscode-editorWidget-border, rgba(255, 255, 255, 0.08));
          --accent: #4da3ff;
        }

        body {
          margin: 0;
          padding: 8px;
          color: var(--ink);
          font-family: var(--vscode-font-family, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif);
          font-size: 11px;
          background: var(--bg);
        }

        .header {
          display: flex;
          flex-direction: column;
          gap: 2px;
          margin-bottom: 6px;
        }

        .title {
          font-size: 12px;
          font-weight: 600;
        }

        .meta {
          color: var(--subtle);
          font-size: 10px;
        }

        .toolbar {
          display: grid;
          gap: 6px;
          margin-bottom: 8px;
        }

        .toolbar-row {
          display: flex;
          gap: 6px;
          align-items: center;
        }

        .input {
          width: 100%;
          padding: 6px 8px;
          border-radius: 6px;
          border: 1px solid var(--border);
          background: var(--panel);
          color: var(--ink);
          font-size: 11px;
        }

        .scope {
          display: flex;
          gap: 4px;
          flex-wrap: wrap;
        }

        .scope button {
          appearance: none;
          border: 1px solid var(--border);
          border-radius: 999px;
          padding: 3px 8px;
          font-size: 10px;
          background: var(--panel);
          color: var(--ink);
          cursor: pointer;
        }

        .scope button.active {
          border-color: rgba(77, 163, 255, 0.35);
          color: var(--vscode-textLink-foreground, #4da3ff);
          font-weight: 600;
        }

        .toolbar-actions {
          display: flex;
          gap: 4px;
        }

        .action-link {
          appearance: none;
          border: 1px solid transparent;
          border-radius: 999px;
          padding: 2px 6px;
          font-size: 9px;
          text-transform: uppercase;
          letter-spacing: 0.3px;
          background: transparent;
          color: var(--vscode-textLink-foreground, var(--accent));
          cursor: pointer;
          display: inline-flex;
          align-items: center;
          gap: 6px;
        }

        .action-link .icon {
          border: 1px solid var(--border);
          border-radius: 4px;
          padding: 1px 4px;
          font-size: 8px;
          color: var(--ink);
          background: rgba(255, 255, 255, 0.04);
        }

        .action-link:hover {
          border-color: rgba(77, 163, 255, 0.35);
          background: rgba(77, 163, 255, 0.08);
        }

        .results {
          display: grid;
          gap: 4px;
        }

        .result {
          border: 1px solid var(--border);
          border-radius: 6px;
          padding: 6px;
          background: var(--panel);
        }

        .result:hover {
          border-color: rgba(77, 163, 255, 0.35);
          background: rgba(77, 163, 255, 0.06);
        }

        .result button {
          appearance: none;
          background: transparent;
          border: none;
          color: inherit;
          padding: 0;
          text-align: left;
          cursor: pointer;
          width: 100%;
        }

        .result-label {
          font-size: 11px;
          font-weight: 600;
        }

        .result-meta {
          margin-top: 2px;
          color: var(--subtle);
          font-size: 10px;
          display: flex;
          gap: 6px;
          flex-wrap: wrap;
        }

        .badge {
          border: 1px solid var(--border);
          padding: 1px 6px;
          border-radius: 999px;
          font-size: 9px;
          color: var(--subtle);
        }
      </style>
    </head>
    <body>
      <div class="header">
        <div class="title">Label Search</div>
        <div class="meta">
          <span id="index-status">${escapeHtml(badge)}</span> •
          <span id="index-name">${escapeHtml(indexLabel)}</span> •
          <span id="index-count">0 labels</span>
        </div>
      </div>

      <div class="toolbar">
        <div class="toolbar-row">
          <input id="query" class="input" placeholder="Search labels" />
          <div class="toolbar-actions">
            <button id="regen" class="action-link" title="Regenerate label index">
              <span class="icon">IDX</span>
              <span>Regen</span>
            </button>
            <button id="refresh" class="action-link" title="Refresh panel">
              <span class="icon">REF</span>
              <span>Refresh</span>
            </button>
          </div>
        </div>
        <div class="scope">
          <button data-scope="all" class="active">All</button>
          <button data-scope="hack">Hack</button>
          <button data-scope="usdasm">USDASM</button>
        </div>
      </div>

      <div id="results" class="results">
        <div class="meta">Type to search.</div>
      </div>

      <script>
        const vscode = acquireVsCodeApi();
        const queryInput = document.getElementById('query');
        const resultsEl = document.getElementById('results');
        const indexStatus = document.getElementById('index-status');
        const indexName = document.getElementById('index-name');
        const indexCount = document.getElementById('index-count');
        const scopeButtons = Array.from(document.querySelectorAll('.scope button[data-scope]'));
        const regenButton = document.getElementById('regen');
        const refreshButton = document.getElementById('refresh');
        const summaryLine = document.createElement('div');
        summaryLine.className = 'meta';
        summaryLine.style.marginTop = '4px';
        summaryLine.style.display = 'none';
        if (resultsEl && resultsEl.parentNode) {
          resultsEl.parentNode.insertBefore(summaryLine, resultsEl);
        }
        let currentScope = 'all';
        let debounce;
        let currentResults = [];

        function requestSearch() {
          vscode.postMessage({
            command: 'search',
            query: queryInput.value || '',
            scope: currentScope
          });
        }

        function scheduleSearch() {
          clearTimeout(debounce);
          debounce = setTimeout(requestSearch, 180);
        }

        queryInput.addEventListener('input', scheduleSearch);
        queryInput.addEventListener('keyup', scheduleSearch);
        queryInput.addEventListener('change', requestSearch);

        scopeButtons.forEach(button => {
          button.addEventListener('click', () => {
            scopeButtons.forEach(btn => btn.classList.remove('active'));
            button.classList.add('active');
            currentScope = button.dataset.scope || 'all';
            requestSearch();
          });
        });

        regenButton.addEventListener('click', () => {
          vscode.postMessage({ command: 'regen' });
        });

        refreshButton.addEventListener('click', () => {
          vscode.postMessage({ command: 'refresh' });
        });

        function renderResults(results, query, mode) {
          currentResults = results;
          resultsEl.innerHTML = '';
          const trimmed = (query || '').trim();
          summaryLine.style.display = 'none';
          if (mode === 'short') {
            resultsEl.innerHTML = '<div class="meta">Type at least 2 characters to search this index.</div>';
            return;
          }
          if (!trimmed && (!results || results.length === 0)) {
            if (mode === 'defaultLarge') {
              resultsEl.innerHTML = '<div class="meta">Large index ready. Type to search.</div>';
            } else {
              resultsEl.innerHTML = '<div class="meta">Type to search.</div>';
            }
            return;
          }
          if (!results.length) {
            resultsEl.innerHTML = '<div class="meta">No matches found.</div>';
            return;
          }
          if (trimmed) {
            summaryLine.textContent = 'Matches: ' + results.length + ' for \"' + trimmed + '\"';
            summaryLine.style.display = 'block';
          }
          if (!trimmed && mode === 'default') {
            const hint = document.createElement('div');
            hint.className = 'meta';
            hint.textContent = 'Showing top labels. Type to filter.';
            resultsEl.appendChild(hint);
          }
          if (!trimmed && mode === 'defaultLarge') {
            const hint = document.createElement('div');
            hint.className = 'meta';
            hint.textContent = 'Large index preview. Type to filter.';
            resultsEl.appendChild(hint);
          }
          results.forEach((entry, index) => {
            const item = document.createElement('div');
            item.className = 'result';
            const lineSuffix = entry.line ? ':' + entry.line : '';
            item.innerHTML =
              '<button data-index=\"' + index + '\">' +
              '<div class=\"result-label\">' + (entry.label || '') + '</div>' +
              '<div class=\"result-meta\">' +
              '<span>' + (entry.address || '') + '</span>' +
              '<span>' + (entry.file || '') + lineSuffix + '</span>' +
              '<span class=\"badge\">' + (entry.source_repo || 'unknown') + '</span>' +
              '</div>' +
              '</button>';
            resultsEl.appendChild(item);
          });
          resultsEl.querySelectorAll('button[data-index]').forEach(button => {
            button.addEventListener('click', () => {
              const index = Number(button.dataset.index);
              const entry = currentResults[index];
              if (entry) {
                vscode.postMessage({ command: 'open', entry });
              }
            });
          });
        }

        window.addEventListener('message', event => {
          const message = event.data || {};
          if (message.command === 'status') {
            indexStatus.textContent = message.exists ? 'index ready' : 'index missing';
            indexName.textContent = message.indexPath ? message.indexPath.split('/').pop() : 'label_index_all.csv';
            indexCount.textContent = message.count ? (message.count + ' labels') : '0 labels';
          }
          if (message.command === 'results') {
            renderResults(message.results || [], message.query || '', message.mode || 'search');
          }
        });

        vscode.postMessage({ command: 'ready' });
      </script>
    </body>
    </html>
  `;
}

async function startClient(context) {
  if (client) {
    return resolveClientValue(client);
  }

  const config = vscode.workspace.getConfiguration('z3dk');
  if (!config.get('autoStart')) {
    return undefined;
  }

  const Client = loadLanguageClient();
  if (!Client) {
    updateStatusBar(context);
    return undefined;
  }

  const rootDir = z3dkRoot(context) || workspaceRoot();
  const explicitServer = expandHome(config.get('serverPath'));
  if (explicitServer && !fs.existsSync(explicitServer)) {
    vscode.window.showWarningMessage(`Z3DK: z3lsp not found at ${explicitServer}. Falling back to auto-detect.`);
  }
  const serverPath = resolveServerPath(config, rootDir);
  const serverArgs = config.get('serverArgs') || [];
  const languageIds = getSupportedLanguageIds(config);

  const serverOptions = {
    command: serverPath,
    args: serverArgs,
    options: {
      cwd: rootDir || undefined
    }
  };

  const clientOptions = {
    documentSelector: languageIds.map(language => ({ language, scheme: 'file' })),
    outputChannel: ensureOutputChannel()
  };

  const candidate = new Client('z3dk', 'Z3DK Language Server', serverOptions, clientOptions);
  if (typeof candidate.start !== 'function') {
    ensureOutputChannel().appendLine(`[Z3DK] LanguageClient missing start: ${describeClient(candidate)}`);
    vscode.window.showErrorMessage('Z3DK: Failed to initialize language client. Check the Output panel.');
    return undefined;
  }
  client = candidate;
  clientStartPromise = client.start()
    .then(() => {
      setLspActive(true);
      updateStatusBar(context);
    })
    .catch(err => {
      setLspActive(false);
      ensureOutputChannel().appendLine(`[Z3DK] LSP failed to start: ${err}`);
      updateStatusBar(context);
    });
  updateStatusBar(context);
  return client;
}

async function stopClient() {
  if (!client) {
    return;
  }
  const current = client;
  client = undefined;
  clientStartPromise = undefined;
  await current.stop();
  setLspActive(false);
  if (extensionContext) {
    updateStatusBar(extensionContext);
  }
}

async function restartClient(context) {
  await stopClient();
  await startClient(context);
}

function buildExportSymbolsCommand(config, romInfo) {
  const resolvedRomInfo = romInfo || resolveRomInfo(config);
  const romPath = resolvedRomInfo.path;
  if (!romPath) {
    return null;
  }
  const yazePath = resolveYazeExecutable(config);
  if (!yazePath) {
    vscode.window.showWarningMessage('Set z3dk.yazePath or install yaze-nightly to export symbols.');
    return null;
  }
  const format = resolveSymbolFormat(config, resolvedRomInfo.toml);
  const outputPath = resolveSymbolsPath(config, romPath, resolvedRomInfo.toml);
  const symCandidate = `${romPath}.sym`;
  if (fs.existsSync(symCandidate)) {
    const command = `${yazePath} --export_symbols_fast --export_symbols "${outputPath}" --symbol_format ${format} --load_symbols "${symCandidate}"`;
    return wrapYazeCommand(command, config);
  }
  const command = `${yazePath} --headless --rom_file "${romPath}" --export_symbols "${outputPath}" --symbol_format ${format}`;
  return wrapYazeCommand(command, config);
}

function activate(context) {
  extensionContext = context;
  setLspActive(false);
  ensureOutputChannel();
  ensureOutputChannel().appendLine('Z3DK extension activated.');
  updateLanguageContext(vscode.window.activeTextEditor);
  context.subscriptions.push(vscode.window.onDidChangeActiveTextEditor(editor => {
    updateLanguageContext(editor);
  }));
  commandProvider = new Z3dkCommandProvider(context);
  context.subscriptions.push(vscode.window.registerTreeDataProvider('z3dk.commands', commandProvider));
  dashboardProvider = new Z3dkDashboardProvider(context);
  context.subscriptions.push(
    vscode.window.registerWebviewViewProvider('z3dk.dashboard', dashboardProvider)
  );
  labelSearchProvider = new Z3dkLabelSearchProvider(context);
  context.subscriptions.push(
    vscode.window.registerWebviewViewProvider('z3dk.labels', labelSearchProvider)
  );
  romMapProvider = new Z3dkRomMapProvider(context);
  context.subscriptions.push(
    vscode.window.registerWebviewViewProvider('z3dk.romMap', romMapProvider)
  );

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.openRomMap', () => {
    vscode.commands.executeCommand('z3dk.romMap.focus');
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.initProject', async () => {
    const name = await vscode.window.showInputBox({
      prompt: 'Enter a name for your new Zelda project',
      placeHolder: 'MyALTTPHack'
    });
    if (!name) return;

    const root = workspaceRoot();
    if (!root) {
      vscode.window.showErrorMessage('Open a folder first to initialize a project.');
      return;
    }

    const config = vscode.workspace.getConfiguration('z3dk');
    const z3asm = expandHome(config.get('serverPath')) || 'z3asm'; // Use server path as proxy for tool path
    const cmd = `${z3asm} init "${name}"`;

    exec(cmd, { cwd: root }, async (error, stdout, stderr) => {
      if (error) {
        vscode.window.showErrorMessage(`Initialization failed: ${stderr || error.message}`);
        return;
      }
      vscode.window.showInformationMessage(`Project "${name}" initialized successfully.`);
      
      const mainAsm = path.join(root, name, 'src', 'main.asm');
      if (fs.existsSync(mainAsm)) {
        const doc = await vscode.workspace.openTextDocument(mainAsm);
        await vscode.window.showTextDocument(doc);
      }
      
      if (dashboardProvider) dashboardProvider.refresh();
    });
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.runTests', () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const command = config.get('testCommand');
    const rootDir = z3dkRoot(context) || workspaceRoot();
    runInTerminal(command, rootDir);
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.build', () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const command = config.get('buildCommand');
    const rootDir = z3dkRoot(context) || workspaceRoot();
    runInTerminal(command, rootDir);
    scheduleLabelIndexRefresh(context, 'build');
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.exportSymbols', async () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const romInfo = await ensureRomInfo(config, { placeHolder: 'Select ROM to export symbols' });
    if (!romInfo.path) {
      vscode.window.showWarningMessage('Select a ROM before exporting symbols.');
      return;
    }
    const command = buildExportSymbolsCommand(config, romInfo);
    const rootDir = z3dkRoot(context) || workspaceRoot();
    if (command) {
      runInTerminal(command, rootDir);
      scheduleLabelIndexRefresh(context, 'symbols');
    }
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.restartServer', async () => {
    await restartClient(context);
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.openDashboard', async () => {
    await vscode.commands.executeCommand('workbench.view.extension.z3dk');
    if (dashboardProvider) {
      dashboardProvider.refresh();
    }
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.lspStatusOn', async () => {
    vscode.window.showInformationMessage('Z3DK LSP: active');
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.lspStatusOff', async () => {
    vscode.window.showInformationMessage('Z3DK LSP: inactive');
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.launchMesen', async () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const mesenExecutable = resolveMesenExecutable(config);
    if (!mesenExecutable) {
      vscode.window.showWarningMessage('Set z3dk.mesenPath to launch Mesen2-OOS.');
      return;
    }
    const argsTemplate = config.get('mesenArgs') || [];
    const needsRom = argsTemplate.some(arg => String(arg).includes('${rom}'));
    const romInfo = needsRom ? await ensureRomInfo(config, { placeHolder: 'Select ROM for Mesen2' }) : resolveRomInfo(config);
    const romPath = romInfo.path || '';
    if (needsRom && !romPath) {
      vscode.window.showWarningMessage('Select a ROM to launch Mesen2.');
      return;
    }
    const symbolsPath = resolveSymbolsPath(config, romPath, romInfo.toml);
    const args = expandArgs(argsTemplate, {
      rom: romPath,
      symbols: symbolsPath
    });
    const command = buildLaunchCommand(mesenExecutable, args);
    const rootDir = z3dkRoot(context) || workspaceRoot();
    runInTerminal(command, rootDir);
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.launchYaze', async () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const yazeExecutable = resolveYazeExecutable(config);
    if (!yazeExecutable) {
      vscode.window.showWarningMessage('Set z3dk.yazePath or install yaze-nightly to launch yaze.');
      return;
    }
    const argsTemplate = config.get('yazeLaunchArgs') || [];
    const needsRom = argsTemplate.some(arg => String(arg).includes('${rom}'));
    const romInfo = needsRom ? await ensureRomInfo(config, { placeHolder: 'Select ROM for yaze' }) : resolveRomInfo(config);
    const romPath = romInfo.path || '';
    if (needsRom && !romPath) {
      vscode.window.showWarningMessage('Select a ROM to launch yaze.');
      return;
    }
    const symbolsPath = resolveSymbolsPath(config, romPath, romInfo.toml);
    const args = expandArgs(argsTemplate, {
      rom: romPath,
      symbols: symbolsPath
    });
    const command = buildLaunchCommand(yazeExecutable, args);
    const rootDir = z3dkRoot(context) || workspaceRoot();
    runInTerminal(command, rootDir);
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.openRomFolder', async () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const romInfo = await ensureRomInfo(config, { placeHolder: 'Select ROM to reveal folder' });
    const romPath = romInfo.path;
    if (!romPath || !fs.existsSync(romPath)) {
      vscode.window.showWarningMessage('Set z3dk.romPath or z3dk.toml rom_path to open the ROM folder.');
      return;
    }
    await revealFolder(path.dirname(romPath));
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.selectRom', async () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const romInfo = await ensureRomInfo(config, {
      forcePick: true,
      updateConfig: true,
      placeHolder: 'Select ROM to use for Z3DK commands'
    });
    if (romInfo && romInfo.path) {
      vscode.window.showInformationMessage(`Z3DK ROM set to ${romInfo.path}`);
    }
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.selectProject', async () => {
    const picked = await promptForProjectRootSelection();
    if (picked) {
      vscode.window.showInformationMessage(`Z3DK project root set to ${picked}`);
      if (client) {
        await restartClient(context);
      }
    }
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.openUsdasmRoot', async () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const usdasmRoot = resolveUsdasmRoot(config);
    if (!usdasmRoot) {
      vscode.window.showWarningMessage('Set z3dk.usdasmRoot to open USDASM.');
      return;
    }
    await revealFolder(usdasmRoot);
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.findLabel', async () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const indexPath = resolveLabelIndex(config, context);
    if (!indexPath || !fs.existsSync(indexPath)) {
      vscode.window.showWarningMessage('Label index not found. Run generate_label_indexes.py first.');
      return;
    }
    const entries = loadLabelIndexFile(indexPath);
    if (!entries.length) {
      vscode.window.showWarningMessage('Label index is empty.');
      return;
    }
    const items = entries.map(entry => ({
      label: entry.label || '',
      description: [entry.address, entry.source_repo].filter(Boolean).join(' • '),
      detail: entry.file ? `${entry.file}${entry.line ? `:${entry.line}` : ''}` : '',
      entry
    }));
    const pick = await vscode.window.showQuickPick(items, {
      placeHolder: 'Search labels (hack + usdasm)',
      matchOnDescription: true,
      matchOnDetail: true
    });
    if (!pick) {
      return;
    }
    await openLabelEntry(pick.entry, config, context);
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.findUsdasmLabel', async () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const usdasmRoot = resolveUsdasmRoot(config);
    const label = await vscode.window.showInputBox({
      prompt: 'USDASM label to locate',
      placeHolder: 'e.g. Link_Main or Sprite_Execute',
      ignoreFocusOut: true
    });
    if (!label) {
      return;
    }
    const defaultGlob = '**/*.asm,**/*.inc,**/*.s,**/*.a65';
    const glob = config.get('usdasmGlob') || defaultGlob;
    let filesToInclude = glob;
    if (usdasmRoot) {
      filesToInclude = glob
        .split(',')
        .map(entry => path.join(usdasmRoot, entry.trim()))
        .join(',');
    }
    await vscode.commands.executeCommand('workbench.action.findInFiles', {
      query: label,
      filesToInclude
    });
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.exportDisassembly', async () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const romInfo = await ensureRomInfo(config, { placeHolder: 'Select ROM for hack disassembly' });
    if (!romInfo.path) {
      vscode.window.showWarningMessage('Select a ROM before exporting disassembly.');
      return;
    }
    const command = buildDisassemblyCommand(config, context, romInfo);
    if (!command) {
      vscode.window.showWarningMessage('Set z3dk.disasmCommand or build z3disasm to export the hack disassembly.');
      return;
    }
    const rootDir = z3dkRoot(context) || workspaceRoot();
    runInTerminal(command, rootDir);
    scheduleLabelIndexRefresh(context, 'disasm');
    if (config.get('autoOpenDisassemblyOutput')) {
      setTimeout(() => {
        vscode.commands.executeCommand('z3dk.openDisassemblyOutput');
      }, 500);
    }
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.openDisassemblyOutput', async () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const outputPath = expandHome(config.get('disasmOutputPath'));
    if (!outputPath) {
      vscode.window.showWarningMessage('Set z3dk.disasmOutputPath to view output.');
      return;
    }
    if (!fs.existsSync(outputPath)) {
      vscode.window.showWarningMessage(`Disassembly output not found: ${outputPath}`);
      return;
    }
    const stats = fs.statSync(outputPath);
    if (stats.isDirectory()) {
      await revealFolder(outputPath);
    } else {
      await openFilePath(outputPath);
    }
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.openDisassemblyFile', async () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const folderPath = resolveDisassemblyFolder(config);
    if (!folderPath) {
      vscode.window.showWarningMessage('Set z3dk.disasmOutputPath to open disassembly files.');
      return;
    }
    const entries = await fs.promises.readdir(folderPath, { withFileTypes: true });
    const files = entries
      .filter(entry => entry.isFile())
      .map(entry => entry.name)
      .filter(name => /\.(asm|inc|s|a65)$/i.test(name))
      .sort((a, b) => a.localeCompare(b));
    if (files.length === 0) {
      vscode.window.showWarningMessage('No disassembly files found in output folder.');
      return;
    }
    const pick = await vscode.window.showQuickPick(files, {
      placeHolder: 'Open disassembly file',
      matchOnDescription: true
    });
    if (!pick) {
      return;
    }
    await openFilePath(path.join(folderPath, pick));
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.openLatestDisassemblyFile', async () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const folderPath = resolveDisassemblyFolder(config);
    if (!folderPath) {
      vscode.window.showWarningMessage('Set z3dk.disasmOutputPath to open disassembly files.');
      return;
    }
    const entries = await fs.promises.readdir(folderPath, { withFileTypes: true });
    const files = entries
      .filter(entry => entry.isFile())
      .map(entry => entry.name);
    const bankRegex = /^bank_[0-9a-f]{2,}\.asm$/i;
    const candidates = files.filter(name => bankRegex.test(name));
    const fallback = files.filter(name => /\.(asm|inc|s|a65)$/i.test(name));
    const targetFiles = candidates.length > 0 ? candidates : fallback;
    if (targetFiles.length === 0) {
      vscode.window.showWarningMessage('No disassembly files found in output folder.');
      return;
    }
    const stats = await Promise.all(
      targetFiles.map(async name => {
        const fullPath = path.join(folderPath, name);
        const stat = await fs.promises.stat(fullPath);
        return { name, fullPath, mtimeMs: stat.mtimeMs };
      })
    );
    stats.sort((a, b) => b.mtimeMs - a.mtimeMs);
    await openFilePath(stats[0].fullPath);
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.openYazeLog', async () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const logPath = resolveYazeLogPath(config);
    if (!logPath) {
      vscode.window.showWarningMessage('Set z3dk.yazeLogPath to view logs.');
      return;
    }
    if (!fs.existsSync(logPath)) {
      vscode.window.showWarningMessage(`Yaze log not found: ${logPath}`);
      return;
    }
    await openFilePath(logPath);
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.refresh', () => {
    if (commandProvider) {
      commandProvider.refresh();
    }
    if (dashboardProvider) {
      dashboardProvider.refresh();
    }
    if (labelSearchProvider) {
      labelSearchProvider.refresh();
    }
    updateStatusBar(context);
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.refreshDashboard', () => {
    if (dashboardProvider) {
      dashboardProvider.refresh();
    }
    updateStatusBar(context);
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.generateLabelIndex', () => {
    runLabelIndexCommand(context, 'manual');
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.openLabelSearch', async () => {
    await vscode.commands.executeCommand('workbench.view.extension.z3dk');
    await vscode.commands.executeCommand('z3dk.labels.focus');
    if (labelSearchProvider) {
      labelSearchProvider.refresh();
    }
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.openWorkspace', async () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    let workspacePath = config.get('devWorkspacePath');
    if (!workspacePath) {
      const fallback = path.join('/Users/scawful/src/hobby', 'dev.code-workspace');
      const fallbackAlt = path.join('/Users/scawful/src/hobby', 'Dev.code-workspace');
      if (fs.existsSync(fallback)) {
        workspacePath = fallback;
      } else if (fs.existsSync(fallbackAlt)) {
        workspacePath = fallbackAlt;
      }
    }
    if (!workspacePath) {
      vscode.window.showWarningMessage('Set z3dk.devWorkspacePath to your .code-workspace file.');
      return;
    }
    const uri = vscode.Uri.file(workspacePath);
    await vscode.commands.executeCommand('vscode.openFolder', uri, false);
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.openMainAsm', async () => {
    const rootDir = resolveProjectRoot();
    const tomlConfig = loadZ3dkToml(rootDir);
    const mainAsm = resolveMainAsmPath(tomlConfig);
    if (!mainAsm) {
      vscode.window.showWarningMessage('Main ASM not found. Set main in z3dk.toml.');
      return;
    }
    await openFilePath(mainAsm);
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.openModelCatalog', async () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const modelCatalogPath = resolveModelCatalogPath(config);
    await openFilePath(modelCatalogPath);
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.openModelPortfolio', async () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const modelPortfolioPath = resolveModelPortfolioPath(config);
    await openFilePath(modelPortfolioPath);
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.openContinueConfig', async () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const configPath = resolveContinueConfigPath(config);
    await openFilePath(configPath);
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.openContinueConfigTs', async () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const configPath = resolveContinueConfigTsPath(config);
    await openFilePath(configPath);
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.openAfsScratchpad', async () => {
    const root = z3dkRoot(context);
    if (!root) {
      vscode.window.showWarningMessage('Unable to locate z3dk root for scratchpad.');
      return;
    }
    const scratchpadPath = path.join(root, '.context', 'scratchpad', 'state.md');
    await openFilePath(scratchpadPath);
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.addAfsContexts', async () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const roots = [
      z3dkRoot(context),
      resolveRepoPath('oracle-of-secrets', 'oracleRoot', config)
    ].filter(Boolean);
    if (roots.length === 0) {
      vscode.window.showWarningMessage('No AFS roots found to add.');
      return;
    }
    for (const root of roots) {
      const contextPath = path.join(root, '.context');
      if (fs.existsSync(contextPath)) {
        ensureWorkspaceFolder(contextPath);
      }
    }
    vscode.window.showInformationMessage('AFS context folders added to workspace.');
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.openOracleRepo', async () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const repoPath = resolveRepoPath('oracle-of-secrets', 'oracleRoot', config);
    await revealFolder(repoPath);
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.openYazeRepo', async () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const repoPath = resolveRepoPath('yaze', 'yazeRoot', config);
    await revealFolder(repoPath);
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.openMesenRepo', async () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const repoPath = resolveRepoPath('mesen2-oos', 'mesenRoot', config);
    await revealFolder(repoPath);
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.openReadme', async () => {
    const root = z3dkRoot(context);
    if (!root) {
      vscode.window.showWarningMessage('Unable to locate z3dk root.');
      return;
    }
    const readmePath = path.join(root, 'README.md');
    if (!fs.existsSync(readmePath)) {
      vscode.window.showWarningMessage(`README not found: ${readmePath}`);
      return;
    }
    const doc = await vscode.workspace.openTextDocument(readmePath);
    await vscode.window.showTextDocument(doc, { preview: false });
  }));

  context.subscriptions.push(vscode.workspace.onDidChangeConfiguration(event => {
    if (event.affectsConfiguration('z3dk')) {
      labelIndexCache.clear();
      restartClient(context);
      if (dashboardProvider) {
        dashboardProvider.refresh();
      }
      if (labelSearchProvider) {
        labelSearchProvider.refresh();
      }
      updateLanguageContext(vscode.window.activeTextEditor);
      updateStatusBar(context);
    }
  }));

  startClient(context);
  updateStatusBar(context);
}

async function deactivate() {
  await stopClient();
}

module.exports = {
  activate,
  deactivate
};
