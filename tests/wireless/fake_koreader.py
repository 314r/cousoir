# A reader app that connects to Calibre's wireless device server as KOReader
# does (plugins/calibre.koplugin/wireless.lua and metadata.lua, AGPL v3, the
# KOReader community), for the tests that compare Cousoir's server with
# Calibre's: it answers each request as KOReader answers it, keeps the books
# it receives in an inbox folder with metadata.calibre and driveinfo.calibre
# beside them, and writes every message it got to a transcript. Unlike
# KOReader it keeps the whole of each book's metadata, for the comparison.
#
#   python3 -I tests/wireless/fake_koreader.py --port PORT --inbox DIR --transcript FILE
#
# It tries to connect for --wait seconds, then answers until the server
# ejects it or closes the connection. With --discover, it first finds the
# server's port as KOReader does, by a UDP datagram to the broadcast ports,
# sent to --host. With --password, it answers the server's challenge.

import argparse
import hashlib
import json
import os
import socket
import sys
import time

OPCODES = {
    'OK': 0, 'SET_CALIBRE_DEVICE_INFO': 1, 'SET_CALIBRE_DEVICE_NAME': 2, 'GET_DEVICE_INFORMATION': 3,
    'TOTAL_SPACE': 4, 'FREE_SPACE': 5, 'GET_BOOK_COUNT': 6, 'SEND_BOOKLISTS': 7, 'SEND_BOOK': 8,
    'GET_INITIALIZATION_INFO': 9, 'BOOK_DONE': 11, 'NOOP': 12, 'DELETE_BOOK': 13,
    'GET_BOOK_FILE_SEGMENT': 14, 'GET_BOOK_METADATA': 15, 'SEND_BOOK_METADATA': 16, 'DISPLAY_MESSAGE': 17,
    'CALIBRE_BUSY': 18, 'SET_LIBRARY_INFO': 19, 'ERROR': 20, 'GET_COLLECTIONS': 21, 'UPDATE_COLLECTIONS': 22,
}
NAMES = {v: k for k, v in OPCODES.items()}
BROADCAST_PORTS = [54982, 48123, 39001, 44044, 59678]
# KOReader's extensions.lua, EPUB its default output first.
EXTENSIONS = ['epub', 'fb2', 'mobi', 'azw', 'xps', 'doc', 'docx', 'djv', 'djvu', 'pdf', 'cbz', 'htm', 'html',
              'xhtml', 'pdb', 'prc', 'rtf', 'txt', 'md', 'chm', 'zip']
FREE_SPACE = 1024 * 1024 * 1024
MODEL = 'Kobo_Libra2'
VERSION = 'v2025.10'


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument('--host', default='127.0.0.1')
    p.add_argument('--port', type=int, default=9090)
    p.add_argument('--discover', action='store_true')
    p.add_argument('--inbox', required=True)
    p.add_argument('--transcript', required=True)
    p.add_argument('--password', default='')
    p.add_argument('--wait', type=float, default=60)
    return p.parse_args()


def discover(host, deadline):
    # find_calibre_server(): "hello" to each port, the reply names the port.
    while time.monotonic() < deadline:
        for port in BROADCAST_PORTS:
            udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            udp.settimeout(0.5)
            try:
                udp.sendto(b'hello', (host, port))
                data, _ = udp.recvfrom(1024)
            except OSError:
                continue
            finally:
                udp.close()
            text = data.decode('utf-8')
            prefix = 'calibre wireless device client (on '
            if text.startswith(prefix) and ');' in text:
                ports = text.rpartition(');')[2].split(',')
                if len(ports) == 2 and ports[0].isdigit() and ports[1].isdigit():
                    return int(ports[1]), text
        time.sleep(0.2)
    raise SystemExit('no server answered')


class Client:
    def __init__(self, args):
        self.args = args
        self.inbox = os.path.abspath(args.inbox)
        os.makedirs(self.inbox, exist_ok=True)
        self.metadata_path = os.path.join(self.inbox, 'metadata.calibre')
        self.driveinfo_path = os.path.join(self.inbox, 'driveinfo.calibre')
        self.books = self.load(self.metadata_path, [])
        self.drive = self.load(self.driveinfo_path, {})
        self.transcript = []
        self.buffer = b''
        self.pending = None
        self.ejected = False

    @staticmethod
    def load(path, default):
        try:
            with open(path, encoding='utf-8') as f:
                return json.load(f)
        except (OSError, ValueError):
            return default

    def save(self):
        with open(self.metadata_path, 'w', encoding='utf-8') as f:
            json.dump(self.books, f, indent=2, sort_keys=True, ensure_ascii=False)
        with open(self.driveinfo_path, 'w', encoding='utf-8') as f:
            json.dump(self.drive, f, indent=2, sort_keys=True, ensure_ascii=False)

    def connect(self, port, deadline):
        while True:
            try:
                return socket.create_connection((self.args.host, port), timeout=2)
            except OSError:
                if time.monotonic() > deadline:
                    raise SystemExit(f'could not connect to {self.args.host}:{port}')
                time.sleep(0.1)

    def read(self, count):
        while len(self.buffer) < count:
            data = self.sock.recv(65536)
            if not data:
                raise EOFError
            self.buffer += data
        data, self.buffer = self.buffer[:count], self.buffer[count:]
        return data

    def read_message(self):
        # "34[0, {...}]": the length, then the JSON array.
        while b'[' not in self.buffer:
            data = self.sock.recv(65536)
            if not data:
                raise EOFError
            self.buffer += data
        start = self.buffer.index(b'[')
        length = int(self.buffer[:start])
        self.buffer = self.buffer[start:]
        op, arg = json.loads(self.read(length))
        return NAMES.get(op, str(op)), arg

    def send(self, name, arg):
        text = json.dumps([OPCODES[name], arg]).encode('utf-8')
        self.sock.sendall(str(len(text)).encode('ascii') + text)

    def book_index(self, lpath):
        for i, book in enumerate(self.books):
            if book.get('lpath') == lpath:
                return i
        return None

    @staticmethod
    def safe(lpath, check_extension):
        # isSafeLpath()
        if not isinstance(lpath, str) or not lpath or '\0' in lpath:
            return False
        if lpath.startswith('/') or '\\' in lpath or (len(lpath) > 1 and lpath[1] == ':'):
            return False
        parts = [p for p in lpath.split('/') if p]
        if not parts or '..' in parts:
            return False
        if check_extension:
            ext = parts[-1].rpartition('.')[2].lower() if '.' in parts[-1] else None
            return ext in EXTENSIONS
        return True

    def handle(self, name, arg):
        if name == 'GET_INITIALIZATION_INFO':
            challenge = arg.get('passwordChallenge')
            digest = ''
            if self.args.password and challenge:
                digest = hashlib.sha1((self.args.password + challenge).encode('utf-8')).hexdigest()
            self.send('OK', {
                'appName': 'KOReader', 'acceptedExtensions': EXTENSIONS, 'cacheUsesLpaths': True,
                'canAcceptLibraryInfo': True, 'canDeleteMultipleBooks': True, 'canReceiveBookBinary': True,
                'canSendOkToSendbook': True, 'canStreamBooks': True, 'canStreamMetadata': True,
                'canUseCachedMetadata': True, 'ccVersionNumber': VERSION, 'coverHeight': 240,
                'deviceKind': MODEL, 'deviceName': f'KOReader ({MODEL})',
                'extensionPathLengths': {e: len(e) for e in EXTENSIONS}, 'passwordHash': digest,
                'maxBookContentPacketLen': 4096, 'useUuidFileNames': False, 'versionOK': True,
            })
        elif name == 'GET_DEVICE_INFORMATION':
            info = {'device_name': f'KOReader ({MODEL})'}
            if self.drive.get('device_store_uuid'):
                info['device_store_uuid'] = self.drive['device_store_uuid']
            self.send('OK', {'device_info': info, 'version': VERSION, 'device_version': VERSION})
        elif name == 'SET_CALIBRE_DEVICE_INFO':
            previous = self.drive.get('device_name')
            self.drive = dict(arg)
            if previous:
                self.drive['device_name'] = previous
            self.save()
            self.send('OK', {})
        elif name == 'FREE_SPACE':
            self.send('OK', {'free_space_on_device': FREE_SPACE})
        elif name == 'TOTAL_SPACE':
            self.send('OK', {'total_space_on_device': 2 * FREE_SPACE})
        elif name == 'SET_LIBRARY_INFO':
            self.send('OK', {})
        elif name == 'GET_BOOK_COUNT':
            self.send('OK', {'willStream': True, 'willScan': True, 'count': len(self.books)})
            for i, book in enumerate(self.books, 1):
                self.send('OK', {'priKey': i, 'uuid': book.get('uuid'), 'lpath': book.get('lpath'),
                                 'last_modified': book.get('last_modified')})
        elif name == 'NOOP':
            if arg.get('ejecting'):
                self.send('OK', {})
                self.ejected = True
            elif 'count' in arg:
                self.pending = arg['count']
            elif 'priKey' in arg:
                self.send('OK', self.books[arg['priKey'] - 1])
            else:
                self.send('OK', {})
        elif name == 'SEND_BOOK':
            lpath, length = arg.get('lpath'), int(arg.get('length') or 0)
            ok = self.safe(lpath, True) and isinstance(arg.get('metadata'), dict)
            if not ok:
                self.send('ERROR', {'message': f'Invalid file name {lpath}'})
                return
            self.send('OK', {})
            data = self.read(length)
            self.transcript.append({'op': 'BINARY', 'length': len(data), 'sha1': hashlib.sha1(data).hexdigest()})
            path = os.path.join(self.inbox, lpath)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, 'wb') as f:
                f.write(data)
            book = dict(arg['metadata'])
            book['lpath'] = lpath
            index = self.book_index(lpath)
            if index is None:
                self.books.append(book)
            else:
                self.books[index] = book
            self.save()
        elif name == 'SEND_BOOK_METADATA':
            book = arg.get('data') or {}
            index = self.book_index(book.get('lpath'))
            if index is not None:
                self.books[index] = book
            if arg.get('index', 0) + 1 == arg.get('count'):
                self.save()
        elif name == 'DELETE_BOOK':
            self.send('OK', {})
            for lpath in arg.get('lpaths') or []:
                index = self.book_index(lpath)
                uuid = 'none'
                if index is not None and self.safe(lpath, False):
                    uuid = self.books[index].get('uuid')
                    try:
                        os.remove(os.path.join(self.inbox, lpath))
                    except OSError:
                        pass
                    del self.books[index]
                self.send('OK', {'uuid': uuid})
            self.save()
        elif name == 'GET_BOOK_FILE_SEGMENT':
            path = os.path.join(self.inbox, arg.get('lpath') or '')
            if not self.safe(arg.get('lpath'), False) or not os.path.isfile(path):
                self.send('NOOP', {})
                return
            with open(path, 'rb') as f:
                data = f.read()
            self.send('OK', {'fileLength': len(data)})
            self.sock.sendall(data)
        elif name == 'GET_COLLECTIONS':
            self.send('OK', {'collections': {}})
        elif name == 'UPDATE_COLLECTIONS':
            self.send('OK', {})
        # SEND_BOOKLISTS, DISPLAY_MESSAGE and CALIBRE_BUSY need no answer.

    def run(self):
        deadline = time.monotonic() + self.args.wait
        port = self.args.port
        if self.args.discover:
            port, reply = discover(self.args.host, deadline)
            self.transcript.append({'op': 'DISCOVERED', 'reply': reply})
        self.sock = self.connect(port, deadline)
        self.sock.settimeout(self.args.wait)
        try:
            while not self.ejected:
                name, arg = self.read_message()
                self.transcript.append({'op': name, 'arg': arg})
                self.handle(name, arg)
        except (EOFError, ConnectionError):
            self.transcript.append({'op': 'CLOSED'})
        except TimeoutError:
            self.transcript.append({'op': 'TIMEOUT'})
        finally:
            self.sock.close()
            self.save()
            with open(self.args.transcript, 'w', encoding='utf-8') as f:
                json.dump(self.transcript, f, indent=1, sort_keys=True, ensure_ascii=False)


if __name__ == '__main__':
    Client(parse_args()).run()
