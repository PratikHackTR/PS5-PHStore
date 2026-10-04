"""Local preview.json -> parallel image-only downloads. No catalog network fetch."""
from __future__ import annotations

import argparse
import csv
import json
import logging
import os
from pathlib import Path
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from urllib.parse import urlsplit

try:
    import requests
except ImportError:
    raise SystemExit('requests eksik. Python ile: python -m pip install requests')

MAX_IMAGE = 8 * 1024 * 1024
EXTENSIONS = ('.jpg', '.png', '.webp', '.gif')
STOP = threading.Event()
LOCAL = threading.local()
SESSIONS = []
SESSION_LOCK = threading.Lock()


def image_key(url):
    a, b = 14695981039346656037, 7809847782465536322
    mask = (1 << 64) - 1
    for c in url.encode('utf-8'):
        a = ((a ^ c) * 1099511628211) & mask
        b = ((b ^ c) * 14029467366897019727) & mask
    return f'{a:016x}{b:016x}'


def image_extension(path):
    try:
        if not 0 < path.stat().st_size <= MAX_IMAGE:
            return None
        with path.open('rb') as f:
            h = f.read(32)
    except OSError:
        return None
    if len(h) >= 24 and h.startswith(b'\x89PNG\r\n\x1a\n'):
        return '.png'
    if len(h) >= 4 and h.startswith(b'\xff\xd8\xff'):
        return '.jpg'
    if len(h) >= 13 and h[:6] in (b'GIF87a', b'GIF89a'):
        return '.gif'
    if len(h) >= 16 and h[:4] == b'RIFF' and h[8:12] == b'WEBP':
        return '.webp'
    return None


def session():
    if not hasattr(LOCAL, 'session'):
        s = requests.Session()
        s.headers.update({'User-Agent': 'PHStore-PC-Covers/2.0', 'Accept-Encoding': 'identity'})
        s.max_redirects = 6
        adapter = requests.adapters.HTTPAdapter(pool_connections=4, pool_maxsize=1, max_retries=0)
        s.mount('https://', adapter)
        s.mount('http://', adapter)
        LOCAL.session = s
        with SESSION_LOCK:
            SESSIONS.append(s)
    return LOCAL.session


def download(item, destination):
    url, title = item
    key = image_key(url)
    host = urlsplit(url).hostname or ''
    row = dict(Oyun=title, Anahtar=key, Sunucu=host, Dosya='', Durum='error', Bayt=0, HTTP=0, Hata='')
    for ext in EXTENSIONS:
        path = destination / (key + ext)
        if image_extension(path) == ext:
            row.update(Dosya=path.name, Durum='existing', Bayt=path.stat().st_size)
            return row
    temporary = destination / (key + '.part')
    for attempt in range(3):
        if STOP.is_set():
            row.update(Durum='canceled', Hata='Durduruldu')
            return row
        retry = False
        delay = 1 + attempt
        try:
            deadline = time.monotonic() + 60
            with session().get(url, stream=True, timeout=(10, 30)) as response:
                row['HTTP'] = response.status_code
                if response.status_code in (408, 429, 500, 502, 503, 504):
                    retry = True
                    value = response.headers.get('Retry-After', '')
                    if value.isdigit():
                        delay = min(15, max(delay, int(value)))
                    raise ValueError(f'HTTP {response.status_code}')
                if response.status_code != 200:
                    raise ValueError(f'HTTP {response.status_code}')
                length = response.headers.get('Content-Length')
                expected = int(length) if length and length.isdigit() else None
                if expected is not None and not 0 < expected <= MAX_IMAGE:
                    raise ValueError('Content-Length bos veya 8 MiB uzerinde')
                count = 0
                with temporary.open('wb') as output:
                    for chunk in response.iter_content(64 * 1024):
                        if STOP.is_set():
                            raise ValueError('Durduruldu')
                        if time.monotonic() > deadline:
                            retry = True
                            raise ValueError('Gorsel zaman asimi')
                        count += len(chunk)
                        if count > MAX_IMAGE:
                            raise ValueError('Gorsel 8 MiB uzerinde')
                        output.write(chunk)
                # Requests decodes Content-Encoding, so compare lengths only
                # when the response actually is identity encoded.
                encoding = response.headers.get('Content-Encoding', 'identity').lower()
                if encoding == 'identity' and expected is not None and count != expected:
                    retry = True
                    raise ValueError('Eksik govde')
                ext = image_extension(temporary)
                if not ext:
                    raise ValueError('Gecerli PNG/JPEG/GIF/WebP degil')
                target = destination / (key + ext)
                os.replace(temporary, target)
                row.update(Dosya=target.name, Durum='downloaded', Bayt=count, Hata='')
                return row
        except requests.RequestException as exc:
            # Exception text may include URL query tokens; keep class only.
            row['Hata'] = type(exc).__name__
            retry = not isinstance(exc, (requests.exceptions.SSLError, requests.exceptions.TooManyRedirects))
        except (OSError, ValueError) as exc:
            row['Hata'] = str(exc)
        finally:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass
        if not retry or attempt == 2 or STOP.wait(delay):
            break
    if STOP.is_set():
        row['Durum'] = 'canceled'
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    home = Path(__file__).resolve().parent
    parser.add_argument('--catalog', type=Path, default=home / 'preview.json')
    parser.add_argument('--workers', type=int, default=16)
    args = parser.parse_args()
    workers = max(1, min(32, args.workers))
    destination = home / 'gorseller'
    log = home / 'kapakindir.log'
    manifest = home / 'kapakindir-listesi.csv'
    logging.basicConfig(level=logging.INFO, format='%(asctime)s %(message)s', handlers=[
        logging.FileHandler(log, encoding='utf-8'), logging.StreamHandler()])
    lock = (home / 'kapakindir.lock').open('a+b')
    try:
        if os.name == 'nt':
            import msvcrt
            lock.seek(0)
            if not lock.read(1):
                lock.write(b'0')
                lock.flush()
            lock.seek(0)
            try:
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            except OSError:
                raise ValueError('Indirici zaten acik. Diger pencereyi kapatin.')
        logging.info('YEREL KATALOG: %s | paralel=%d | TLS dogrulamasi acik', args.catalog, workers)
        data = json.loads(args.catalog.read_text(encoding='utf-8-sig'))
        if not isinstance(data, dict) or not isinstance(data.get('games'), list):
            raise ValueError('JSON kokunde games dizisi bekleniyor')
        items = {}
        invalid = 0
        for game in data['games']:
            url = game.get('cover') if isinstance(game, dict) else None
            if not url:
                continue
            try:
                parsed = urlsplit(url)
                if not isinstance(url, str) or parsed.scheme not in ('https', 'http') or not parsed.hostname or parsed.username:
                    raise ValueError('Gecersiz URL')
            except (TypeError, ValueError):
                invalid += 1
                continue
            items.setdefault(url, str(game.get('title', '')))
        if not items:
            raise ValueError('Katalogda kapak yok')
        destination.mkdir(parents=True, exist_ok=True)
        logging.info('%d benzersiz kapak -> %s', len(items), destination)
        counts = dict(downloaded=0, existing=0, error=0, canceled=0)
        received = 0
        began = time.monotonic()
        pool = ThreadPoolExecutor(max_workers=workers)
        try:
            futures = [pool.submit(download, item, destination) for item in items.items()]
            with manifest.open('w', newline='', encoding='utf-8-sig') as f:
                writer = csv.DictWriter(f, fieldnames=['Oyun','Anahtar','Sunucu','Dosya','Durum','Bayt','HTTP','Hata'])
                writer.writeheader()
                for future in as_completed(futures):
                    row = future.result()
                    counts[row['Durum']] += 1
                    if row['Durum'] == 'downloaded':
                        received += row['Bayt']
                    writer.writerow(row)
                    f.flush()
                    done = sum(counts.values())
                    speed = received / max(0.1, time.monotonic() - began) / 1048576
                    logging.info('[%d/%d] %s key=%s HTTP=%s bytes=%d %s | %.2f MiB/s',
                                 done, len(items), row['Durum'], row['Anahtar'], row['HTTP'], row['Bayt'], row['Hata'], speed)
        except KeyboardInterrupt:
            STOP.set()
            logging.info('Durduruluyor; tamamlanan gorseller korunuyor.')
            return 130
        finally:
            STOP.set()
            pool.shutdown(wait=True, cancel_futures=True)
            for s in SESSIONS:
                s.close()
        logging.info('BITTI indirilen=%d mevcut=%d hata=%d gecersiz_url=%d sure=%.1fs',
                     counts['downloaded'], counts['existing'], counts['error'], invalid, time.monotonic()-began)
        return 1 if counts['error'] or invalid else 0
    finally:
        lock.close()


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as exc:
        logging.error('DURDU: %s', exc)
        raise SystemExit(1)
