"""A stand-in for GitHub's release pages on this PC, for the launcher's tests (nothing leaves the PC).

    python tools/lab/launcher_feed.py <port> <folder> [release | norepo | empty]

The launcher is pointed at it with --repo http://127.0.0.1:<port>/r and asks what it would ask GitHub:

  release  the folder holds metronet-latest.json and metronet-launcher.exe. /r/releases/latest/download/<file>
           and /r/releases/download/v<version>/<file> give the files; /r/releases/latest redirects to
           /r/releases/tag/v<version>, as GitHub does.
  norepo   there is no such repository (or it is not open yet): every address answers 404.
  empty    the repository is there and has no release: the files answer 404, /r/releases/latest leads back to
           /r/releases - the page of an empty list.

Every request is written to requests.log in the folder. The lab (launcher_lab.py) runs it in its own process
through the Feed class.
"""
import http.server
import json
import os
import sys
import threading


def handler(root, mode):
    version = ""
    if mode == "release":
        version = json.load(open(os.path.join(root, "metronet-latest.json"), encoding="utf-8"))["version"]

    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def answer(self, status, body=b"", location=None):
            self.send_response(status)
            if location:
                self.send_header("Location", location)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            with open(os.path.join(root, "requests.log"), "a") as f:
                f.write(self.path + "\n")
            p = self.path.split("?")[0]
            if mode == "norepo":
                return self.answer(404)
            if p == "/r/releases/latest":
                if mode == "empty":
                    return self.answer(302, location="/r/releases")
                return self.answer(302, location="/r/releases/tag/v" + version)
            if p == "/r/releases" or p.startswith("/r/releases/tag/"):
                return self.answer(200, b"<html><body>releases</body></html>")
            if mode == "release":
                for prefix in ("/r/releases/latest/download/", "/r/releases/download/v%s/" % version):
                    if p.startswith(prefix):
                        name = p[len(prefix):]
                        path = os.path.join(root, name)
                        if "/" not in name and "\\" not in name and os.path.isfile(path):
                            return self.answer(200, open(path, "rb").read())
            return self.answer(404)

    return H


class Feed:
    """The server on a port of its own, in a thread of this process."""

    def __init__(self, root, mode="release"):
        os.makedirs(root, exist_ok=True)
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler(root, mode))
        self.url = "http://127.0.0.1:%d/r" % self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def stop(self):
        self.server.shutdown()
        self.server.server_close()


if __name__ == "__main__":
    mode = sys.argv[3] if len(sys.argv) > 3 else "release"
    http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), handler(sys.argv[2], mode)).serve_forever()
