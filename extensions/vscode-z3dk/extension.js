const fs = require('fs');
const os = require('os');
const path = require('path');
const vscode = require('vscode');
const { LanguageClient } = require('vscode-languageclient/node');

let client;
let outputChannel;
let terminal;
let commandProvider;
let dashboardProvider;
let statusItems = [];

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
  if (!inputPath) {
    return inputPath;
  }
  if (inputPath.startsWith('~')) {
    return path.join(os.homedir(), inputPath.slice(1));
  }
  return inputPath;
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
  const explicit = config.get('serverPath');
  if (explicit) {
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

function ensureOutputChannel() {
  if (!outputChannel) {
    outputChannel = vscode.window.createOutputChannel('Z3DK');
  }
  return outputChannel;
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

function resolveSymbolsPath(config, romPath) {
  const configured = expandHome(config.get('symbolsPath'));
  if (configured) {
    return configured;
  }
  if (!romPath) {
    return '';
  }
  const format = config.get('symbolFormat') || 'mesen';
  return format === 'mesen' ? `${romPath}.mlb` : `${romPath}.sym`;
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
  return `${expanded}${quotedArgs ? ` ${quotedArgs}` : ''}`;
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
  return '';
}

function buildDisassemblyCommand(config) {
  const command = config.get('disasmCommand');
  if (!command) {
    return '';
  }
  const romPath = expandHome(config.get('romPath')) || '';
  const symbolsPath = resolveSymbolsPath(config, romPath);
  const outputPath = expandHome(config.get('disasmOutputPath')) || '';
  const usdasmRoot = resolveUsdasmRoot(config);
  return applyTemplate(command, {
    rom: romPath,
    symbols: symbolsPath,
    output: outputPath,
    usdasm: usdasmRoot
  });
}

function ensureStatusBar(context) {
  if (statusItems.length > 0) {
    return;
  }
  const main = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 100);
  const lsp = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 99);
  const rom = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 98);
  const symbols = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 97);
  const mesen = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 96);
  const yaze = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 95);

  main.command = 'z3dk.openDashboard';
  lsp.command = 'z3dk.restartServer';
  rom.command = 'z3dk.openRomFolder';
  symbols.command = 'z3dk.exportSymbols';
  mesen.command = 'z3dk.launchMesen';
  yaze.command = 'z3dk.launchYaze';

  statusItems = [main, lsp, rom, symbols, mesen, yaze];
  statusItems.forEach(item => context.subscriptions.push(item));
}

function updateStatusBar(context) {
  ensureStatusBar(context);
  const config = vscode.workspace.getConfiguration('z3dk');
  const romPath = expandHome(config.get('romPath')) || '';
  const symbolsPath = resolveSymbolsPath(config, romPath);
  const mesenPath = expandHome(config.get('mesenPath')) || '';
  const yazePath = expandHome(config.get('yazePath')) || '';
  const lspLabel = client ? 'on' : 'off';

  const [main, lsp, rom, symbols, mesen, yaze] = statusItems;
  main.text = '$(triangle-up) Z3DK';
  main.tooltip = 'Open Z3DK dashboard';
  main.show();

  lsp.text = `$(pulse) z3lsp: ${lspLabel}`;
  lsp.tooltip = 'Restart z3lsp';
  lsp.show();

  if (romPath) {
    rom.text = `$(circuit-board) ROM: ${path.basename(romPath)}`;
    rom.tooltip = romPath;
  } else {
    rom.text = '$(circuit-board) ROM: unset';
    rom.tooltip = 'Set z3dk.romPath';
  }
  rom.show();

  if (symbolsPath && fs.existsSync(symbolsPath)) {
    symbols.text = `$(database) Symbols: ready`;
    symbols.tooltip = symbolsPath;
  } else {
    symbols.text = `$(database) Symbols: missing`;
    symbols.tooltip = symbolsPath || 'Set z3dk.symbolsPath';
  }
  symbols.show();

  if (mesenPath) {
    mesen.text = `$(debug-alt-small) Mesen2: ${fs.existsSync(mesenPath) ? 'ready' : 'missing'}`;
    mesen.tooltip = mesenPath;
  } else {
    mesen.text = '$(debug-alt-small) Mesen2: unset';
    mesen.tooltip = 'Set z3dk.mesenPath';
  }
  mesen.show();

  if (yazePath) {
    yaze.text = `$(rocket) yaze: ${fs.existsSync(yazePath) ? 'ready' : 'missing'}`;
    yaze.tooltip = yazePath;
  } else {
    yaze.text = '$(rocket) yaze: unset';
    yaze.tooltip = 'Set z3dk.yazePath';
  }
  yaze.show();
}

class CommandItem extends vscode.TreeItem {
  constructor(label, command, tooltip, iconId) {
    super(label, vscode.TreeItemCollapsibleState.None);
    this.command = command;
    this.tooltip = tooltip || label;
    if (iconId) {
      this.iconPath = new vscode.ThemeIcon(iconId);
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
    if (element) {
      return [];
    }
    return this.buildItems();
  }

  buildItems() {
    return [
      new CommandItem(
        'Open Dev Workspace',
        { command: 'z3dk.openWorkspace', title: 'Open Dev Workspace' },
        'Open the multi-root workspace for Oracle + Yaze + Z3DK',
        'file-directory'
      ),
      new CommandItem(
        'Open Z3DK README',
        { command: 'z3dk.openReadme', title: 'Open Z3DK README' },
        'Open Z3DK README',
        'book'
      ),
      new CommandItem(
        'Open Model Catalog',
        { command: 'z3dk.openModelCatalog', title: 'Open Model Catalog' },
        'Open the Zelda model catalog',
        'library'
      ),
      new CommandItem(
        'Open Model Portfolio',
        { command: 'z3dk.openModelPortfolio', title: 'Open Model Portfolio' },
        'Open the AFS model portfolio',
        'map'
      ),
      new CommandItem(
        'Open Continue Config (TS)',
        { command: 'z3dk.openContinueConfigTs', title: 'Open Continue Config (TS)' },
        'Open Continue config.ts',
        'gear'
      ),
      new CommandItem(
        'Open Continue Config (YAML)',
        { command: 'z3dk.openContinueConfig', title: 'Open Continue Config (YAML)' },
        'Open Continue config.yaml',
        'gear'
      ),
      new CommandItem(
        'Open AFS Scratchpad',
        { command: 'z3dk.openAfsScratchpad', title: 'Open AFS Scratchpad' },
        'Open the z3dk AFS scratchpad',
        'note'
      ),
      new CommandItem(
        'Add AFS Context Folders',
        { command: 'z3dk.addAfsContexts', title: 'Add AFS Context Folders' },
        'Add .context folders to the workspace',
        'root-folder'
      ),
      new CommandItem(
        'Build Z3DK',
        { command: 'z3dk.build', title: 'Build Z3DK' },
        'Run the configured build command',
        'tools'
      ),
      new CommandItem(
        'Run Z3DK Tests',
        { command: 'z3dk.runTests', title: 'Run Z3DK Tests' },
        'Run regression tests',
        'beaker'
      ),
      new CommandItem(
        'Export Mesen Symbols',
        { command: 'z3dk.exportSymbols', title: 'Export Mesen Symbols' },
        'Export Mesen .mlb symbols via yaze',
        'database'
      ),
      new CommandItem(
        'Restart Language Server',
        { command: 'z3dk.restartServer', title: 'Restart Language Server' },
        'Restart z3lsp',
        'refresh'
      )
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
        'z3dk.openModelCatalog',
        'z3dk.openModelPortfolio',
        'z3dk.openContinueConfig',
        'z3dk.openContinueConfigTs',
        'z3dk.openAfsScratchpad',
        'z3dk.addAfsContexts',
        'z3dk.openOracleRepo',
        'z3dk.openYazeRepo',
        'z3dk.openMesenRepo'
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

function buildDashboardHtml(context) {
  const config = vscode.workspace.getConfiguration('z3dk');
  const rootDir = z3dkRoot(context) || workspaceRoot();
  const serverPath = resolveServerPath(config, rootDir);
  const romPath = expandHome(config.get('romPath')) || '';
  const symbolsPath = resolveSymbolsPath(config, romPath);
  const yazePath = expandHome(config.get('yazePath')) || 'yaze';
  const devWorkspacePath = expandHome(config.get('devWorkspacePath')) || '';
  const modelCatalogPath = resolveModelCatalogPath(config);
  const modelPortfolioPath = resolveModelPortfolioPath(config);
  const continueConfigPath = resolveContinueConfigPath(config);
  const continueConfigTsPath = resolveContinueConfigTsPath(config);
  const oracleRoot = resolveRepoPath('oracle-of-secrets', 'oracleRoot', config);
  const yazeRoot = resolveRepoPath('yaze', 'yazeRoot', config);
  const mesenRoot = resolveRepoPath('mesen2-oos', 'mesenRoot', config);
  const z3dkRootPath = z3dkRoot(context);
  const afsScratchpad = z3dkRootPath
    ? path.join(z3dkRootPath, '.context', 'scratchpad', 'state.md')
    : '';

  const statuses = [
    { label: 'z3lsp', ...pathStatus(serverPath) },
    { label: 'yaze', ...pathStatus(yazePath) },
    { label: 'rom', ...pathStatus(romPath) },
    { label: 'symbols', ...pathStatus(symbolsPath) },
    { label: 'mesen2-oos', ...pathStatus(mesenRoot) }
  ];

  const docLinks = [
    { label: 'Model Catalog', path: modelCatalogPath },
    { label: 'Model Portfolio', path: modelPortfolioPath },
    { label: 'AFS Scratchpad', path: afsScratchpad },
    { label: 'Continue config.yaml', path: continueConfigPath },
    { label: 'Continue config.ts', path: continueConfigTsPath }
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

  const docRows = docLinks
    .map(item => {
      const status = pathStatus(item.path);
      return `
        <div class="status-row">
          <div class="status-label">${escapeHtml(item.label)}</div>
          <div class="status-value">${escapeHtml(status.value)}</div>
          <div class="status-pill ${status.exists ? 'ok' : 'warn'}">${status.exists ? 'linked' : 'missing'}</div>
        </div>
      `;
    })
    .join('');

  const infoBadges = [
    'asar-first',
    devWorkspacePath ? 'workspace linked' : 'workspace unset'
  ];

  return `
    <!DOCTYPE html>
    <html lang="en">
    <head>
      <meta charset="UTF-8" />
      <meta name="viewport" content="width=device-width, initial-scale=1.0" />
      <style>
        :root {
          --ink: #1a1a1a;
          --subtle: rgba(0, 0, 0, 0.65);
          --card: rgba(255, 255, 255, 0.65);
          --card-strong: rgba(255, 255, 255, 0.8);
          --accent: #2a7f6f;
          --accent-2: #d07a2d;
          --accent-3: #3e6fb0;
          --warn: #c86f3f;
          --ok: #2c7d6f;
          --border: rgba(26, 26, 26, 0.12);
        }

        body {
          margin: 0;
          padding: 20px 16px 40px;
          color: var(--vscode-foreground, var(--ink));
          font-family: "Space Grotesk", "Avenir Next", "Helvetica Neue", sans-serif;
          background: radial-gradient(circle at 10% 10%, #f7efe2 0%, transparent 45%),
            radial-gradient(circle at 85% 15%, #d7efe7 0%, transparent 42%),
            linear-gradient(160deg, #f7f2ea 0%, #e8eef2 70%);
          min-height: 100vh;
        }

        .hero {
          position: relative;
          padding: 18px 16px 20px;
          border-radius: 18px;
          background: var(--card-strong);
          border: 1px solid var(--border);
          box-shadow: 0 12px 30px rgba(0, 0, 0, 0.08);
          overflow: hidden;
        }

        .hero::after {
          content: "";
          position: absolute;
          top: -20px;
          right: -40px;
          width: 140px;
          height: 140px;
          background: conic-gradient(from 180deg, rgba(42, 127, 111, 0.35), rgba(208, 122, 45, 0.3), rgba(62, 111, 176, 0.25));
          border-radius: 50%;
          filter: blur(0.5px);
          opacity: 0.7;
        }

        .hero h1 {
          margin: 0 0 6px;
          font-size: 20px;
          letter-spacing: 0.3px;
        }

        .hero p {
          margin: 0 0 12px;
          font-size: 12.5px;
          color: var(--subtle);
          line-height: 1.4;
        }

        .badges {
          display: flex;
          flex-wrap: wrap;
          gap: 8px;
        }

        .badge {
          padding: 4px 10px;
          border-radius: 999px;
          font-size: 11px;
          background: rgba(42, 127, 111, 0.12);
          color: #1f5d52;
          border: 1px solid rgba(42, 127, 111, 0.2);
          text-transform: uppercase;
          letter-spacing: 0.8px;
        }

        .grid {
          display: grid;
          grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
          gap: 14px;
          margin-top: 16px;
        }

        .card {
          padding: 14px 14px 16px;
          border-radius: 16px;
          background: var(--card);
          border: 1px solid var(--border);
          box-shadow: 0 8px 20px rgba(0, 0, 0, 0.05);
        }

        .card h2 {
          margin: 0 0 10px;
          font-size: 14px;
          text-transform: uppercase;
          letter-spacing: 1px;
          color: rgba(26, 26, 26, 0.7);
        }

        .button-grid {
          display: grid;
          grid-template-columns: repeat(auto-fit, minmax(160px, 1fr));
          gap: 8px;
        }

        button {
          appearance: none;
          border: none;
          border-radius: 12px;
          padding: 10px 12px;
          background: rgba(42, 127, 111, 0.12);
          color: #1e544c;
          font-size: 12px;
          font-weight: 600;
          letter-spacing: 0.3px;
          cursor: pointer;
          transition: transform 0.15s ease, box-shadow 0.15s ease, background 0.2s ease;
        }

        button:hover {
          transform: translateY(-1px);
          box-shadow: 0 4px 10px rgba(0, 0, 0, 0.1);
          background: rgba(42, 127, 111, 0.18);
        }

        button.secondary {
          background: rgba(208, 122, 45, 0.15);
          color: #7a4017;
        }

        button.secondary:hover {
          background: rgba(208, 122, 45, 0.2);
        }

        button.tertiary {
          background: rgba(62, 111, 176, 0.15);
          color: #2a4d82;
        }

        button.tertiary:hover {
          background: rgba(62, 111, 176, 0.22);
        }

        .status {
          display: flex;
          flex-direction: column;
          gap: 8px;
        }

        .status-row {
          display: grid;
          grid-template-columns: 90px 1fr auto;
          gap: 8px;
          align-items: center;
        }

        .status-label {
          font-size: 11px;
          text-transform: uppercase;
          letter-spacing: 0.8px;
          color: rgba(26, 26, 26, 0.6);
        }

        .status-value {
          font-family: "Azeret Mono", "JetBrains Mono", monospace;
          font-size: 11px;
          color: rgba(26, 26, 26, 0.8);
          overflow-wrap: anywhere;
        }

        .status-pill {
          padding: 4px 8px;
          border-radius: 999px;
          font-size: 10px;
          text-transform: uppercase;
          letter-spacing: 0.6px;
          border: 1px solid transparent;
        }

        .status-pill.ok {
          background: rgba(42, 127, 111, 0.2);
          color: #1f5d52;
          border-color: rgba(42, 127, 111, 0.3);
        }

        .status-pill.warn {
          background: rgba(200, 111, 63, 0.2);
          color: #6b3b1b;
          border-color: rgba(200, 111, 63, 0.3);
        }

        .note {
          margin-top: 12px;
          font-size: 11px;
          color: rgba(26, 26, 26, 0.55);
        }
      </style>
    </head>
    <body>
      <section class="hero">
        <h1>Z3DK Command Deck</h1>
        <p>Asar-first workflows, LSP boosts, emulator tooling, and model intelligence in one cockpit.</p>
        <div class="badges">
          ${infoBadges.map(badge => `<span class="badge">${escapeHtml(badge)}</span>`).join('')}
        </div>
      </section>

      <section class="grid">
        <div class="card">
          <h2>Quick Actions</h2>
          <div class="button-grid">
            <button data-command="z3dk.build">Build Z3DK</button>
            <button data-command="z3dk.runTests">Run Tests</button>
            <button data-command="z3dk.exportSymbols" class="secondary">Export Symbols</button>
            <button data-command="z3dk.restartServer" class="tertiary">Restart z3lsp</button>
            <button data-command="z3dk.refreshDashboard" class="secondary">Refresh Dashboard</button>
          </div>
          <div class="note">Defaulting to Asar semantics while z3dk stabilizes.</div>
        </div>

        <div class="card">
          <h2>Integrations</h2>
          <div class="button-grid">
            <button data-command="z3dk.openWorkspace">Open Dev Workspace</button>
            <button data-command="z3dk.openReadme" class="secondary">Open Z3DK README</button>
            <button data-command="z3dk.openOracleRepo">Open Oracle Repo</button>
            <button data-command="z3dk.openYazeRepo" class="tertiary">Open Yaze Repo</button>
            <button data-command="z3dk.openMesenRepo" class="secondary">Open Mesen2-OOS</button>
          </div>
        </div>

        <div class="card">
          <h2>Models + AFS</h2>
          <div class="button-grid">
            <button data-command="z3dk.openModelCatalog">Model Catalog</button>
            <button data-command="z3dk.openModelPortfolio" class="secondary">Model Portfolio</button>
            <button data-command="z3dk.openAfsScratchpad" class="tertiary">Open AFS Scratchpad</button>
            <button data-command="z3dk.addAfsContexts">Add AFS Contexts</button>
          </div>
        </div>

        <div class="card">
          <h2>Continue</h2>
          <div class="button-grid">
            <button data-command="z3dk.openContinueConfigTs" class="secondary">Open config.ts</button>
            <button data-command="z3dk.openContinueConfig">Open config.yaml</button>
          </div>
        </div>
      </section>

      <section class="grid">
        <div class="card">
          <h2>Runtime Status</h2>
          <div class="status">
            ${statusRows}
          </div>
        </div>
        <div class="card">
          <h2>Docs & Profiles</h2>
          <div class="status">
            ${docRows}
          </div>
        </div>
      </section>

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
}

async function startClient(context) {
  if (client) {
    return client;
  }

  const config = vscode.workspace.getConfiguration('z3dk');
  if (!config.get('autoStart')) {
    return undefined;
  }

  const rootDir = z3dkRoot(context) || workspaceRoot();
  const serverPath = resolveServerPath(config, rootDir);
  const serverArgs = config.get('serverArgs') || [];

  const serverOptions = {
    command: serverPath,
    args: serverArgs,
    options: {
      cwd: rootDir || undefined
    }
  };

  const clientOptions = {
    documentSelector: [{ language: 'asar', scheme: 'file' }],
    outputChannel: ensureOutputChannel()
  };

  client = new LanguageClient('z3dk', 'Z3DK Language Server', serverOptions, clientOptions);
  context.subscriptions.push(client.start());
  return client;
}

async function stopClient() {
  if (!client) {
    return;
  }
  const current = client;
  client = undefined;
  await current.stop();
}

async function restartClient(context) {
  await stopClient();
  await startClient(context);
}

function buildExportSymbolsCommand(config) {
  const romPath = expandHome(config.get('romPath'));
  if (!romPath) {
    vscode.window.showWarningMessage('Set z3dk.romPath before exporting symbols.');
    return null;
  }
  const yazePath = expandHome(config.get('yazePath')) || 'yaze';
  const format = config.get('symbolFormat') || 'mesen';
  const configuredOutput = expandHome(config.get('symbolsPath'));
  let outputPath = configuredOutput;
  if (!outputPath) {
    outputPath = format === 'mesen' ? `${romPath}.mlb` : `${romPath}.sym`;
  }
  const symCandidate = `${romPath}.sym`;
  if (fs.existsSync(symCandidate)) {
    return `${yazePath} --export_symbols_fast --export_symbols "${outputPath}" --symbol_format ${format} --load_symbols "${symCandidate}"`;
  }
  return `${yazePath} --headless --rom_file "${romPath}" --export_symbols "${outputPath}" --symbol_format ${format}`;
}

function activate(context) {
  ensureOutputChannel();
  commandProvider = new Z3dkCommandProvider(context);
  context.subscriptions.push(vscode.window.registerTreeDataProvider('z3dk.commands', commandProvider));
  dashboardProvider = new Z3dkDashboardProvider(context);
  context.subscriptions.push(
    vscode.window.registerWebviewViewProvider('z3dk.dashboard', dashboardProvider)
  );

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
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.exportSymbols', () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const command = buildExportSymbolsCommand(config);
    const rootDir = z3dkRoot(context) || workspaceRoot();
    if (command) {
      runInTerminal(command, rootDir);
    }
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.restartServer', async () => {
    await restartClient(context);
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.refresh', () => {
    if (commandProvider) {
      commandProvider.refresh();
    }
    if (dashboardProvider) {
      dashboardProvider.refresh();
    }
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.refreshDashboard', () => {
    if (dashboardProvider) {
      dashboardProvider.refresh();
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
      restartClient(context);
      if (dashboardProvider) {
        dashboardProvider.refresh();
      }
    }
  }));

  startClient(context);
}

async function deactivate() {
  await stopClient();
}

module.exports = {
  activate,
  deactivate
};
