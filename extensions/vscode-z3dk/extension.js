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

async function startClient(context) {
  if (client) {
    return client;
  }

  const config = vscode.workspace.getConfiguration('z3dk');
  if (!config.get('autoStart')) {
    return undefined;
  }

  const rootDir = workspaceRoot();
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
  const romPath = config.get('romPath');
  if (!romPath) {
    vscode.window.showWarningMessage('Set z3dk.romPath before exporting symbols.');
    return null;
  }
  const yazePath = config.get('yazePath') || 'yaze';
  const format = config.get('symbolFormat') || 'mesen';
  const configuredOutput = config.get('symbolsPath');
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

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.runTests', () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const command = config.get('testCommand');
    const rootDir = workspaceRoot();
    runInTerminal(command, rootDir);
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.build', () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const command = config.get('buildCommand');
    const rootDir = workspaceRoot();
    runInTerminal(command, rootDir);
  }));

  context.subscriptions.push(vscode.commands.registerCommand('z3dk.exportSymbols', () => {
    const config = vscode.workspace.getConfiguration('z3dk');
    const command = buildExportSymbolsCommand(config);
    const rootDir = workspaceRoot();
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
