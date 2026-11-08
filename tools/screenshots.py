"""Render real UI code with demo data. No board, serial port or credentials needed.

python tools/screenshots.py                # device + web screenshots
python tools/screenshots.py --device-only  # native LVGL only
python tools/screenshots.py --web-only     # existing HTML + local demo APIs
"""
import argparse
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import subprocess
import threading
import time
from urllib.parse import parse_qs

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / '.local' / 'screenshots'
IMAGES = ROOT / 'docs' / 'images'


def command(args, log_name):
    log = WORK / log_name
    with log.open('wb') as output:
        result = subprocess.run([str(a) for a in args], cwd=ROOT, stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit(f'Command failed ({result.returncode}); see {log}\n' + log.read_text(encoding='utf-8', errors='replace')[-5000:])


def tool(name):
    found = shutil.which(name)
    if not found and os.name == 'nt':
        # ESP-IDF tools are usable without opening an exported IDF shell.
        base = Path(os.environ.get('IDF_TOOLS_PATH', 'D:/Espressif/tools'))
        if base.name != 'tools':
            base /= 'tools'
        candidates = sorted((base / name).glob(f'*/**/{name}.exe'))
        if candidates:
            found = str(candidates[-1])
    if not found:
        raise SystemExit(f'{name} not found; install it or add it to PATH (see tools/screenshots/README.md).')
    return found


def device_images(lvgl_path):
    from PIL import Image
    if not (lvgl_path / 'CMakeLists.txt').is_file():
        raise SystemExit('LVGL sources missing; run idf.py reconfigure first, or pass --lvgl-path.')
    build = WORK / 'build'
    cmake, ninja = tool('cmake'), tool('ninja')
    command([cmake, '-S', ROOT / 'tools/screenshots', '-B', build, '-G', 'Ninja',
             f'-DCMAKE_MAKE_PROGRAM={ninja}', '-DCMAKE_C_COMPILER=gcc', '-DCMAKE_CXX_COMPILER=g++',
             '-DCMAKE_BUILD_TYPE=Debug', f'-DLVGL_PATH={lvgl_path.as_posix()}'], 'configure.log')
    command([cmake, '--build', build, '--target', 'render', '--parallel', str(min(6, os.cpu_count() or 2))], 'build.log')
    render = build / ('render.exe' if os.name == 'nt' else 'render')
    command([render, WORK], 'render.log')
    for name in ('01_main', '01_main_no_account', '02_wifi', '02_wifi_scanning', '03_pass', '04_acct', '05_add',
                 '08_delete_wifi', '09_delete_account'):
        with Image.open(WORK / f'{name}.ppm') as img:
            assert img.size == (480, 480)
            img.save(IMAGES / f'{name}.png', optimize=True)
        print('Generated', name + '.png', flush=True)


DEMO_STATE = {
    'state': 'ONLINE', 'stext': '外网正常', 'ssid': 'ExampleWiFi', 'target': 'ExampleWiFi',
    'ip': '192.0.2.10', 'account': 'demo-user-01', 'auth_count': 2, 'next_in_ms': 150000,
    'uptime_ms': 4550000, 'detail': '外网正常', 'portal': 'http://portal.example.edu',
    'sd': True, 'ui': 100, 'stage': 7, 'bat_present': True, 'bat_pct': 82,
    'bat_usb': True, 'bat_charging': True, 'bat_min': -1,
    'nets': [{'ssid': 'ExampleWiFi', 'active': True, 'has_pass': True,
              'current': 'demo-user-01', 'accts': [{'user': 'demo-user-01'}, {'user': 'demo-user-02'}]},
             {'ssid': 'LabDemo', 'active': False, 'has_pass': True, 'current': '', 'accts': []}],
}


class DemoHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(ROOT / 'main'), **kwargs)

    def log_message(self, *args):
        pass

    def reply(self, data):
        body = json.dumps(data, ensure_ascii=False).encode('utf-8')
        self.send_response(200)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.split('?')[0] == '/api/state':
            self.reply(DEMO_STATE)
        elif self.path == '/favicon.ico':
            self.send_response(204)
            self.end_headers()
        else:
            super().do_GET()

    def do_POST(self):
        data = parse_qs(self.rfile.read(int(self.headers.get('Content-Length', '0'))).decode())
        if self.path == '/api/auth' and data.get('code') == ['123456']:
            self.reply({'ok': True, 'token': 'demo-session-not-a-real-token'})
        elif self.path == '/api/session':
            valid = data.get('token') == ['demo-session-not-a-real-token']
            self.reply({'ok': valid, 'need_auth': not valid})
        else:
            self.reply({'ok': False, 'msg': '截图演示服务不保存配置'})


def web_images(browser):
    npx = tool('npx.cmd' if os.name == 'nt' else 'npx')
    server = ThreadingHTTPServer(('127.0.0.1', 0), DemoHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    cli = [npx, '--yes', '--package', '@playwright/cli', 'playwright-cli', '-s=netkeeper-docs']
    config = WORK / 'playwright.json'
    config.write_text(json.dumps({'outputDir': str(ROOT / 'output/playwright')}), encoding='utf-8')
    try:
        url = f'http://127.0.0.1:{server.server_port}/index.html'
        command(cli + ['open', url, '--browser', browser, '--config', config], 'web-open.log')
        command(cli + ['snapshot'], 'web-snapshot.log')
        command(cli + ['--raw', 'run-code', '--filename', ROOT / 'tools/screenshots/web.js'], 'web-render.log')
        print('Generated 06_web.png and 07_web_locked.png', flush=True)
    finally:
        command(cli + ['close'], 'web-close.log')
        server.shutdown()
        server.server_close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group()
    group.add_argument('--device-only', action='store_true')
    group.add_argument('--web-only', action='store_true')
    parser.add_argument('--lvgl-path', type=Path, default=ROOT / 'managed_components/lvgl__lvgl')
    parser.add_argument('--browser', default='chrome', help='playwright-cli browser, default: chrome')
    args = parser.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    IMAGES.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    if not args.web_only:
        device_images(args.lvgl_path.resolve())
    if not args.device_only:
        web_images(args.browser)
    print(f'Completed in {time.monotonic()-started:.1f}s. All displayed data is synthetic.')


if __name__ == '__main__':
    main()
