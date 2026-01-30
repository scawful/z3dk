#!/usr/bin/env python3
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time
from select import select

DEBUG = bool(os.environ.get('Z3LSP_TEST_DEBUG'))


def find_z3lsp() -> str:
    # Test file is at tests/z3dk/test_z3lsp_rules.py; repo root is parents[1]
    test_dir = pathlib.Path(__file__).resolve().parent
    repo_root = test_dir.parents[1]  # z3dk repo root (tests/z3dk -> tests -> repo)
    candidates = [
        repo_root / 'build' / 'bin' / 'z3lsp',
        repo_root / 'build' / 'src' / 'z3lsp' / 'z3lsp',
        repo_root / 'build' / 'z3lsp' / 'z3lsp',
    ]
    for candidate in candidates:
        if candidate.exists():
            return str(candidate)
    path = shutil.which('z3lsp')
    if path:
        return path
    raise FileNotFoundError('z3lsp binary not found (build it first)')


class LspClient:
    def __init__(self, executable: str):
        self.proc = subprocess.Popen(
            [executable],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
        )
        self.buffer = b''

    def send(self, payload: dict):
        body = json.dumps(payload).encode('utf-8')
        header = f"Content-Length: {len(body)}\r\n\r\n".encode('ascii')
        assert self.proc.stdin is not None
        self.proc.stdin.write(header + body)
        self.proc.stdin.flush()

    def read_message(self, timeout: float = 2.0):
        end_time = time.time() + timeout
        while time.time() < end_time:
            if b"\r\n\r\n" not in self.buffer:
                stdout = self.proc.stdout
                if stdout is None:
                    break
                ready, _, _ = select([stdout], [], [], 0.1)
                if not ready:
                    continue
                chunk = os.read(stdout.fileno(), 4096)
                if not chunk:
                    continue
                self.buffer += chunk
                continue

            header, rest = self.buffer.split(b"\r\n\r\n", 1)
            length = 0
            for line in header.split(b"\r\n"):
                if line.lower().startswith(b"content-length:"):
                    length = int(line.split(b":", 1)[1].strip())
                    break
            if length == 0 or len(rest) < length:
                stdout = self.proc.stdout
                if stdout is None:
                    break
                ready, _, _ = select([stdout], [], [], 0.1)
                if not ready:
                    continue
                chunk = os.read(stdout.fileno(), 4096)
                if not chunk:
                    continue
                self.buffer += chunk
                continue

            body = rest[:length]
            self.buffer = rest[length:]
            message = json.loads(body.decode('utf-8'))
            if DEBUG:
                print(f'[lsp] {message}', file=sys.stderr)
            return message
        return None

    def wait_for_diagnostics(self, uri: str, timeout: float = 4.0):
        end_time = time.time() + timeout
        while time.time() < end_time:
            message = self.read_message(timeout=0.5)
            if not message:
                continue
            if message.get('method') == 'textDocument/publishDiagnostics':
                params = message.get('params', {})
                if params.get('uri') == uri:
                    return params.get('diagnostics', [])
        raise TimeoutError(f'No diagnostics for {uri}')

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.proc.kill()


def write_file(path: pathlib.Path, contents: str):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(contents)


def _init_lsp_client(client: LspClient, root_uri: str) -> None:
    client.send({
        'jsonrpc': '2.0',
        'id': 1,
        'method': 'initialize',
        'params': {
            'rootUri': root_uri,
            'capabilities': {}
        }
    })
    init_deadline = time.time() + 4.0
    initialized = False
    while time.time() < init_deadline:
        message = client.read_message(timeout=0.5)
        if message and message.get('id') == 1:
            initialized = True
            break
    if not initialized:
        raise TimeoutError('No initialize response from z3lsp')
    client.send({'jsonrpc': '2.0', 'method': 'initialized', 'params': {}})


def run():
    """Original test: z3dk.toml with main = Oracle_main.asm; shared.asm included after org has no errors."""
    z3lsp = find_z3lsp()
    with tempfile.TemporaryDirectory() as tmpdir:
        root = pathlib.Path(tmpdir)
        write_file(root / 'z3dk.toml', 'main = "Oracle_main.asm"\n')

        write_file(
            root / 'Oracle_main.asm',
            'incsrc "shared_before.asm"\n'
            'org $008000\n'
            'OracleOnlyLabel:\n'
            '  NOP\n'
            'incsrc "shared.asm"\n'
            'incsrc "shared_after.asm"\n'
        )
        write_file(
            root / 'Meadow_main.asm',
            'org $008000\n'
            'MeadowOnlyLabel:\n'
            '  NOP\n'
            'incsrc "shared.asm"\n'
        )
        write_file(root / 'shared.asm', 'JSL OracleOnlyLabel\n')
        write_file(root / 'shared_before.asm', 'NOP\n')
        write_file(root / 'shared_after.asm', 'NOP\n')

        client = LspClient(z3lsp)
        try:
            root_uri = root.as_uri()
            _init_lsp_client(client, root_uri)

            shared_uri = (root / 'shared.asm').as_uri()
            client.send({
                'jsonrpc': '2.0',
                'method': 'textDocument/didOpen',
                'params': {
                    'textDocument': {
                        'uri': shared_uri,
                        'languageId': 'asar',
                        'version': 1,
                        'text': (root / 'shared.asm').read_text()
                    }
                }
            })
            shared_diags = client.wait_for_diagnostics(shared_uri)
            error_diags = [d for d in shared_diags if d.get('severity') == 1]
            if error_diags:
                raise AssertionError(f'Unexpected errors for shared.asm: {error_diags}')

            before_uri = (root / 'shared_before.asm').as_uri()
            client.send({
                'jsonrpc': '2.0',
                'method': 'textDocument/didOpen',
                'params': {
                    'textDocument': {
                        'uri': before_uri,
                        'languageId': 'asar',
                        'version': 1,
                        'text': (root / 'shared_before.asm').read_text()
                    }
                }
            })
            before_diags = client.wait_for_diagnostics(before_uri)
            if not any('Missing org or freespace command' in d.get('message', '') for d in before_diags):
                raise AssertionError('Expected missing org diagnostic for shared_before.asm')

            after_uri = (root / 'shared_after.asm').as_uri()
            client.send({
                'jsonrpc': '2.0',
                'method': 'textDocument/didOpen',
                'params': {
                    'textDocument': {
                        'uri': after_uri,
                        'languageId': 'asar',
                        'version': 1,
                        'text': (root / 'shared_after.asm').read_text()
                    }
                }
            })
            after_diags = client.wait_for_diagnostics(after_uri)
            if any('Missing org or freespace command' in d.get('message', '') for d in after_diags):
                raise AssertionError('Unexpected missing org diagnostic for shared_after.asm')

        finally:
            client.close()
            if client.proc.stderr:
                try:
                    err = client.proc.stderr.read()
                    if err:
                        print(err.decode('utf-8', errors='ignore'), file=sys.stderr)
                except Exception:
                    pass

    print('z3lsp rules test: ok')


def test_no_toml_main_asm_include_no_missing_org():
    """With no z3dk.toml, workspace has only Main.asm; opening an include after org must not show Missing org."""
    z3lsp = find_z3lsp()
    with tempfile.TemporaryDirectory() as tmpdir:
        root = pathlib.Path(tmpdir)
        # No z3dk.toml - SeedMainCandidates will pick Main.asm
        write_file(
            root / 'Main.asm',
            'org $008000\n'
            'MainLabel:\n'
            '  NOP\n'
            'incsrc "included.asm"\n'
        )
        write_file(root / 'included.asm', 'JSL MainLabel\n')

        client = LspClient(z3lsp)
        try:
            _init_lsp_client(client, root.as_uri())
            included_uri = (root / 'included.asm').as_uri()
            client.send({
                'jsonrpc': '2.0',
                'method': 'textDocument/didOpen',
                'params': {
                    'textDocument': {
                        'uri': included_uri,
                        'languageId': 'asar',
                        'version': 1,
                        'text': (root / 'included.asm').read_text()
                    }
                }
            })
            diags = client.wait_for_diagnostics(included_uri)
            missing_org = [d for d in diags if 'Missing org or freespace command' in d.get('message', '')]
            assert not missing_org, f'Included file (after org in Main.asm) should not get Missing org: {missing_org}'
            errors = [d for d in diags if d.get('severity') == 1]
            assert not errors, f'Included file should have no errors when main defines MainLabel: {errors}'
        finally:
            client.close()


def test_diagnostic_path_included_file():
    """Diagnostic in an included file (subpath) must be attributed to that document, not dropped."""
    z3lsp = find_z3lsp()
    with tempfile.TemporaryDirectory() as tmpdir:
        root = pathlib.Path(tmpdir)
        write_file(root / 'z3dk.toml', 'main = "Main.asm"\ninclude_paths = ["."]\n')
        write_file(root / 'Main.asm', 'org $008000\nincsrc "subdir/other.asm"\n')
        (root / 'subdir').mkdir(exist_ok=True)
        # Intentional error in subdir/other.asm so we get a diagnostic for that file
        write_file(root / 'subdir/other.asm', 'JSL NonexistentLabel\n')

        client = LspClient(z3lsp)
        try:
            _init_lsp_client(client, root.as_uri())
            other_uri = (root / 'subdir' / 'other.asm').as_uri()
            client.send({
                'jsonrpc': '2.0',
                'method': 'textDocument/didOpen',
                'params': {
                    'textDocument': {
                        'uri': other_uri,
                        'languageId': 'asar',
                        'version': 1,
                        'text': (root / 'subdir' / 'other.asm').read_text()
                    }
                }
            })
            diags = client.wait_for_diagnostics(other_uri)
            # We expect a diagnostic (label not found) for this document; PathMatchesDocumentPath
            # must match so the diagnostic is not dropped. So we should see at least one diag for other.asm.
            # If the assembler reports with relative path "subdir/other.asm", it must match other_uri.
            errors = [d for d in diags if d.get('severity') == 1]
            assert any('wasn\'t found' in d.get('message', '') or 'Label' in d.get('message', '') for d in errors), \
                f'Expected label diagnostic for subdir/other.asm: {diags}'
        finally:
            client.close()


if __name__ == '__main__':
    try:
        run()
    except Exception as exc:
        print(f'z3lsp rules test: failed: {exc}', file=sys.stderr)
        sys.exit(1)
