const fs = require('fs');
const path = require('path');
const vscode = require('vscode');
const { LanguageClient } = require('vscode-languageclient/node');

let client;
let outputChannel;
let terminal;
let commandProvider;

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
    path.join(rootDir, 'build', 'src', 'z3lsp', 'z3lsp'),
    path.join(rootDir, 'build-z3dk-foundation', 'src', 'z3lsp', 'z3lsp'),
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
