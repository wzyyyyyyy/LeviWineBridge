#!/usr/bin/env python3
"""Exercise the built client through Sikarugir, without game files or real accounts."""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import socket
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

p = argparse.ArgumentParser()
p.add_argument('--compiler', required=True, help='zig cc -target x86_64-windows-gnu')
p.add_argument('--launcher', required=True, type=Path, help='Sikarugir Contents/MacOS/wineskinlauncher')
p.add_argument('--output', required=True, type=Path)
p.add_argument('--client', type=Path, help='Alternate client artifact for regression comparison')
args = p.parse_args()
root = Path(__file__).resolve().parents[1]
source = Path(__file__).resolve().parent
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
cc = shlex.split(args.compiler) + ['-O2']
subprocess.run(cc + ['-shared', str(source/'fixture_api.c'), str(source/'fixture-api.def'), '-o', str(output/'fixture-api.dll')], check=True)
subprocess.run(cc + ['-shared', '-fno-builtin', str(source/'fixture_runtime.c'), str(output/'fixture_api.lib'), '-o', str(output/'xgameruntime.dll')], check=True)
subprocess.run(cc + ['-shared', str(source/'fixture_preloader.c'), '-o', str(output/'PreLoader.dll')], check=True)
subprocess.run(cc + [str(source/'fixture_main.c'), '-lshell32', '-lcomdlg32', '-lole32', '-o', str(output/'fixture.exe')], check=True)
shutil.copyfile(args.client or root/'dist/LeviWineBridge.dll', output/'LeviWineBridge.dll')

mode, received = 'accepted', []
class Handler(BaseHTTPRequestHandler):
    def log_message(self, *unused): pass
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        assert self.headers['Authorization'] == 'Bearer fixture-token'
        received.append((self.path, body))
        self.send_response(200 if mode == 'accepted' else 503)
        self.send_header('Content-Length', '0')
        self.end_headers()

server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
report = {}
try:
    for mode, expected in [('accepted', 1), ('fallback', 2), ('inert', 2)]:
        received.clear()
        result = output/'result.txt'
        result.unlink(missing_ok=True)
        env = {k: v for k, v in os.environ.items() if not k.startswith(('WINE', 'DYLD_')) and k not in ('LEVI_GAME_AUTH_URL', 'LEVI_GAME_AUTH_TOKEN')}
        if mode != 'inert':
            env.update(LEVI_GAME_AUTH_URL=f'http://127.0.0.1:{server.server_port}/v1/game-login/fixture', LEVI_GAME_AUTH_TOKEN='fixture-token')
        subprocess.run([str(args.launcher), str(output/'fixture.exe')], cwd=output, env=env, timeout=30, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        deadline = time.monotonic() + 30
        while not result.exists() and time.monotonic() < deadline:
            time.sleep(0.1)
        actual = result.read_text().strip()
        guard = -1 if mode == 'inert' else 1
        assert actual == f'prompt={expected} other=2 guard={guard}', (mode, actual)
        if mode == 'accepted':
            assert received == [('/v1/game-login/fixture', {'code': 'AB12CD34', 'expiresIn': 900, 'resultEvents': True}), ('/v1/game-login/fixture/result', {'phase': 'failed', 'errorCode': '80004005'})], received
        elif mode == 'inert':
            assert not received, received
        report[mode] = {'passed': True, 'promptResult': expected, 'unrelatedPromptResult': 2, 'heapGuard': guard == 1 if mode != 'inert' else 'inert'}
        print(f'{mode}: passed', flush=True)

    # Interactive console bridge test (bidirectional UTF-8 + ANSI + ReadConsoleW + stdin)
    console_out = bytearray()
    console_ready = threading.Event()
    csock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    csock.bind(('127.0.0.1', 0))
    csock.listen(1)
    cport = csock.getsockname()[1]

    def console_server():
        try:
            csock.settimeout(25)
            conn, _ = csock.accept()
            with conn:
                conn.settimeout(15)
                buf = b''
                while b'\n' not in buf:
                    chunk = conn.recv(256)
                    if not chunk:
                        return
                    buf += chunk
                line, rest = buf.split(b'\n', 1)
                assert line == b'GAME sess:ctoken', line
                conn.sendall(b'OK\n')
                if rest:
                    console_out.extend(rest)
                console_ready.set()
                conn.sendall(b'cmd1\ncmd2\n')
                while True:
                    try:
                        chunk = conn.recv(4096)
                    except socket.timeout:
                        break
                    if not chunk:
                        break
                    console_out.extend(chunk)
        finally:
            csock.close()

    cthread = threading.Thread(target=console_server, daemon=True)
    cthread.start()
    result = output/'result.txt'
    result.unlink(missing_ok=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('WINE', 'DYLD_')) and k not in ('LEVI_GAME_AUTH_URL', 'LEVI_GAME_AUTH_TOKEN', 'LEVI_CONSOLE_ADDR', 'LEVI_CONSOLE_TOKEN')}
    env.update(
        LEVI_GAME_AUTH_URL=f'http://127.0.0.1:{server.server_port}/v1/game-login/fixture',
        LEVI_GAME_AUTH_TOKEN='fixture-token',
        LEVI_CONSOLE_ADDR=f'127.0.0.1:{cport}',
        LEVI_CONSOLE_TOKEN='sess:ctoken',
    )
    mode = 'accepted'
    received.clear()
    subprocess.run([str(args.launcher), str(output/'fixture.exe')], cwd=output, env=env, timeout=30, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    deadline = time.monotonic() + 30
    while not result.exists() and time.monotonic() < deadline:
        time.sleep(0.1)
    cthread.join(timeout=5)
    actual = result.read_text().strip()
    assert actual == 'prompt=1 other=2 guard=1 console=1', (actual, console_out)
    text = console_out.decode('utf-8', errors='replace')
    assert '\x1b[0;92m' in text and '你好-W\n' in text and 'printf-ok\n' in text and 'echo:cmd2\n' in text, repr(text)
    report['console'] = {'passed': True, 'output': text}
    print('console: passed', flush=True)

    # Wine input fix test (stuck-key release on focus loss, Cmd+V -> Ctrl+V)
    result = output/'result.txt'
    result.unlink(missing_ok=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('WINE', 'DYLD_')) and k not in ('LEVI_GAME_AUTH_URL', 'LEVI_GAME_AUTH_TOKEN', 'LEVI_CONSOLE_ADDR', 'LEVI_CONSOLE_TOKEN', 'LEVI_WINE_INPUT_FIX')}
    env.update(LEVI_WINE_INPUT_FIX='1')
    subprocess.run([str(args.launcher), str(output/'fixture.exe')], cwd=output, env=env, timeout=30, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    deadline = time.monotonic() + 30
    while not result.exists() and time.monotonic() < deadline:
        time.sleep(0.1)
    actual = result.read_text().strip()
    assert actual == 'prompt=2 other=2 guard=-1 input=1', actual
    report['input'] = {'passed': True}
    print('input: passed', flush=True)

    # Shell & native file/folder dialog bridge test
    shell_events = []
    class ShellHandler(BaseHTTPRequestHandler):
        def log_message(self, *unused): pass
        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            assert self.headers['Authorization'] == 'Bearer shell-token'
            shell_events.append((self.path, body))
            if self.path == '/v1/shell/fixture/open':
                resp = json.dumps({'ok': True}).encode()
            elif self.path == '/v1/shell/fixture/dialog':
                kind = body.get('kind')
                if kind == 'open':
                    resp = json.dumps({'cancelled': False, 'paths': [str(output / '123中文' / 'shaderpacks' / 'picked.txt')]}).encode()
                elif kind == 'save':
                    resp = json.dumps({'cancelled': True, 'paths': []}).encode()
                elif kind == 'folder':
                    resp = json.dumps({'cancelled': False, 'paths': [str(output / '123中文' / 'shaderpacks')]}).encode()
                else:
                    self.send_response(400)
                    self.end_headers()
                    return
            else:
                self.send_response(404)
                self.end_headers()
                return
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(resp)))
            self.end_headers()
            self.wfile.write(resp)

    shell_server = ThreadingHTTPServer(('127.0.0.1', 0), ShellHandler)
    threading.Thread(target=shell_server.serve_forever, daemon=True).start()
    try:
        result = output/'result.txt'
        result.unlink(missing_ok=True)
        env = {k: v for k, v in os.environ.items() if not k.startswith(('WINE', 'DYLD_')) and k not in ('LEVI_GAME_AUTH_URL', 'LEVI_GAME_AUTH_TOKEN', 'LEVI_CONSOLE_ADDR', 'LEVI_CONSOLE_TOKEN', 'LEVI_WINE_INPUT_FIX', 'LEVI_SHELL_BRIDGE_URL', 'LEVI_SHELL_BRIDGE_TOKEN')}
        env.update(
            LEVI_SHELL_BRIDGE_URL=f'http://127.0.0.1:{shell_server.server_port}/v1/shell/fixture',
            LEVI_SHELL_BRIDGE_TOKEN='shell-token',
        )
        subprocess.run([str(args.launcher), str(output/'fixture.exe')], cwd=output, env=env, timeout=30, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        deadline = time.monotonic() + 30
        while not result.exists() and time.monotonic() < deadline:
            time.sleep(0.1)
        actual = result.read_text().strip()
        assert actual == 'prompt=2 other=2 guard=-1 shell=1', (actual, shell_events)
        assert len(shell_events) == 7, shell_events
        assert shell_events[0][0] == '/v1/shell/fixture/open' and shell_events[0][1]['reveal'] is False and '123中文/shaderpacks' in shell_events[0][1]['path'], shell_events[0]
        assert shell_events[1][0] == '/v1/shell/fixture/open' and shell_events[1][1]['reveal'] is False, shell_events[1]
        assert shell_events[2][0] == '/v1/shell/fixture/open' and shell_events[2][1]['reveal'] is True and shell_events[2][1]['path'].endswith('pack.zip'), shell_events[2]
        assert shell_events[3][0] == '/v1/shell/fixture/open' and shell_events[3][1]['reveal'] is False and '123中文/shaderpacks' in shell_events[3][1]['path'], shell_events[3]
        assert shell_events[4][0] == '/v1/shell/fixture/dialog' and shell_events[4][1]['kind'] == 'open' and len(shell_events[4][1].get('filters', [])) == 2, shell_events[4]
        assert shell_events[5][0] == '/v1/shell/fixture/dialog' and shell_events[5][1]['kind'] == 'save' and shell_events[5][1]['defaultName'] == 'out.txt', shell_events[5]
        assert shell_events[6][0] == '/v1/shell/fixture/dialog' and shell_events[6][1]['kind'] == 'folder', shell_events[6]
        report['shell'] = {'passed': True, 'events': len(shell_events)}
        print('shell: passed', flush=True)
    finally:
        shell_server.shutdown()
finally:
    server.shutdown()
(output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
