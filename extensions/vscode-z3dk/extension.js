const fs = require('fs');
const os = require('os');
const path = require('path');
const vscode = require('vscode');

let LanguageClient;

let client;
let outputChannel;
let terminal;
let commandProvider;
let dashboardProvider;
let statusItems = [];
let extensionContext;
let lspActive = false;
let codeLensProvider;
let languageClientLoadError;

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
  const explicit = expandHome(config.get('serverPath'));
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
  const escapedExecutable = expanded.includes(' ') ? `"${expanded}"` : expanded;
  return `${escapedExecutable}${quotedArgs ? ` ${quotedArgs}` : ''}`;
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

function buildDisassemblyCommand(config, context) {
  const romPath = expandHome(config.get('romPath')) || '';
  const symbolsPath = resolveSymbolsPath(config, romPath);
  const outputPath = expandHome(config.get('disasmOutputPath')) || '';
  const usdasmRoot = resolveUsdasmRoot(config);
  const command = config.get('disasmCommand');
  if (command) {
    return applyTemplate(command, {
      rom: romPath,
      symbols: symbolsPath,
      output: outputPath,
      usdasm: usdasmRoot
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
  return buildLaunchCommand(disasmExe, args);
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
  const romPath = expandHome(config.get('romPath')) || '';
  const symbolsPath = resolveSymbolsPath(config, romPath);
  const lspLabel = client ? 'on' : 'off';

  const [main, lsp, rom] = statusItems;
  main.text = '$(triangle-up) Z3DK';
  main.tooltip = 'Open Z3DK dashboard';
  main.show();

  lsp.text = `$(pulse) LSP ${lspLabel}`;
  lsp.tooltip = 'Restart z3lsp';
  lsp.show();

  if (romPath) {
    const romName = path.basename(romPath);
    const symReady = symbolsPath && fs.existsSync(symbolsPath) ? 'ok' : 'miss';
    rom.text = `$(circuit-board) ${romName} • sym ${symReady}`;
    rom.tooltip = `ROM: ${romPath}\nSymbols: ${symbolsPath || 'unset'}`;
  } else {
    rom.text = '$(circuit-board) ROM unset';
    rom.tooltip = 'Set z3dk.romPath';
  }
  rom.show();
}

function setLspActive(active) {
  lspActive = active;
  vscode.commands.executeCommand('setContext', 'z3dk.lspActive', active);
  if (codeLensProvider) {
    codeLensProvider.refresh();
  }
}

function isAsarDocument(document) {
  return document && document.languageId === 'asar' && document.uri.scheme === 'file';
}

function initDecorations() {
  if (!labelDecorationType) {
    labelDecorationType = vscode.window.createTextEditorDecorationType({
      after: {
        color: new vscode.ThemeColor('descriptionForeground'),
        margin: '0 0 0 8px'
      }
    });
  }
  if (!orgDecorationType) {
    orgDecorationType = vscode.window.createTextEditorDecorationType({
      after: {
        color: new vscode.ThemeColor('textLink.foreground'),
        margin: '0 0 0 8px'
      }
    });
  }
}

function clearAnnotations(editor) {
  if (!editor) {
    return;
  }
  if (labelDecorationType) {
    editor.setDecorations(labelDecorationType, []);
  }
  if (orgDecorationType) {
    editor.setDecorations(orgDecorationType, []);
  }
}

function updateAnnotations(editor) {
  if (!editor || !isAsarDocument(editor.document)) {
    return;
  }
  const config = vscode.workspace.getConfiguration('z3dk');
  if (!lspActive || !config.get('editorAnnotations')) {
    clearAnnotations(editor);
    return;
  }
  initDecorations();

  const labelRanges = [];
  const orgRanges = [];
  const doc = editor.document;
  const labelRegex = /^\s*[A-Za-z_][A-Za-z0-9_]*:/;
  const orgRegex = /^\s*org\b/i;
  for (let i = 0; i < doc.lineCount; i += 1) {
    const line = doc.lineAt(i);
    const text = line.text;
    if (labelRegex.test(text)) {
      labelRanges.push({
        range: new vscode.Range(i, text.length, i, text.length),
        renderOptions: { after: { contentText: 'label' } }
      });
    }
    if (orgRegex.test(text)) {
      orgRanges.push({
        range: new vscode.Range(i, text.length, i, text.length),
        renderOptions: { after: { contentText: 'org' } }
      });
    }
  }
  editor.setDecorations(labelDecorationType, labelRanges);
  editor.setDecorations(orgDecorationType, orgRanges);
}

function scheduleAnnotationUpdate(editor) {
  if (annotationTimer) {
    clearTimeout(annotationTimer);
  }
  annotationTimer = setTimeout(() => updateAnnotations(editor), 150);
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
        'Workspace',
        { command: 'z3dk.openWorkspace', title: 'Open Dev Workspace' },
        'Open the multi-root workspace for Oracle + Yaze + Z3DK',
        'file-directory'
      ),
      new CommandItem(
        'Dashboard',
        { command: 'z3dk.openDashboard', title: 'Open Dashboard' },
        'Open the Z3DK dashboard view',
        'layout'
      ),
      new CommandItem(
        'README',
        { command: 'z3dk.openReadme', title: 'Open Z3DK README' },
        'Open Z3DK README',
        'book'
      ),
      new CommandItem(
        'Model Catalog',
        { command: 'z3dk.openModelCatalog', title: 'Open Model Catalog' },
        'Open the Zelda model catalog',
        'library'
      ),
      new CommandItem(
        'Model Portfolio',
        { command: 'z3dk.openModelPortfolio', title: 'Open Model Portfolio' },
        'Open the AFS model portfolio',
        'map'
      ),
      new CommandItem(
        'Continue TS',
        { command: 'z3dk.openContinueConfigTs', title: 'Open Continue Config (TS)' },
        'Open Continue config.ts',
        'gear'
      ),
      new CommandItem(
        'Continue YAML',
        { command: 'z3dk.openContinueConfig', title: 'Open Continue Config (YAML)' },
        'Open Continue config.yaml',
        'gear'
      ),
      new CommandItem(
        'AFS Scratchpad',
        { command: 'z3dk.openAfsScratchpad', title: 'Open AFS Scratchpad' },
        'Open the z3dk AFS scratchpad',
        'note'
      ),
      new CommandItem(
        'Add AFS Contexts',
        { command: 'z3dk.addAfsContexts', title: 'Add AFS Context Folders' },
        'Add .context folders to the workspace',
        'root-folder'
      ),
      new CommandItem(
        'Build',
        { command: 'z3dk.build', title: 'Build Z3DK' },
        'Run the configured build command',
        'tools'
      ),
      new CommandItem(
        'Tests',
        { command: 'z3dk.runTests', title: 'Run Z3DK Tests' },
        'Run regression tests',
        'beaker'
      ),
      new CommandItem(
        'Symbols',
        { command: 'z3dk.exportSymbols', title: 'Export Mesen Symbols' },
        'Export Mesen .mlb symbols via yaze',
        'database'
      ),
      new CommandItem(
        'Mesen2',
        { command: 'z3dk.launchMesen', title: 'Mesen2-OOS' },
        'Launch Mesen2-OOS with optional ROM args',
        'debug-alt-small'
      ),
      new CommandItem(
        'yaze',
        { command: 'z3dk.launchYaze', title: 'yaze' },
        'Launch yaze with optional ROM args',
        'rocket'
      ),
      new CommandItem(
        'Hack Disasm',
        { command: 'z3dk.exportDisassembly', title: 'Export Hack Disassembly' },
        'Export USDASM-style disassembly of the hack',
        'symbol-structure'
      ),
      new CommandItem(
        'USDASM Search',
        { command: 'z3dk.findUsdasmLabel', title: 'Find USDASM Label' },
        'Search USDASM for a label',
        'search'
      ),
      new CommandItem(
        'USDASM Root',
        { command: 'z3dk.openUsdasmRoot', title: 'Open USDASM Root' },
        'Open USDASM source-of-truth disassembly',
        'folder'
      ),
      new CommandItem(
        'ROM Folder',
        { command: 'z3dk.openRomFolder', title: 'Open ROM Folder' },
        'Reveal ROM folder in Explorer',
        'file-submodule'
      ),
      new CommandItem(
        'Restart LSP',
        { command: 'z3dk.restartServer', title: 'Restart Language Server' },
        'Restart z3lsp',
        'refresh'
      )
    ];
  }
}

class Z3dkCodeLensProvider {
  constructor() {
    this._onDidChangeCodeLenses = new vscode.EventEmitter();
    this.onDidChangeCodeLenses = this._onDidChangeCodeLenses.event;
  }

  refresh() {
    this._onDidChangeCodeLenses.fire();
  }

  provideCodeLenses(document) {
    const config = vscode.workspace.getConfiguration('z3dk');
    if (!lspActive || !config.get('enableCodeLens') || !isAsarDocument(document)) {
      return [];
    }
    const top = new vscode.Range(0, 0, 0, 0);
    return [
      new vscode.CodeLens(top, {
        title: 'Z3DK Dashboard',
        command: 'z3dk.openDashboard'
      }),
      new vscode.CodeLens(top, {
        title: 'Symbols',
        command: 'z3dk.exportSymbols'
      }),
      new vscode.CodeLens(top, {
        title: 'USDASM Search',
        command: 'z3dk.findUsdasmLabel'
      })
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
        'z3dk.findUsdasmLabel',
        'z3dk.exportDisassembly',
        'z3dk.launchMesen',
        'z3dk.launchYaze',
        'z3dk.openRomFolder'
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
  try {
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
    const mesenPath = resolveMesenExecutable(config);
    const usdasmRoot = resolveUsdasmRoot(config);
    const disasmOutputPath = expandHome(config.get('disasmOutputPath')) || '';
    const z3dkRootPath = z3dkRoot(context);
    const afsScratchpad = z3dkRootPath
      ? path.join(z3dkRootPath, '.context', 'scratchpad', 'state.md')
      : '';

  const statuses = [
    { label: 'z3lsp', ...pathStatus(serverPath) },
    { label: 'yaze', ...pathStatus(yazePath) },
    { label: 'rom', ...pathStatus(romPath) },
    { label: 'symbols', ...pathStatus(symbolsPath) },
    { label: 'mesen2 app', ...pathStatus(mesenPath) },
    { label: 'usdasm', ...pathStatus(usdasmRoot) }
  ];

  const docLinks = [
    { label: 'Model Catalog', path: modelCatalogPath },
    { label: 'Model Portfolio', path: modelPortfolioPath },
    { label: 'AFS Scratchpad', path: afsScratchpad },
    { label: 'Oracle Repo', path: oracleRoot },
    { label: 'Yaze Repo', path: yazeRoot },
    { label: 'Mesen2 Repo', path: mesenRoot },
    { label: 'Continue config.yaml', path: continueConfigPath },
    { label: 'Continue config.ts', path: continueConfigTsPath },
    { label: 'Disasm Output', path: disasmOutputPath }
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
    devWorkspacePath ? 'workspace linked' : 'workspace unset',
    usdasmRoot ? 'usdasm linked' : 'usdasm unset'
  ];

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
          --accent: var(--vscode-focusBorder, #3b82f6);
          --ok: #3fb99a;
          --warn: #d08a4a;
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

        .grid {
          display: grid;
          grid-template-columns: 1fr;
          gap: 6px;
        }

        .card {
          padding: 8px;
          border-radius: 6px;
          background: var(--panel);
          border: 1px solid var(--border);
        }

        .card .accent-line {
          display: none;
        }

        .card h2 {
          margin: 0 0 6px;
          font-size: 10px;
          text-transform: uppercase;
          letter-spacing: 0.5px;
          color: var(--subtle);
        }

        .button-grid {
          display: flex;
          flex-wrap: wrap;
          gap: 4px;
        }

        button {
          appearance: none;
          border: 1px solid transparent;
          border-radius: 5px;
          padding: 4px 6px;
          background: var(--vscode-button-background, #2563eb);
          color: var(--vscode-button-foreground, #ffffff);
          font-size: 10px;
          font-weight: 600;
          letter-spacing: 0.1px;
          cursor: pointer;
          transition: background 0.15s ease, border-color 0.15s ease;
          min-width: 0;
        }

        button:hover {
          background: var(--vscode-button-hoverBackground, #1d4ed8);
        }

        button.secondary {
          background: var(--vscode-button-secondaryBackground, #e5e7eb);
          color: var(--vscode-button-secondaryForeground, #1f2937);
        }

        button.tertiary {
          background: transparent;
          color: var(--ink);
          border-color: var(--border);
        }

        .status {
          display: flex;
          flex-direction: column;
          gap: 8px;
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

        .note {
          margin-top: 4px;
          font-size: 9px;
          color: var(--subtle);
        }

        .mini {
          font-size: 9px;
          color: var(--subtle);
          margin-top: 4px;
        }
      </style>
    </head>
    <body>
      <div class="header">
        <div class="title">Z3DK</div>
        <div class="meta">${escapeHtml(infoBadges.join(' • '))}</div>
      </div>

      <section class="grid">
        <div class="card">
          <h2>Quick Actions</h2>
          <div class="accent-line"></div>
          <div class="button-grid">
            <button data-command="z3dk.build">Build</button>
            <button data-command="z3dk.runTests">Tests</button>
            <button data-command="z3dk.exportSymbols" class="secondary">Symbols</button>
            <button data-command="z3dk.restartServer" class="tertiary">Restart LSP</button>
            <button data-command="z3dk.refreshDashboard" class="secondary">Refresh</button>
          </div>
          <div class="note">Defaulting to Asar semantics while z3dk stabilizes.</div>
        </div>

        <div class="card">
          <h2>Integrations</h2>
          <div class="accent-line"></div>
          <div class="button-grid">
            <button data-command="z3dk.openWorkspace">Workspace</button>
            <button data-command="z3dk.openReadme" class="secondary">README</button>
            <button data-command="z3dk.openOracleRepo">Oracle Repo</button>
            <button data-command="z3dk.openYazeRepo" class="tertiary">Yaze Repo</button>
            <button data-command="z3dk.openMesenRepo" class="secondary">Mesen2 Repo</button>
          </div>
        </div>

        <div class="card">
          <h2>Models + AFS</h2>
          <div class="accent-line"></div>
          <div class="button-grid">
            <button data-command="z3dk.openModelCatalog">Model Catalog</button>
            <button data-command="z3dk.openModelPortfolio" class="secondary">Model Portfolio</button>
            <button data-command="z3dk.openAfsScratchpad" class="tertiary">AFS Scratchpad</button>
            <button data-command="z3dk.addAfsContexts">Add AFS Contexts</button>
          </div>
        </div>

        <div class="card">
          <h2>Continue</h2>
          <div class="accent-line"></div>
          <div class="button-grid">
            <button data-command="z3dk.openContinueConfigTs" class="secondary">Continue TS</button>
            <button data-command="z3dk.openContinueConfig">Continue YAML</button>
          </div>
          <div class="mini">Farore is the default chat + autocomplete model.</div>
        </div>

        <div class="card">
          <h2>Emulator Loop</h2>
          <div class="accent-line"></div>
          <div class="button-grid">
            <button data-command="z3dk.launchMesen">Mesen2-OOS</button>
            <button data-command="z3dk.launchYaze" class="secondary">yaze</button>
            <button data-command="z3dk.openRomFolder" class="tertiary">ROM Folder</button>
            <button data-command="z3dk.exportSymbols">Symbols</button>
          </div>
          <div class="mini">Use launch args to auto-load ROM + symbols.</div>
        </div>

        <div class="card">
          <h2>Disassembly Lab</h2>
          <div class="accent-line"></div>
          <div class="button-grid">
            <button data-command="z3dk.exportDisassembly">Hack Disasm</button>
            <button data-command="z3dk.findUsdasmLabel" class="secondary">USDASM Search</button>
            <button data-command="z3dk.openUsdasmRoot" class="tertiary">USDASM Root</button>
          </div>
          <div class="mini">USDASM is the source of truth for vanilla labels.</div>
        </div>
      </section>

      <section class="grid">
        <div class="card">
          <h2>Runtime Status</h2>
          <div class="accent-line"></div>
          <div class="status">
            ${statusRows}
          </div>
        </div>
        <div class="card">
          <h2>Docs & Profiles</h2>
          <div class="accent-line"></div>
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

async function startClient(context) {
  if (client) {
    return client;
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

  client = new Client('z3dk', 'Z3DK Language Server', serverOptions, clientOptions);
  context.subscriptions.push(client.start());
  setLspActive(true);
  updateStatusBar(context);
  scheduleAnnotationUpdate(vscode.window.activeTextEditor);
  return client;
}

async function stopClient() {
  if (!client) {
    return;
  }
  const current = client;
  client = undefined;
  await current.stop();
  setLspActive(false);
  clearAnnotations(vscode.window.activeTextEditor);
  if (extensionContext) {
    updateStatusBar(extensionContext);
  }
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
  extensionContext = context;
  setLspActive(false);
  ensureOutputChannel();
  ensureOutputChannel().appendLine('Z3DK extension activated.');
  commandProvider = new Z3dkCommandProvider(context);
  context.subscriptions.push(vscode.window.registerTreeDataProvider('z3dk.commands', commandProvider));
  dashboardProvider = new Z3dkDashboardProvider(context);
  context.subscriptions.push(
    vscode.window.registerWebviewViewProvider('z3dk.dashboard', dashboardProvider)
  );
  codeLensProvider = new Z3dkCodeLensProvider();
  context.subscriptions.push(
    vscode.languages.registerCodeLensProvider({ language: 'asar', scheme: 'file' }, codeLensProvider)
  );

  context.subscriptions.push(vscode.window.onDidChangeActiveTextEditor(editor => {
    scheduleAnnotationUpdate(editor);
  }));

  context.subscriptions.push(vscode.workspace.onDidChangeTextDocument(event => {
    const active = vscode.window.activeTextEditor;
    if (active && event.document === active.document) {
      scheduleAnnotationUpdate(active);
    }
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

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.openDashboard', async () => {
    await vscode.commands.executeCommand('workbench.view.extension.z3dk');
    if (dashboardProvider) {
      dashboardProvider.refresh();
    }
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.toggleAnnotations', async () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const current = config.get('editorAnnotations');
    await config.update('editorAnnotations', !current, vscode.ConfigurationTarget.Workspace);
    scheduleAnnotationUpdate(vscode.window.activeTextEditor);
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.launchMesen', () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const mesenExecutable = resolveMesenExecutable(config);
    if (!mesenExecutable) {
      vscode.window.showWarningMessage('Set z3dk.mesenPath to launch Mesen2-OOS.');
      return;
    }
    const romPath = expandHome(config.get('romPath')) || '';
    const symbolsPath = resolveSymbolsPath(config, romPath);
    const args = expandArgs(config.get('mesenArgs') || [], {
      rom: romPath,
      symbols: symbolsPath
    });
    const command = buildLaunchCommand(mesenExecutable, args);
    const rootDir = z3dkRoot(context) || workspaceRoot();
    runInTerminal(command, rootDir);
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.launchYaze', () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const yazeExecutable = expandHome(config.get('yazePath'));
    if (!yazeExecutable) {
      vscode.window.showWarningMessage('Set z3dk.yazePath to launch yaze.');
      return;
    }
    const romPath = expandHome(config.get('romPath')) || '';
    const symbolsPath = resolveSymbolsPath(config, romPath);
    const args = expandArgs(config.get('yazeLaunchArgs') || [], {
      rom: romPath,
      symbols: symbolsPath
    });
    const command = buildLaunchCommand(yazeExecutable, args);
    const rootDir = z3dkRoot(context) || workspaceRoot();
    runInTerminal(command, rootDir);
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.openRomFolder', async () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const romPath = expandHome(config.get('romPath'));
    if (!romPath || !fs.existsSync(romPath)) {
      vscode.window.showWarningMessage('Set z3dk.romPath to open the ROM folder.');
      return;
    }
    await revealFolder(path.dirname(romPath));
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

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.exportDisassembly', () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const command = buildDisassemblyCommand(config, context);
    if (!command) {
      vscode.window.showWarningMessage('Set z3dk.disasmCommand or build z3disasm to export the hack disassembly.');
      return;
    }
    const rootDir = z3dkRoot(context) || workspaceRoot();
    runInTerminal(command, rootDir);
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.refresh', () => {
    if (commandProvider) {
      commandProvider.refresh();
    }
    if (dashboardProvider) {
      dashboardProvider.refresh();
    }
    updateStatusBar(context);
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.refreshDashboard', () => {
    if (dashboardProvider) {
      dashboardProvider.refresh();
    }
    updateStatusBar(context);
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
      if (codeLensProvider) {
        codeLensProvider.refresh();
      }
      scheduleAnnotationUpdate(vscode.window.activeTextEditor);
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
