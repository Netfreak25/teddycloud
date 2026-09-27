# Plugin files API

These web-server endpoints expose files beneath the configured
`internal.pluginsdirfull` (`core.pluginsdir`, relative to `core.wwwdir` or absolute).
They use the existing web access controls (`SERTY_WEB`) and do not require
`special=plugins`. Content/library APIs retain their existing behavior.

## Requests

| Method | Endpoint | Input |
| --- | --- | --- |
| GET | `/api/plugins/files/index` | Optional URL-encoded `path` query parameter; defaults to `/`. |
| GET | `/api/plugins/files/read` | Required URL-encoded `path` query parameter. |
| POST | `/api/plugins/files/upload` | Optional URL-encoded `path` identifying an existing directory; defaults to `/`. Existing `multipart/form-data` format with a `file` field and filename. |
| POST | `/api/plugins/files/mkdir` | Raw UTF-8 directory path in the request body. Creates one directory. |
| POST | `/api/plugins/files/move` | Form-encoded `source` and `target` in the request body. Moves a file or directory without overwriting an existing target. |
| POST | `/api/plugins/files/delete` | Raw UTF-8 file path in the request body. |
| POST | `/api/plugins/files/rmdir` | Raw UTF-8 directory path in the request body. Removes an empty directory. |

Paths, including those beginning with `/`, are relative to plugin storage.
Query/form values are decoded once; raw bodies and multipart filenames are not
URL-decoded. Use `Content-Length` for request bodies. Paths must fit the existing
handler buffers; oversized paths are rejected rather than truncated.

Only the listed methods and exact endpoint paths are supported. Query parameters
other than `path` are rejected, including `special` and `overlay`. Body-path and
move operations accept no query parameters. Duplicate path/source/target fields
and embedded NUL bytes are rejected.

## Responses

Successful writes return HTTP 200 and the existing plain-text `OK` response.
Index returns the existing basic file-index shape, without Tonie/audio metadata:

```json
{"files":[{"name":"index.html","date":1700000000,"size":128,"isDir":false}]}
```

Read returns the requested file bytes using the existing file response mechanism;
it does not substitute a neighboring `.gz` file. A successful index request with
the expected JSON shape can be used to detect API support without writing files.

| Status | Meaning |
| --- | --- |
| 400 | Invalid parameters, disallowed paths, or invalid/incomplete upload. |
| 404 | Unknown endpoint or missing plugin storage/path. |
| 405 | Unsupported method for a known endpoint. |
| 500 | File operation failed, including permissions, existing move targets, or nonempty directory removal. |

Paths containing `.`/`..` components, backslashes, drive/stream syntax or control
characters are rejected. Symlinks and Windows reparse points below the configured
root cannot be used as path components. The plugin root cannot be deleted or
moved. The configured root and local filesystem remain administrator-controlled;
these checks do not provide isolation from concurrent local filesystem changes.

Directory moves use a filesystem rename, supporting staging activation, backups
and recovery. Existing file or directory targets are rejected. Rename failures,
including moves across filesystems or into the source directory's own subtree,
return HTTP 500; directories are never moved by recursively copying and deleting.

## Client responsibilities

There is no ZIP installer, external download facility, recursive removal, or
installation transaction in this API. Clients retain responsibility for package
validation, consent, backups, staging, recovery and source policy. Upload failures
may leave a partial file; clients must verify writes and handle staging cleanup.
Existing Plugin Manager versions need an adapter update to use these endpoints.

## Native regression tests

From the repository root, on Linux as a non-root user:

```sh
make build
TEDDYCLOUD_BINARY=./bin/teddycloud python3 tests/test_plugin_files_api.py
```

The suite starts native servers bound to loopback with temporary storage and
disables certificate generation for its HTTP-only fixtures. It tests file
lifecycle, alternative roots, legacy storage separation, route/method validation,
path and symlink rejection, segmented bodies, and filesystem/upload failures.
Default debug builds retain AddressSanitizer and UBSan; their runtime diagnostics
fail the suite. This is not validation of a production installation or Windows
runtime behavior.
