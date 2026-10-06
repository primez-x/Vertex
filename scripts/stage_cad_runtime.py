"""Stage exact manifest-bound CAD bytes and an adapter into a Windows build.

No CAD imports, execution, downloads or qualification occur. An unmarked old
output requires a separately authorized migration. One replacement retains the
verified old output at OUTPUT.rollback. A fixed transaction record bounds
rotation and recovery; unknown objects are preserved. The caller exclusively
owns and freezes the selected inputs and build tree for the whole operation,
serializes build/stage jobs and prevents runtime use during replacement. This
is not a promise of an atomic snapshot against arbitrary concurrent writers.
"""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import stat
import uuid

# Share the composer's bounded Windows namespace and exact receipt validation.
# Loading this inert Python helper does not load CAD or native libraries.
_spec = importlib.util.spec_from_file_location(
    "_stage_receipts", Path(__file__).parent / "qualification/compose_controlled_cad_runtime.py")
_receipts = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_receipts)
MANIFEST = "cad-stage-manifest.json"
ADAPTER = "cad_library_adapter.py"
SOURCE_MANIFESTS = ("runtime-manifest.json", "controlled-runtime-manifest.json")
_KERNEL = None


class _Information(ctypes.Structure):
    _fields_ = [("attributes", wintypes.DWORD), ("creation", wintypes.FILETIME),
                ("access", wintypes.FILETIME), ("write", wintypes.FILETIME),
                ("volume", wintypes.DWORD), ("size_high", wintypes.DWORD),
                ("size_low", wintypes.DWORD), ("links", wintypes.DWORD),
                ("index_high", wintypes.DWORD), ("index_low", wintypes.DWORD)]


class _Unicode(ctypes.Structure):
    _fields_ = [("length", wintypes.USHORT), ("maximum", wintypes.USHORT), ("buffer", wintypes.LPWSTR)]


class _Attributes(ctypes.Structure):
    _fields_ = [("length", wintypes.ULONG), ("root", wintypes.HANDLE),
                ("name", ctypes.POINTER(_Unicode)), ("attributes", wintypes.ULONG),
                ("security", wintypes.LPVOID), ("qos", wintypes.LPVOID)]


class _IoStatus(ctypes.Structure):
    _fields_ = [("status", wintypes.LPVOID), ("information", ctypes.c_size_t)]


def create_handle(parent, name, *, directory):
    """Atomically CREATE_NEW relative to a retained parent directory handle.

    Win32 CreateDirectory followed by CreateFile leaves an adoption race.
    NtCreateFile returns the new identity in the creation operation itself.
    """
    if len(_receipts.relative_path(name).parts) != 1:
        raise ValueError("Creation requires one safe local component")
    api = ctypes.WinDLL("ntdll")
    api.NtCreateFile.argtypes = [ctypes.POINTER(wintypes.HANDLE), wintypes.DWORD,
        ctypes.POINTER(_Attributes), ctypes.POINTER(_IoStatus), wintypes.LPVOID,
        wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD]
    api.NtCreateFile.restype = wintypes.LONG
    api.RtlNtStatusToDosError.argtypes = [wintypes.LONG]
    api.RtlNtStatusToDosError.restype = wintypes.ULONG
    buffer = ctypes.create_unicode_buffer(name)
    length = len(name.encode("utf-16-le"))
    text = _Unicode(length, length + 2, ctypes.cast(buffer, wintypes.LPWSTR))
    attributes = _Attributes(ctypes.sizeof(_Attributes), parent.handle, ctypes.pointer(text), 0x40, None, None)
    status, handle = _IoStatus(), wintypes.HANDLE()
    access = 0x10000 | 0x80 | 0x100000 | (0 if directory else 0xC0000000)
    result = api.NtCreateFile(ctypes.byref(handle), access, ctypes.byref(attributes), ctypes.byref(status),
        None, 0x10 if directory else 0x80, 3 if directory else 5,
        2, 0x200020 | (1 if directory else 0x40), None, 0)
    if result < 0:
        raise ctypes.WinError(api.RtlNtStatusToDosError(result))
    return handle.value


def kernel():
    global _KERNEL
    if os.name != "nt":
        raise ValueError("Safe CAD staging publication requires Windows handle operations")
    if _KERNEL is not None:
        return _KERNEL
    api = ctypes.WinDLL("kernel32", use_last_error=True)
    api.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                               wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    api.CreateFileW.restype = wintypes.HANDLE
    api.CloseHandle.argtypes = [wintypes.HANDLE]
    api.CloseHandle.restype = wintypes.BOOL
    api.GetFileInformationByHandle.argtypes = [wintypes.HANDLE, ctypes.POINTER(_Information)]
    api.GetFileInformationByHandle.restype = wintypes.BOOL
    api.SetFileInformationByHandle.argtypes = [wintypes.HANDLE, ctypes.c_int,
                                             wintypes.LPVOID, wintypes.DWORD]
    api.SetFileInformationByHandle.restype = wintypes.BOOL
    api.SetFilePointerEx.argtypes = [wintypes.HANDLE, ctypes.c_longlong,
                                     ctypes.POINTER(ctypes.c_longlong), wintypes.DWORD]
    api.SetFilePointerEx.restype = wintypes.BOOL
    api.ReadFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                            ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
    api.ReadFile.restype = wintypes.BOOL
    api.WriteFile.argtypes = api.ReadFile.argtypes
    api.WriteFile.restype = wintypes.BOOL
    api.FlushFileBuffers.argtypes = [wintypes.HANDLE]
    api.FlushFileBuffers.restype = wintypes.BOOL
    _KERNEL = api
    return api


class Object:
    """Keep a checked object alive without sharing delete access.

    Root and ancestor handles deny competing rename/delete. Descendants share
    deletion when reopened for verification, while file writes remain blocked.
    Destructive operations address retained handles, never re-resolved names.
    """
    def __init__(self, path, *, directory=False, writable=False, delete=False, share_delete=False, _handle=None, _parent_pinned=False):
        self.path = Path(path)
        self.api = kernel()
        self.handle = None
        access = 0x80 | (0x10000 if delete else 0)
        if not directory:
            access |= 0x80000000 | (0x40000000 if writable else 0)
        # Directories share read/write but NEVER deletion; files share only read.
        sharing = (3 if directory else 1) | (4 if share_delete else 0)
        if _handle is None and not _parent_pinned:
            _receipts.no_links(self.path)
        handle = _handle if _handle is not None else self.api.CreateFileW(
            str(self.path), access, sharing, None, 3, 0x02200000, None)
        if handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        self.handle = handle
        try:
            info = self.info()
            if bool(info.attributes & 0x10) != directory or info.attributes & 0x400:
                raise ValueError("Object is a link/reparse point or has unexpected type")
            if not directory and info.links != 1:
                raise ValueError("Hardlinked file is forbidden")
            self.identity = (info.volume, info.index_high, info.index_low)
        except BaseException:
            self.close()
            raise

    def info(self):
        info = _Information()
        if not self.api.GetFileInformationByHandle(self.handle, ctypes.byref(info)):
            raise ctypes.WinError(ctypes.get_last_error())
        return info

    def receipt(self, *, limit=_receipts.MAX_FILE_BYTES):
        info = self.info()
        if info.links != 1 or info.attributes & (0x10 | 0x400):
            raise ValueError("Linked or nonregular file")
        if ((info.size_high << 32) | info.size_low) > limit:
            raise ValueError("File exceeds byte bound")
        self.seek(0)
        size, digest = 0, hashlib.sha256()
        expected = (info.size_high << 32) | info.size_low
        while size < expected:
            data = self.read(min(1 << 20, expected - size))
            if not data:
                raise ValueError("File shrank during hashing")
            size += len(data)
            if size > limit:
                raise ValueError("File exceeds byte bound")
            digest.update(data)
        if self.read(1):
            raise ValueError("File grew during hashing")
        return {"bytes": size, "sha256": digest.hexdigest()}

    def seek(self, offset):
        if not self.api.SetFilePointerEx(self.handle, offset, None, 0):
            raise ctypes.WinError(ctypes.get_last_error())

    def read(self, count):
        buffer, size = ctypes.create_string_buffer(count), wintypes.DWORD()
        if not self.api.ReadFile(self.handle, buffer, count, ctypes.byref(size), None):
            raise ctypes.WinError(ctypes.get_last_error())
        return ctypes.string_at(buffer, size.value)

    def write(self, data):
        size = wintypes.DWORD()
        if not self.api.WriteFile(self.handle, data, len(data), ctypes.byref(size), None):
            raise ctypes.WinError(ctypes.get_last_error())
        if size.value != len(data):
            raise OSError("Short write while staging")

    def flush(self):
        if not self.api.FlushFileBuffers(self.handle):
            raise ctypes.WinError(ctypes.get_last_error())

    def delete(self):
        flag = wintypes.BOOL(True)
        if not self.api.SetFileInformationByHandle(self.handle, 4, ctypes.byref(flag), ctypes.sizeof(flag)):
            raise ctypes.WinError(ctypes.get_last_error())

    def matches_path(self):
        # Probe the current name with metadata access; retain the original handle
        # even when a descendant was renamed. Cleanup never deletes this probe.
        handle = self.api.CreateFileW(str(self.path), 0x80, 7, None, 3, 0x02200000, None)
        if handle == ctypes.c_void_p(-1).value:
            return False
        try:
            info = _Information()
            if not self.api.GetFileInformationByHandle(handle, ctypes.byref(info)):
                raise ctypes.WinError(ctypes.get_last_error())
            return (not info.attributes & 0x400 and
                    (info.volume, info.index_high, info.index_low) == self.identity)
        finally:
            self.api.CloseHandle(handle)

    def close(self):
        if self.handle is not None:
            self.api.CloseHandle(self.handle)
            self.handle = None


class Directory(Object):
    def __init__(self, path, *, delete=False, share_delete=False, _handle=None, _parent_pinned=False):
        super().__init__(path, directory=True, delete=delete, share_delete=share_delete,
                         _handle=_handle, _parent_pinned=_parent_pinned)

    def rename(self, destination):
        destination = Path(destination)
        _receipts.no_links(destination)
        name = str(destination)
        name_bytes = len(name.encode("utf-16-le"))
        class Rename(ctypes.Structure):
            _fields_ = [("replace", wintypes.BOOL), ("root", wintypes.HANDLE),
                        ("length", wintypes.DWORD), ("name", wintypes.WCHAR * (name_bytes // 2 + 1))]
        request = Rename(False, None, name_bytes, name)
        if not self.api.SetFileInformationByHandle(self.handle, 3, ctypes.byref(request), ctypes.sizeof(request)):
            raise ctypes.WinError(ctypes.get_last_error())
        self.path = destination


class Tree:
    """Lock every directory and file in a verified or newly created tree."""
    def __init__(self, path, *, owned=False, created=False, _handle=None):
        self.path = path
        self.owned = owned
        self.dirs = {"": Directory(path, delete=owned, _handle=_handle)}
        self.files = {}
        self.created_dirs = {""} if created else None
        self.directory_ids = {}
        self.file_ids = {}
        self.table = None

    def close(self):
        for item in reversed(list(self.files.values())):
            item.close()
        for item in reversed(list(self.dirs.values())):
            item.close()

    def parents(self, name):
        parts = _receipts.relative_path(name).parts
        for index in range(1, len(parts)):
            parent = "/".join(parts[:index])
            if parent not in self.dirs:
                path = self.path / parent
                ancestor = "/".join(parts[:index - 1])
                handle = create_handle(self.dirs[ancestor], parts[index - 1], directory=True)
                self.dirs[parent] = Directory(path, delete=True, share_delete=True, _handle=handle)
                self.created_dirs.add(parent)

    def inventory(self):
        actual_files, actual_dirs, count = set(), set(), 0
        todo = [""]
        while todo:
            parent = todo.pop()
            with os.scandir(self.path / parent) as entries:
                for entry in entries:
                    count += 1
                    if count > _receipts.MAX_ENTRIES:
                        raise ValueError("Tree exceeds entry bound")
                    name = (parent + "/" if parent else "") + entry.name
                    _receipts.relative_path(name)
                    info = entry.stat(follow_symlinks=False)
                    if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
                        raise ValueError("Tree contains a link or reparse point")
                    if entry.is_dir(follow_symlinks=False):
                        if name not in self.dirs:
                            self.dirs[name] = Directory(entry.path, delete=self.owned, _parent_pinned=True)
                        actual_dirs.add(name)
                        todo.append(name)
                    else:
                        actual_files.add(name)
        return actual_files, actual_dirs

    def verify(self, table):
        table = _receipts.file_table(sorted(
            ({"path": name, **_receipts.checked_record(value)} for name, value in table.items()),
            key=lambda r: r["path"]))
        expected_dirs = {p.as_posix() for name in table for p in _receipts.relative_path(name).parents
                         if p.as_posix() != "."}
        files, directories = self.inventory()
        if files != set(table) or directories != expected_dirs:
            raise ValueError("Tree differs from exact listed files/directories")
        if any(not item.matches_path() for item in self.dirs.values()):
            raise ValueError("Directory identity substituted")
        for name, receipt in table.items():
            if name not in self.files:
                self.files[name] = Object(self.path / name, delete=self.owned, share_delete=True, _parent_pinned=True)
            if not self.files[name].matches_path() or self.files[name].receipt() != _receipts.checked_record(receipt):
                raise ValueError("File bytes differ from receipt: " + name)
            if name in self.file_ids and self.files[name].identity != self.file_ids[name]:
                raise ValueError("File identity substituted")
        # Hashing opens a second membership window: close it with an exact scan
        # and directory/file identity checks AFTER the last hash.
        if self.inventory() != (set(table), expected_dirs) or any(
                not item.matches_path() for item in (*self.dirs.values(), *self.files.values())):
            raise ValueError("Tree changed during final hashing")
        for name, identity in self.directory_ids.items():
            if self.dirs[name].identity != identity:
                raise ValueError("Directory identity substituted")
        self.table = table
        self.directory_ids = {n: item.identity for n, item in self.dirs.items() if n}
        self.file_ids = {n: item.identity for n, item in self.files.items()}

    def rename(self, destination):
        try:
            self.move_root(destination)
        finally:
            self.reopen()

    def move_root(self, destination):
        # Windows refuses to rename a directory with open descendants. Keep the
        # root handle (which prevents source substitution), close descendants,
        # and rename that SAME root identity. Descendant identity snapshots stay
        # intact until the caller reopens and verifies. This separate operation
        # lets restoration put every original root slot back before a persistent
        # descendant reopen failure can interrupt it.
        if not self.directory_ids:
            self.directory_ids = {n: item.identity for n, item in self.dirs.items() if n}
        if not self.file_ids:
            self.file_ids = {n: item.identity for n, item in self.files.items()}
        for item in self.files.values():
            item.close()
        for name, item in self.dirs.items():
            if name:
                item.close()
        self.files = {}
        self.dirs = {"": self.dirs[""]}
        try:
            self.dirs[""].rename(destination)
        finally:
            self.path = self.dirs[""].path

    def reopen(self):
            # Reacquire only recorded identities. Any foreign substitution is
            # preserved and causes a refusal rather than becoming cleanup-owned.
            for name, identity in sorted(self.directory_ids.items(), key=lambda r: len(Path(r[0]).parts)):
                if name in self.dirs:
                    continue
                item = Directory(self.path / name, delete=self.owned, _parent_pinned=True)
                if item.identity != identity:
                    item.close()
                    raise ValueError("Directory substituted during root rename")
                self.dirs[name] = item
            for name, identity in self.file_ids.items():
                if name in self.files:
                    continue
                item = Object(self.path / name, delete=self.owned, share_delete=True, _parent_pinned=True)
                if item.identity != identity:
                    item.close()
                    raise ValueError("File substituted during root rename")
                self.files[name] = item

    def cleanup(self):
        # Never discover files for deletion: only handles created by this call.
        # An added unknown file/directory retains the whole private stage.
        files, dirs = self.inventory()
        expected_dirs = self.created_dirs - {""} if self.created_dirs is not None else set(self.directory_ids)
        if (files != set(self.files) or dirs != expected_dirs or
                any(not item.matches_path() for item in (*self.dirs.values(), *self.files.values()))):
            return False
        for item in self.files.values():
            item.delete()
            item.close()
        for name in sorted(self.dirs, key=lambda p: len(Path(p).parts), reverse=True):
            self.dirs[name].delete()
            self.dirs[name].close()
        return True


def copy_verified(source, target, receipt, *, parent):
    """Create and retain the exact output identity, not just its pathname."""
    # Every source ancestor was pinned by the verified source/adapter tree.
    source_handle = Object(source, _parent_pinned=True)
    target_handle = None
    try:
        if source_handle.receipt() != _receipts.checked_record(receipt):
            raise ValueError("Copy source differs from receipt")
        handle = create_handle(parent, target.name, directory=False)
        target_handle = Object(target, writable=True, delete=True, share_delete=True, _handle=handle)
        source_handle.seek(0)
        remaining = receipt["bytes"]
        while remaining:
            data = source_handle.read(min(1 << 20, remaining))
            if not data:
                raise ValueError("Copy source shrank")
            target_handle.write(data)
            remaining -= len(data)
        target_handle.flush()
        if target_handle.receipt() != _receipts.checked_record(receipt):
            raise ValueError("Staged bytes differ from receipt")
        return target_handle
    except BaseException:
        if target_handle is not None:
            try:
                target_handle.delete()
            finally:
                target_handle.close()
        raise
    finally:
        source_handle.close()


def read_json(path, item=None):
    if item is None:
        return _receipts.read_json(path)
    receipt = item.receipt(limit=_receipts.MAX_JSON_BYTES)
    item.seek(0)
    data = item.read(receipt["bytes"] + 1)
    if {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()} != receipt:
        raise ValueError("Retained JSON bytes changed")
    value = json.loads(data.decode("utf-8-sig"), object_pairs_hook=_receipts.unique_object,
                       parse_constant=lambda value: (_ for _ in ()).throw(ValueError("Nonfinite JSON")))
    if not isinstance(value, dict):
        raise ValueError("Expected an owned JSON object")
    return value, receipt


def validate_owned(value):
    if (set(value) != {"schema_version", "kind", "stage_id", "source", "adapter", "files",
                       "qualification", "runtime_qualified", "production_worker_integrated"} or
            type(value.get("schema_version")) is not int or value["schema_version"] != 1 or
            value.get("kind") != "vertex_cad_runtime_stage" or
            not isinstance(value.get("stage_id"), str) or
            len(value["stage_id"]) != 32 or any(c not in "0123456789abcdef" for c in value["stage_id"]) or
            value.get("qualification") != "incomplete" or value.get("runtime_qualified") is not False or
            value.get("production_worker_integrated") is not False):
        raise ValueError("Invalid owned stage manifest")
    source = value["source"]
    if (not isinstance(source, dict) or set(source) != {"manifest", "bytes", "sha256"} or
            source["manifest"] not in SOURCE_MANIFESTS):
        raise ValueError("Invalid source identity")
    _receipts.checked_record(source, limit=_receipts.MAX_JSON_BYTES)
    adapter = value["adapter"]
    if not isinstance(adapter, dict) or set(adapter) != {"path", "bytes", "sha256"} or adapter["path"] != ADAPTER:
        raise ValueError("Invalid adapter identity")
    table = _receipts.file_table(value["files"])
    if (MANIFEST in table or ADAPTER not in table or source["manifest"] not in table or
            _receipts.checked_record(table[ADAPTER]) != _receipts.checked_record(adapter) or
            _receipts.checked_record(table[source["manifest"]]) != _receipts.checked_record(source)):
        raise ValueError("Stage identities do not bind its exact table")
    return table


def checkpoint(name):
    """Inert failure-injection boundary; the CLI never supplies a callback."""


def owned_tree(path):
    tree = Tree(path, owned=True)
    try:
        if not (path / MANIFEST).exists():
            raise ValueError("migration-required: output/rollback lacks an owned stage manifest")
        value, receipt = read_json(path / MANIFEST)
        table = validate_owned(value)
        tree.verify({**table, MANIFEST: receipt})
        return tree, value, receipt
    except BaseException:
        tree.close()
        raise


def tree_record(tree):
    value, receipt = read_json(tree.path / MANIFEST, tree.files[MANIFEST])
    tree.verify({**validate_owned(value), MANIFEST: receipt})
    return {"root_identity": list(tree.dirs[""].identity), "stage_id": value["stage_id"],
            "manifest_receipt": receipt,
            "files": [{"path": name, **_receipts.checked_record(r), "identity": list(tree.file_ids[name])}
                      for name, r in sorted(tree.table.items())],
            "directories": [{"path": name, "identity": list(identity)}
                            for name, identity in sorted(tree.directory_ids.items())]}


def valid_identity(value):
    if not isinstance(value, list) or len(value) != 3 or any(type(n) is not int or not 0 <= n <= 0xFFFFFFFF for n in value):
        raise ValueError("Invalid transaction object identity")
    return tuple(value)


def validate_record(record):
    if not isinstance(record, dict) or set(record) != {"root_identity", "stage_id", "manifest_receipt", "files", "directories"}:
        raise ValueError("Invalid transaction tree record")
    valid_identity(record["root_identity"])
    if not isinstance(record["stage_id"], str) or len(record["stage_id"]) != 32 or any(
            c not in "0123456789abcdef" for c in record["stage_id"]):
        raise ValueError("Invalid transaction stage identity")
    _receipts.checked_record(record["manifest_receipt"], limit=_receipts.MAX_JSON_BYTES)
    if not isinstance(record["files"], list):
        raise ValueError("Invalid transaction file table")
    for item in record["files"]:
        if not isinstance(item, dict) or set(item) != {"path", "bytes", "sha256", "identity"}:
            raise ValueError("Invalid transaction member")
        valid_identity(item["identity"])
    table = _receipts.file_table([{k: v for k, v in r.items() if k != "identity"} for r in record["files"]])
    if MANIFEST not in table or _receipts.checked_record(table[MANIFEST]) != record["manifest_receipt"]:
        raise ValueError("Transaction lacks exact owned manifest receipt")
    expected_dirs = {p.as_posix() for n in table for p in _receipts.relative_path(n).parents if p.as_posix() != "."}
    if not isinstance(record["directories"], list) or len(record["directories"]) > _receipts.MAX_ENTRIES:
        raise ValueError("Invalid transaction directory table")
    dirs = {}
    for item in record["directories"]:
        if not isinstance(item, dict) or set(item) != {"path", "identity"}:
            raise ValueError("Invalid transaction directory")
        name = _receipts.relative_path(item["path"]).as_posix()
        if name in dirs:
            raise ValueError("Duplicate transaction directory")
        dirs[name] = valid_identity(item["identity"])
    if set(dirs) != expected_dirs:
        raise ValueError("Transaction directory table differs from exact members")
    return table, dirs


class Journal:
    """One CREATE_NEW, bounded, flushed transaction plan plus commit trailer.

    Before the trailer is flushed recovery restores the original two slots.
    Afterward recovery finishes retiring only recorded original identities.
    A torn/unknown trailer is deliberately ambiguous and is preserved.
    """
    def __init__(self, output, parent, value=None):
        self.path = output.with_name(output.name + ".transaction.json")
        self.item = None
        try:
            if value is not None:
                data = (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()
                if len(data) + 10 > _receipts.MAX_JSON_BYTES:
                    raise ValueError("Transaction record exceeds byte bound")
                handle = create_handle(parent, self.path.name, directory=False)
                self.item = Object(self.path, writable=True, delete=True, _handle=handle)
                self.item.write(data)
                self.item.flush()
                self.value, self.committed = value, False
            else:
                self.item = Object(self.path, writable=True, delete=True)
                data = self.item.read(_receipts.MAX_JSON_BYTES + 1)
                if len(data) > _receipts.MAX_JSON_BYTES:
                    raise ValueError("Transaction record exceeds byte bound")
                lines = data.split(b"\n")
                if len(lines) not in (2, 3) or lines[-1] != b"" or (len(lines) == 3 and lines[1] != b"committed"):
                    raise ValueError("Ambiguous transaction trailer; preserve and inspect")
                self.value = json.loads(lines[0], object_pairs_hook=_receipts.unique_object,
                    parse_constant=lambda value: (_ for _ in ()).throw(ValueError("Nonfinite transaction JSON")))
                self.committed = len(lines) == 3
            self.validate(output, parent)
        except BaseException:
            self.close()
            raise

    def validate(self, output, parent):
        value = self.value
        if (not isinstance(value, dict) or set(value) != {"schema_version", "kind", "transaction_id", "parent_identity", "names", "trees"} or
                type(value["schema_version"]) is not int or value["schema_version"] != 1 or
                value["kind"] != "vertex_cad_stage_transaction" or valid_identity(value["parent_identity"]) != parent.identity):
            raise ValueError("Unowned or changed transaction parent")
        identity = value["transaction_id"]
        if not isinstance(identity, str) or len(identity) != 32 or any(c not in "0123456789abcdef" for c in identity):
            raise ValueError("Invalid transaction identity")
        names = value["names"]
        expected = {"output": output.name, "rollback": output.name + ".rollback",
                    "stage": ".cad-stage-" + identity, "retired": ".cad-retired-" + identity}
        if names != expected or not isinstance(value["trees"], dict) or set(value["trees"]) != {"current", "rollback", "new"}:
            raise ValueError("Transaction names are outside the exact publication plan")
        for name in expected.values():
            if len(_receipts.relative_path(name).parts) != 1:
                raise ValueError("Unsafe transaction sibling")
        identities = set()
        for role, record in value["trees"].items():
            if record is None and role != "new":
                continue
            validate_record(record)
            identity = valid_identity(record["root_identity"])
            if identity in identities:
                raise ValueError("Transaction roots collide")
            identities.add(identity)
        if value["trees"]["current"] is None and value["trees"]["rollback"] is not None:
            raise ValueError("Rollback cannot precede an initial publication")

    def commit(self):
        self.item.seek(self.item.receipt(limit=_receipts.MAX_JSON_BYTES)["bytes"])
        self.item.write(b"committed\n")
        self.item.flush()
        self.committed = True

    def delete(self):
        if not self.item.matches_path():
            raise ValueError("Transaction pathname was substituted")
        self.item.delete()
        self.item.close()

    def close(self):
        if self.item:
            self.item.close()


def recorded_tree(path, record, *, partial=False):
    table, dirs = validate_record(record)
    tree = Tree(path, owned=True)
    try:
        if tree.dirs[""].identity != valid_identity(record["root_identity"]):
            raise ValueError("Recorded root identity substituted")
        tree.directory_ids = dirs
        tree.file_ids = {r["path"]: valid_identity(r["identity"]) for r in record["files"]}
        if not partial:
            tree.verify(table)
            value, receipt = read_json(path / MANIFEST, tree.files[MANIFEST])
            if value.get("stage_id") != record["stage_id"] or receipt != record["manifest_receipt"]:
                raise ValueError("Recorded owned manifest differs")
            owned = {**validate_owned(value), MANIFEST: receipt}
            if ({name: _receipts.checked_record(r) for name, r in owned.items()} !=
                    {name: _receipts.checked_record(r) for name, r in table.items()}):
                raise ValueError("Recorded owned manifest receipts differ")
        else:
            files, directories = tree.inventory()
            if not files <= table.keys() or not directories <= dirs.keys():
                raise ValueError("Retired tree contains unknown members")
            for name in directories:
                if tree.dirs[name].identity != dirs[name]:
                    raise ValueError("Retired directory substituted")
            for name in files:
                item = Object(path / name, delete=True, share_delete=True, _parent_pinned=True)
                tree.files[name] = item
                if item.identity != tree.file_ids[name] or item.receipt() != _receipts.checked_record(table[name]):
                    raise ValueError("Retired member substituted or modified")
            if tree.inventory() != (files, directories) or any(not item.matches_path() for item in (*tree.dirs.values(), *tree.files.values())):
                raise ValueError("Retired tree changed during hashing")
            tree.created_dirs = directories | {""}
            tree.table = table
            tree.partial = True
        return tree
    except BaseException:
        tree.close()
        raise


def restore_transaction(journal, trees, output):
    names = {role: output.parent / name for role, name in journal.value["names"].items()}
    current, previous, new = (trees.get(role) for role in ("current", "rollback", "new"))
    # Restore all root names through retained identities before reopening any
    # descendants. A failure reopening the new private tree must not leave the
    # known original output/rollback in transaction-only slots.
    if new and new.path == names["output"]:
        new.move_root(names["stage"])
    if current and current.path == names["rollback"]:
        current.move_root(names["output"])
    if previous and previous.path == names["retired"]:
        previous.move_root(names["rollback"])
    for role in ("current", "rollback"):
        if trees.get(role):
            trees[role].reopen()
            trees[role].verify(trees[role].table)
    if new:
        if not getattr(new, "partial", False):
            new.reopen()
            new.verify(new.table)
        if not new.cleanup():
            raise ValueError("New private tree contains foreign members; transaction preserved")
    journal.delete()


def recover_transaction(output, parent):
    path = output.with_name(output.name + ".transaction.json")
    _receipts.no_links(path)
    if not path.exists():
        return
    journal, trees = Journal(output, parent), {}
    try:
        # Enumerate only the four names in the immutable plan; never use globs.
        records = journal.value["trees"]
        for slot, name in journal.value["names"].items():
            candidate = output.parent / name
            _receipts.no_links(candidate)
            if not candidate.exists():
                continue
            probe = Directory(candidate)
            try:
                roles = [role for role, record in records.items() if record and valid_identity(record["root_identity"]) == probe.identity]
            finally:
                probe.close()
            if len(roles) != 1 or roles[0] in trees:
                raise ValueError("Unknown/ambiguous transaction object; all trees preserved")
            role = roles[0]
            allowed = {"current": {"output", "rollback"}, "rollback": {"rollback", "retired"}, "new": {"stage", "output"}}
            if slot not in allowed[role]:
                raise ValueError("Recorded root is in an unexpected transaction slot")
            trees[role] = recorded_tree(candidate, records[role], partial=(journal.committed and role == "rollback") or
                                       (not journal.committed and role == "new" and slot == "stage"))
        for role in ("current",):
            if records[role] and role not in trees:
                raise ValueError("Recorded transaction root missing; preserve and inspect")
        if journal.committed:
            if "new" not in trees:
                raise ValueError("Committed new output is missing")
            if trees["new"].path != output or (trees.get("current") and trees["current"].path.name != journal.value["names"]["rollback"]):
                raise ValueError("Committed transaction slots are ambiguous")
            previous = trees.get("rollback")
            if previous and (previous.path.name != journal.value["names"]["retired"] or not previous.cleanup()):
                raise ValueError("Retired original tree is not safe to remove")
            journal.delete()
        else:
            if records["rollback"] and "rollback" not in trees:
                raise ValueError("Original rollback missing; preserve and inspect")
            if "new" not in trees and (any(trees[role].path.name != journal.value["names"][slot]
                    for role, slot in (("current", "output"), ("rollback", "rollback")) if role in trees)):
                raise ValueError("Missing new tree before original slots restored")
            restore_transaction(journal, trees, output)
    finally:
        for tree in trees.values():
            tree.close()
        journal.close()


def publish_transaction(output, parent, current, previous, new, transaction_id):
    names = {"output": output.name, "rollback": output.name + ".rollback",
             "stage": ".cad-stage-" + transaction_id, "retired": ".cad-retired-" + transaction_id}
    trees = {"current": current, "rollback": previous, "new": new}
    value = {"schema_version": 1, "kind": "vertex_cad_stage_transaction", "transaction_id": transaction_id,
             "parent_identity": list(parent.identity), "names": names,
             "trees": {role: tree_record(tree) if tree else None for role, tree in trees.items()}}
    journal = Journal(output, parent, value)
    try:
        try:
            checkpoint("prepared")
            if previous:
                previous.rename(output.parent / names["retired"])
            checkpoint("rollback-retired")
            if current:
                current.rename(output.parent / names["rollback"])
            checkpoint("current-rollback")
            new.rename(output)
            checkpoint("new-output")
            for tree in trees.values():
                if tree:
                    tree.verify(tree.table)
            journal.commit()
            checkpoint("committed")
        except Exception:
            if not journal.committed:
                restore_transaction(journal, trees, output)
            raise
        if previous:
            previous.verify(previous.table)
            if not previous.cleanup():
                raise ValueError("Retired rollback contains foreign members; transaction preserved")
        checkpoint("retired-removed")
        journal.delete()
    finally:
        journal.close()


def stage(runtime_root, adapter, output):
    kernel()  # Fail closed before any filesystem mutation on other platforms.
    def local(value):
        raw = Path(value).absolute()
        resolved = _receipts.local_path(raw)
        if os.path.normcase(str(raw)) != os.path.normcase(str(resolved)):
            raise ValueError("Local path changed during resolution or uses an alias")
        return raw
    runtime_root, adapter, output = (local(value) for value in (runtime_root, adapter, output))
    rollback = output.with_name(output.name + ".rollback")
    if (runtime_root == output or runtime_root.is_relative_to(output) or output.is_relative_to(runtime_root) or
            adapter == output or adapter.is_relative_to(output) or adapter.is_relative_to(rollback) or
            runtime_root == rollback or runtime_root.is_relative_to(rollback) or rollback.is_relative_to(runtime_root)):
        raise ValueError("Runtime, adapter and output/rollback must not overlap")
    if not output.parent.is_dir():
        raise ValueError("Output parent must already exist")
    ancestors, source_tree, old_tree, rollback_tree, private, adapter_handle = [], None, None, None, None, None
    published = False
    try:
        # Pin local ancestry before following any tree components. These handles
        # prevent parent-directory substitutions for the complete transaction.
        paths = set(runtime_root.parents) | set(adapter.parents) | set(output.parents)
        for path in sorted(paths, key=lambda p: len(p.parts)):
            ancestors.append(Directory(path))
        parent_handle = next(item for item in ancestors if item.path == output.parent)
        recover_transaction(output, parent_handle)
        source_tree = Tree(runtime_root)
        present = [name for name in SOURCE_MANIFESTS if (runtime_root / name).exists()]
        if len(present) != 1:
            raise ValueError("Exactly one source runtime manifest is required")
        source_name = present[0]
        source_value, source_receipt = read_json(runtime_root / source_name)
        if (type(source_value.get("schema_version")) is not int or source_value["schema_version"] != 1 or
                source_value.get("qualification") != "incomplete" or
                source_value.get("production_worker_integrated") is not False):
            raise ValueError("Unsupported or qualified source runtime manifest")
        controlled = source_name == SOURCE_MANIFESTS[1]
        if controlled and (source_value.get("kind") != "controlled_cad_runtime" or
                           source_value.get("platform") != "win_amd64"):
            raise ValueError("Unsupported controlled source runtime")
        source_table = _receipts.file_table(source_value["files"], extra_keys={"origin"} if controlled else None)
        if any(name.casefold() in {m.casefold() for m in SOURCE_MANIFESTS} for name in source_table):
            raise ValueError("Source manifest cannot list itself or another runtime manifest")
        if controlled and (any(source_value.get(flag, False) is not False for flag in _receipts.RESULT_FLAGS) or
                any(not isinstance(r["origin"], str) or not 0 < len(r["origin"]) <= 128
                    for r in source_table.values())):
            raise ValueError("Controlled runtime must retain inert origins and unqualified flags")
        exact_source = {**source_table, source_name: source_receipt}
        source_tree.verify(exact_source)
        adapter_handle = Object(adapter)
        adapter_receipt = adapter_handle.receipt()
        result_table = {name: _receipts.checked_record(r) for name, r in exact_source.items()}
        if ADAPTER in result_table or MANIFEST in result_table:
            raise ValueError("Reserved stage namespace collision")
        result_table[ADAPTER] = adapter_receipt
        # Include the reserved manifest name in namespace collision validation.
        _receipts.file_table(sorted(({"path": n, **r} for n, r in
            {**result_table, MANIFEST: {"bytes": 0, "sha256": "0" * 64}}.items()), key=lambda r: r["path"]))
        identity = {"source": {"manifest": source_name, **source_receipt},
                    "adapter": {"path": ADAPTER, **adapter_receipt}}
        if output.exists():
            old_tree = Tree(output, owned=True)
            if not (output / MANIFEST).exists():
                raise ValueError("migration-required: existing output has no owned stage manifest")
            old_value, old_receipt = read_json(output / MANIFEST)
            old_table = validate_owned(old_value)
            old_tree.verify({**old_table, MANIFEST: old_receipt})
            if (old_value["source"] == identity["source"] and old_value["adapter"] == identity["adapter"] and
                    old_table == _receipts.file_table(sorted(
                        ({"path": n, **r} for n, r in result_table.items()), key=lambda r: r["path"]))):
                source_tree.verify(exact_source)
                old_tree.verify({**old_table, MANIFEST: old_receipt})
                return {"status": "unchanged", "stage_id": old_value["stage_id"],
                        "runtime_qualified": False, "production_worker_integrated": False}
            _receipts.no_links(rollback)
            if rollback.exists():
                try:
                    rollback_tree, _, _ = owned_tree(rollback)
                except (ValueError, OSError) as error:
                    raise ValueError("Unowned or modified rollback; both slots preserved") from error
        else:
            _receipts.no_links(rollback)
            if rollback.exists():
                raise ValueError("Occupied rollback slot with missing output; explicit recovery required")
        transaction_id = uuid.uuid4().hex
        private_path = output.parent / (".cad-stage-" + transaction_id)
        private_handle = create_handle(parent_handle, private_path.name, directory=True)
        private = Tree(private_path, owned=True, created=True, _handle=private_handle)
        value = {"schema_version": 1, "kind": "vertex_cad_runtime_stage", "stage_id": uuid.uuid4().hex,
                 **identity, "qualification": "incomplete", "runtime_qualified": False,
                 "production_worker_integrated": False,
                 "files": sorted(({"path": n, **r} for n, r in result_table.items()), key=lambda r: r["path"])}
        for name, receipt in sorted(result_table.items()):
            if len(str(private.path / name)) > 240:
                raise ValueError("Staged path exceeds Windows path bound")
            private.parents(name)
            parent_name = _receipts.relative_path(name).parent.as_posix()
            private.files[name] = copy_verified(adapter if name == ADAPTER else runtime_root / name,
                private.path / name, receipt, parent=private.dirs["" if parent_name == "." else parent_name])
        manifest_data = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")
        if len(manifest_data) > _receipts.MAX_JSON_BYTES:
            raise ValueError("Stage manifest exceeds byte bound")
        handle = create_handle(private.dirs[""], MANIFEST, directory=False)
        manifest_handle = Object(private.path / MANIFEST, writable=True, delete=True,
                                 share_delete=True, _handle=handle)
        private.files[MANIFEST] = manifest_handle
        manifest_handle.write(manifest_data)
        manifest_handle.flush()
        manifest_receipt = {"bytes": len(manifest_data), "sha256": hashlib.sha256(manifest_data).hexdigest()}
        exact_result = {**result_table, MANIFEST: manifest_receipt}
        source_tree.verify(exact_source)
        if adapter_handle.receipt() != adapter_receipt:
            raise ValueError("Adapter changed before publication")
        private.verify(exact_result)
        publish_transaction(output, parent_handle, old_tree, rollback_tree, private, transaction_id)
        published = True
        return {"status": "replaced" if old_tree else "published", "stage_id": value["stage_id"],
                "runtime_qualified": False, "production_worker_integrated": False}
    finally:
        try:
            journal_path = output.with_name(output.name + ".transaction.json")
            if private and not published and private.dirs[""].handle is not None and not journal_path.exists():
                private.cleanup()
        finally:
            for tree in (private, old_tree, rollback_tree, source_tree):
                if tree:
                    tree.close()
            if adapter_handle:
                adapter_handle.close()
            for item in reversed(ancestors):
                item.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runtime-root", "adapter", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    try:
        result = stage(args.runtime_root, args.adapter, args.output)
    except (ValueError, OSError, KeyError, TypeError, IndexError, UnicodeError, RecursionError) as error:
        parser.exit(1, "CAD runtime staging refused: " + str(error) + "\n")
    print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()
