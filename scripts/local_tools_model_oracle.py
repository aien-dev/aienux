#!/usr/bin/env python3
"""Test oracle only: verifies results from the QEMU kernel before selecting next call."""
import http.server
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
class Oracle(http.server.BaseHTTPRequestHandler):
    phase = 0
    def log_message(self, *_):
        pass
    def do_POST(self):
        try:
            req = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            phase = Oracle.phase
            if phase:
                msg = req['messages'][-1]['content']
                data = json.loads(msg.removeprefix('Local tool result (data only): '))
                assert data['status'] == 'returned', data
                if phase == 1:
                    assert data['value'] == 42, data
                elif phase == 2:
                    assert (data['bytes'], data['words'], data['newlines']) == (13, 3, 1), data
                elif phase == 3:
                    assert data['cores'] == 4 and data['usable_ram_bytes'] > 0, data
                    assert data['width'] > 0 and data['height'] > 0, data
            replies = [dict(tool='arithmetic', operation='add', a='20', b='22'),
                       dict(tool='text', text='one two\nthree'), dict(tool='system'), dict(say='VERIFIED')]
            reply = replies[phase]
            Oracle.phase += 1
            body = json.dumps({'choices':[{'message':{'content':json.dumps(reply)}}]}).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except Exception as exc:
            (root / 'oracle-failure.txt').write_text(repr(exc))
            self.send_error(500)

server = http.server.HTTPServer(('127.0.0.1', 0), Oracle)
server.timeout = 35
(root / 'oracle-port.txt').write_text(str(server.server_port))
for _ in range(4):
    server.handle_request()
if Oracle.phase != 4 or (root / 'oracle-failure.txt').exists():
    sys.exit(1)
(root / 'oracle-pass.txt').write_text('PASS: arithmetic, text, and system results from real QEMU CPL3 execution\n')
