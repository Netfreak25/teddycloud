#!/usr/bin/env python3
"""Run against a native binary in temporary storage, never a user's server.

Usage: TEDDYCLOUD_BINARY=./bin/teddycloud python3 tests/test_plugin_files_api.py
"""

import http.client
import json
import os
import shutil
import socket
import subprocess
import tempfile
import time
import unittest
from pathlib import Path
from urllib.parse import urlencode

API = "/api/plugins/files"
BINARY = Path(os.environ.get("TEDDYCLOUD_BINARY", "bin/teddycloud")).resolve()
STARTUP_TIMEOUT = 45
REQUEST_TIMEOUT = 5


class Server:
    """Own one loopback-only server and its disposable configuration/storage."""

    def __init__(self, plugins="plugins"):
        self.temp = tempfile.TemporaryDirectory(prefix="tc-plugin-api-")
        self.root = Path(self.temp.name)
        self.plugins = self.root / "data/www" / plugins
        if plugins == "absolute":
            self.plugins = self.root / "external-plugins"
            plugins = str(self.plugins)

        # Supply only local data; this HTTP fixture does not need TLS certificates.
        for folder in (self.plugins, self.root / "config", self.root / "data/content/default",
                       self.root / "data/library", self.root / "data/www/web",
                       self.root / "certs/server", self.root / "certs/server_tb2"):
            folder.mkdir(parents=True, exist_ok=True)
        (self.root / "data/www/web/index.html").write_text("test")
        for name in ("tonies", "tonies.custom", "tonieboxes", "tonieboxes.custom"):
            (self.root / "config" / f"{name}.json").write_text("[]")

        # Reserve distinct ephemeral ports until all three have been selected.
        sockets = [socket.socket() for _ in range(3)]
        for sock in sockets:
            sock.bind(("127.0.0.1", 0))
        ports = [sock.getsockname()[1] for sock in sockets]
        for sock in sockets:
            sock.close()
        self.port = ports[0]
        settings = {"core.server.bind_ip": "127.0.0.1", "core.pluginsdir": plugins,
                    "internal.autogen_certs": "false",
                    "core.server.http_port": ports[0], "core.server.https_web_port": ports[1],
                    "core.server.https_api_port": ports[2], "core.tonies_json_auto_update": "false"}

        # Capture diagnostics without risking a full pipe blocking the server.
        self.log = (self.root / "server.log").open("w+")
        self.process = subprocess.Popen(
            [str(BINARY), "-b", str(self.root), "--config-set",
             ",".join(f"{key}={value}" for key, value in settings.items())],
            cwd=self.root, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + STARTUP_TIMEOUT
        while time.monotonic() < deadline and self.process.poll() is None:
            try:
                if self.request("GET", "/api/plugins/get")[0] == 200:
                    return
            except (OSError, http.client.HTTPException):
                pass
            time.sleep(0.1)
        self.log.flush()
        self.log.seek(0)
        diagnostics = self.log.read()
        self.close()
        raise RuntimeError(f"Native server did not start:\n{diagnostics[-12000:]}")

    def request(self, method, path, body=None, headers=None):
        """Return HTTP status and bytes without interpreting errors as success."""
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=REQUEST_TIMEOUT)
        try:
            connection.request(method, path, body=body, headers=headers or {})
            response = connection.getresponse()
            return response.status, response.read()
        finally:
            connection.close()

    def close(self):
        """Stop only the process created by this fixture."""
        self.process.terminate()
        try:
            self.process.wait(timeout=REQUEST_TIMEOUT)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        self.log.flush()
        self.log.seek(0)
        diagnostics = self.log.read()
        self.log.close()
        self.temp.cleanup()
        if "ERROR: AddressSanitizer" in diagnostics or "runtime error:" in diagnostics:
            raise AssertionError(f"Sanitizer diagnostics:\n{diagnostics}")


class PluginFilesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = Server()
        cls.addClassCleanup(cls.server.close)

    def setUp(self):
        self.folder = Path(tempfile.mkdtemp(prefix="case-", dir=self.server.plugins))
        self.path = "/" + self.folder.name
        self.addCleanup(shutil.rmtree, self.folder, ignore_errors=True)

    def request(self, method, operation, body=None, path=None, headers=None):
        uri = API + "/" + operation
        if path is not None:
            uri += "?" + urlencode({"path": path})
        return self.server.request(method, uri, body, headers)

    def upload(self, filename, payload=b"plugin-data", path=None):
        boundary = "plugin-api-test-boundary"
        body = (f'--{boundary}\r\nContent-Disposition: form-data; name="file"; '
                f'filename="{filename}"\r\nContent-Type: application/octet-stream\r\n\r\n').encode()
        body += payload + f"\r\n--{boundary}--\r\n".encode()
        return self.request("POST", "upload", body, path=self.path if path is None else path,
                            headers={"Content-Type": f"multipart/form-data; boundary={boundary}"})

    def test_file_lifecycle(self):
        nested = self.path + "/nested"
        self.assertEqual(self.request("POST", "mkdir", nested)[0], 200)
        payload = bytes(range(256)) * 100
        self.assertEqual(self.upload("test.bin", payload, nested)[0], 200)
        status, body = self.request("GET", "index", path=nested)
        self.assertEqual(status, 200)
        files = json.loads(body)["files"]
        self.assertEqual(len(files), 1)
        self.assertEqual(set(files[0]), {"name", "date", "size", "isDir"})
        self.assertEqual(files[0]["size"], len(payload))
        self.assertFalse(files[0]["isDir"])
        self.assertEqual(self.request("GET", "read", path=nested + "/test.bin"), (200, payload))

        # Move and delete operate on the same files visible in native storage.
        source, target = nested + "/test.bin", nested + "/moved.bin"
        self.assertEqual(self.request("POST", "move", urlencode({"source": source, "target": target}))[0], 200)
        self.assertEqual(self.request("GET", "read", path=source)[0], 404)
        self.assertEqual(self.request("GET", "read", path=target), (200, payload))
        self.assertEqual(self.request("POST", "delete", target)[0], 200)
        self.assertEqual(self.request("POST", "rmdir", nested)[0], 200)

    def test_default_root(self):
        status, body = self.request("GET", "index")
        self.assertEqual(status, 200)
        self.assertNotIn("..", [entry["name"] for entry in json.loads(body)["files"]])
        name = self.folder.name + ".bin"
        self.addCleanup((self.server.plugins / name).unlink, missing_ok=True)
        self.assertEqual(self.upload(name, path="")[0], 200)

    def test_missing_paths_and_root_protection(self):
        for operation in ("read", "index"):
            self.assertEqual(self.request("GET", operation, path=self.path + "/missing")[0], 404)
        for operation in ("delete", "rmdir"):
            self.assertEqual(self.request("POST", operation, self.path + "/missing")[0], 404)
        for operation in ("mkdir", "delete", "rmdir"):
            for path in ("", "/", ".", "/./"):
                with self.subTest(operation=operation, path=path):
                    self.assertEqual(self.request("POST", operation, path)[0], 400)
        self.assertEqual(self.request("GET", "read")[0], 400)

    def test_traversal_and_nul(self):
        for path in ("../outside", "/../outside", self.path + "/../../outside",
                     "..\\outside", "C:/outside", self.path + "\x00/hidden"):
            with self.subTest(path=path):
                self.assertEqual(self.request("GET", "read", path=path)[0], 400)
                self.assertEqual(self.request("POST", "mkdir", path.encode())[0], 400)
        self.assertFalse((self.server.root / "outside").exists())

    def test_query_validation(self):
        for query in ("special=plugins", "overlay=box", "special=", "overlay", "xpath=/",
                      "path=/&path=/", "path=/&special=library", "path=%00", "path=" + "a" * 200):
            with self.subTest(query=query):
                self.assertEqual(self.server.request("GET", API + "/index?" + query)[0], 400)
        self.assertEqual(self.request("POST", "mkdir", self.path + "/x", path="/")[0], 400)

    def test_exact_routes_and_methods(self):
        for operation in ("index", "read", "upload", "mkdir", "move", "delete", "rmdir"):
            method = "POST" if operation in ("index", "read") else "GET"
            self.assertEqual(self.request(method, operation)[0], 405)
            self.assertEqual(self.request("GET", operation + "Extra")[0], 404)
        self.assertEqual(self.request("GET", "unknown")[0], 404)

    def test_upload_filename_validation(self):
        for filename in ("../escape", "dir/file", "dir\\file", ".", "..", "C:escape"):
            with self.subTest(filename=filename):
                self.assertEqual(self.upload(filename)[0], 400)
        self.assertEqual(list(self.folder.iterdir()), [])

    def test_symlink_components(self):
        outside = self.server.root / "outside.bin"
        outside.write_bytes(b"unchanged")
        self.addCleanup(outside.unlink, missing_ok=True)
        (self.folder / "link.bin").symlink_to(outside)
        (self.folder / "dirlink").symlink_to(self.server.root, target_is_directory=True)
        (self.folder / "dangling").symlink_to(self.server.root / "absent")
        for name in ("link.bin", "dirlink/outside.bin", "dangling"):
            self.assertEqual(self.request("GET", "read", path=self.path + "/" + name)[0], 400)
            self.assertEqual(self.request("POST", "delete", self.path + "/" + name)[0], 400)
        self.assertEqual(self.upload("link.bin")[0], 400)
        self.assertEqual(self.upload("dangling")[0], 400)
        self.assertEqual(self.upload("new", path=self.path + "/dirlink")[0], 400)
        self.assertEqual(outside.read_bytes(), b"unchanged")

    def test_move_validation_and_existing_target(self):
        (self.folder / "source").write_bytes(b"source")
        (self.folder / "target").write_bytes(b"target")
        for source, target in (("/", self.path + "/target"), (self.path + "/source", "/"),
                               (self.path + "/source", "../escape")):
            self.assertEqual(self.request("POST", "move", urlencode({"source": source, "target": target}))[0], 400)
        body = urlencode({"source": self.path + "/source", "target": self.path + "/target"})
        self.assertEqual(self.request("POST", "move", body)[0], 500)
        self.assertEqual((self.folder / "source").read_bytes(), b"source")
        self.assertEqual((self.folder / "target").read_bytes(), b"target")
        for extra in ("&source=/", "&target=/", "&overlay=box"):
            self.assertEqual(self.request("POST", "move", body + extra)[0], 400)

    def test_directory_activation_and_recovery(self):
        for name, payload in (("active", b"old"), ("stage", bytes(range(256)))):
            nested = self.folder / name / "nested"
            nested.mkdir(parents=True)
            (nested / "data.bin").write_bytes(payload)

        # Activate staging only after the previous installation has a backup.
        for source, target in (("active", "backup"), ("stage", "active"),
                               ("active", "failed"), ("backup", "active")):
            expected = (self.folder / source / "nested/data.bin").read_bytes()
            body = urlencode({"source": self.path + "/" + source, "target": self.path + "/" + target})
            self.assertEqual(self.request("POST", "move", body), (200, b"OK"))
            self.assertFalse((self.folder / source).exists())
            self.assertEqual((self.folder / target / "nested/data.bin").read_bytes(), expected)
        self.assertEqual((self.folder / "active/nested/data.bin").read_bytes(), b"old")

    def test_move_never_replaces_existing_targets(self):
        (self.folder / "file").write_bytes(b"unchanged")
        (self.folder / "empty").mkdir()
        (self.folder / "directory").mkdir()
        (self.folder / "directory/child").write_bytes(b"nested")

        # Files, empty directories and populated directories all block a move.
        for source in ("file", "directory"):
            for target in ("file", "empty", "directory"):
                with self.subTest(source=source, target=target):
                    body = urlencode({"source": self.path + "/" + source, "target": self.path + "/" + target})
                    self.assertEqual(self.request("POST", "move", body)[0], 500)
                    self.assertEqual((self.folder / "file").read_bytes(), b"unchanged")
                    self.assertEqual((self.folder / "directory/child").read_bytes(), b"nested")
                    self.assertEqual(list((self.folder / "empty").iterdir()), [])

    def test_directory_move_rejects_unsafe_targets(self):
        source = self.folder / "source"
        source.mkdir()
        (source / "child").write_bytes(b"keep")
        (self.folder / "link").symlink_to(source, target_is_directory=True)
        (self.folder / "dangling").symlink_to(self.folder / "missing")

        # Root and symlink targets are rejected before rename; self-descendants fail.
        for target, expected in (("/", 400), (self.path + "/link", 400),
                                 (self.path + "/dangling", 400), (self.path + "/source/sub", 500)):
            body = urlencode({"source": self.path + "/source", "target": target})
            self.assertEqual(self.request("POST", "move", body)[0], expected)
            self.assertEqual((source / "child").read_bytes(), b"keep")
            self.assertFalse((source / "sub").exists())

    def test_filesystem_failures(self):
        self.assertEqual(self.request("POST", "mkdir", self.path)[0], 500)
        (self.folder / "file").write_bytes(b"data")
        self.assertEqual(self.request("GET", "read", path=self.path)[0], 400)
        self.assertEqual(self.request("GET", "index", path=self.path + "/file")[0], 400)
        self.assertEqual(self.upload("wrong-target", path=self.path + "/file")[0], 400)
        self.assertEqual(self.request("POST", "rmdir", self.path)[0], 500)
        self.folder.chmod(0o555)
        try:
            self.assertEqual(self.request("POST", "mkdir", self.path + "/forbidden")[0], 500)
            self.assertEqual(self.upload("forbidden")[0], 500)
        finally:
            self.folder.chmod(0o755)
        (self.folder / "file").chmod(0)
        try:
            self.assertEqual(self.request("GET", "read", path=self.path + "/file")[0], 500)
        finally:
            (self.folder / "file").chmod(0o644)

    def test_plain_read_even_with_gzip_sibling(self):
        (self.folder / "file").write_bytes(b"original")
        (self.folder / "file.gz").write_bytes(b"other")
        self.assertEqual(self.request("GET", "read", path=self.path + "/file",
                                     headers={"Accept-Encoding": "gzip"}), (200, b"original"))

    def test_empty_upload_is_not_success(self):
        self.assertEqual(self.request("POST", "upload", b"--empty--\r\n", path=self.path,
                                     headers={"Content-Type": "multipart/form-data; boundary=empty"})[0], 400)
        self.assertEqual(list(self.folder.iterdir()), [])

    def test_segmented_body(self):
        (self.folder / "file").write_bytes(b"delete-me")
        for operation, name in (("mkdir", "segmented"), ("delete", "file"), ("rmdir", "segmented")):
            body = (self.path + "/" + name).encode()
            with socket.create_connection(("127.0.0.1", self.server.port), REQUEST_TIMEOUT) as sock:
                headers = (f"POST {API}/{operation} HTTP/1.1\r\nHost: localhost\r\n"
                           f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n").encode()
                # Separate TCP writes exercise complete-body handling in all three callers.
                sock.sendall(headers + body[:4])
                time.sleep(0.05)
                sock.sendall(body[4:])
                response = http.client.HTTPResponse(sock)
                response.begin()
                self.assertEqual(response.status, 200)
                response.read()
            self.assertEqual((self.folder / name).exists(), operation == "mkdir")

    def test_invalid_path_bodies_do_not_modify_storage(self):
        (self.folder / "file").write_bytes(b"keep")
        (self.folder / "directory").mkdir()
        for operation, name in (("mkdir", "new"), ("delete", "file"), ("rmdir", "directory")):
            for body in (b"/" + b"x" * 300, (self.path + "/" + name).encode() + b"\x00suffix"):
                with self.subTest(operation=operation, body_length=len(body)):
                    self.assertEqual(self.request("POST", operation, body)[0], 400)
            # A rejected truncated path must not create or remove its valid prefix.
            self.assertFalse((self.folder / "new").exists())
            self.assertEqual((self.folder / "file").read_bytes(), b"keep")
            self.assertTrue((self.folder / "directory").is_dir())

    def test_legacy_storage_is_separate(self):
        for selector, folder in (("", "content/default"), ("?special=library", "library")):
            name = self.folder.name
            uri = "/api/dirCreate" + selector
            self.assertEqual(self.server.request("POST", uri, "/" + name)[0], 200)
            destination = self.server.root / "data" / folder / name
            self.assertTrue(destination.is_dir())

            # The shared upload/move/delete handlers retain their legacy behavior.
            body = (b'--legacy\r\nContent-Disposition: form-data; name="file"; '
                    b'filename="legacy.bin"\r\n\r\noriginal\r\n--legacy--\r\n')
            query = urlencode({"path": "/" + name})
            if selector:
                query += "&" + selector[1:]
            headers = {"Content-Type": "multipart/form-data; boundary=legacy"}
            self.assertEqual(self.server.request("POST", "/api/fileUpload?" + query, body, headers)[0], 200)
            self.assertEqual((destination / "legacy.bin").read_bytes(), b"original")

            # Native bytes and source removal prove the target root stayed unchanged.
            source, target = "/" + name + "/legacy.bin", "/" + name + "/moved.bin"
            body = urlencode({"source": source, "target": target})
            self.assertEqual(self.server.request("POST", "/api/fileMove" + selector, body)[0], 200)
            self.assertEqual((destination / "moved.bin").read_bytes(), b"original")
            self.assertFalse((destination / "legacy.bin").exists())
            self.assertEqual(self.server.request("POST", "/api/fileDelete" + selector, target)[0], 200)
            self.assertEqual(self.server.request("POST", "/api/dirDelete" + selector, "/" + name)[0], 200)
        self.assertTrue(self.folder.is_dir())

    def test_missing_root_never_falls_back(self):
        plugins = self.server.plugins
        hidden = plugins.with_name(plugins.name + "-hidden")
        plugins.rename(hidden)
        try:
            self.assertEqual(self.request("GET", "index")[0], 404)
            self.assertEqual(self.request("POST", "mkdir", "/no-fallback")[0], 404)
            self.assertFalse((self.server.root / "data/content/default/no-fallback").exists())
        finally:
            hidden.rename(plugins)

    def test_alternate_roots(self):
        for plugins in ("nested/addons", "absolute"):
            with self.subTest(plugins=plugins):
                server = Server(plugins)
                try:
                    self.assertEqual(server.request("POST", API + "/mkdir", "/custom")[0], 200)
                    self.assertTrue((server.plugins / "custom").is_dir())
                    (server.plugins / "file").write_bytes(b"custom-root")
                    self.assertEqual(server.request("GET", API + "/read?path=/file"), (200, b"custom-root"))
                    self.assertFalse((server.root / "data/content/default/custom").exists())
                finally:
                    server.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
