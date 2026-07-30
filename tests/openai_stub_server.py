#!/usr/bin/env python3
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    def _json(self, status, body):
        raw = json.dumps(body).encode("utf-8")
        self.send_response(status)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        if self.path == "/v1/models":
            self._json(200, {"data": [{"id": "gpt-5"}]})
            return
        self._json(404, {"error": "not found"})

    def do_POST(self):
        if self.path != "/v1/responses":
            self._json(404, {"error": "not found"})
            return
        length = int(self.headers.get("content-length", "0"))
        try:
            request = json.loads(self.rfile.read(length).decode("utf-8"))
        except Exception as exc:
            self._json(400, {"error": {"message": str(exc)}})
            return

        text_format = (request.get("text") or {}).get("format") or {}
        if text_format.get("type") != "json_schema":
            self._json(200, {
                "id": "resp_bad_schema",
                "status": "completed",
                "error": {"message": "expected text.format.json_schema"},
            })
            return

        prompt = request.get("input", "")
        if "MISSING_ACTION_CLASS" in prompt:
            output = {"completed": 1, "failed": 0, "confidence": 0.41}
        elif "INCOMPLETE_RESPONSE" in prompt:
            self._json(200, {"id": "resp_incomplete", "status": "incomplete"})
            return
        else:
            output = {"completed": 1, "failed": 0, "confidence": 0.83, "actionClass": 0}

        self._json(200, {
            "id": "resp_stub",
            "model": request.get("model", "gpt-5"),
            "status": "completed",
            "receivedTextFormat": text_format,
            "output_text": json.dumps(output),
        })

    def log_message(self, fmt, *args):
        return


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 5412
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
