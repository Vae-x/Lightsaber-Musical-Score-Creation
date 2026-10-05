"""Download pinned upstream models into a local cache, checking published hashes."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import time
import urllib.request
from http.client import HTTPException
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = "BierHerr/InfernoSaber"
REVISIONS = {
    "easy_15": "e04b14ad772a4e091a0df65780ddc4011f43409b",
    "expert_15": "0cfd41f330f47eba81106684255a916934da3ae7",
}


def file_hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def emit(**values: object) -> None:
    print(json.dumps(values, ensure_ascii=False), flush=True)


def download(url: str, target: Path, size: int, expected_hash: str) -> None:
    if target.exists():
        if target.stat().st_size == size and file_hash(target) == expected_hash:
            emit(stage="cached", file=target.name, bytes=size)
            return
        raise ValueError(f"Existing model file has an unexpected hash; preserved: {target}")
    partial = target.with_name(target.name + ".part")
    if size >= 16 * 1024 * 1024:
        download_parallel(url, target, partial, size, expected_hash)
        return
    failures = 0
    last_report = time.monotonic()
    while True:
        offset = partial.stat().st_size if partial.exists() else 0
        if offset > size:
            raise ValueError(f"Oversized partial model preserved: {partial}")
        if offset == size:
            if file_hash(partial) != expected_hash:
                raise ValueError(f"Partial model hash mismatch; preserved: {partial}")
            partial.replace(target)
            return
        headers = {"User-Agent": "LMSC-Inferno-Trial"}
        # Bounded ranges survive proxies/CDNs that close large transfers early.
        end = min(offset + 8 * 1024 * 1024 - 1, size - 1)
        before = offset
        headers["Range"] = f"bytes={offset}-{end}"
        # A fresh URL avoids stale signed CDN redirects from intermediary caches.
        request = urllib.request.Request(url + f"?download=true&trial={time.time_ns()}", headers=headers)
        try:
            with urllib.request.urlopen(request, timeout=120) as response:
                status = response.status
                if status == 206:
                    content_range = response.headers.get("Content-Range", "")
                    if content_range != f"bytes {offset}-{end}/{size}":
                        raise ValueError("Unexpected resume range; partial file preserved")
                elif status == 200:
                    offset = 0
                else:
                    raise ValueError(f"Unexpected download status: {status}")
                with partial.open("ab" if offset else "wb") as stream:
                    while True:
                        chunk = response.read(1024 * 1024)
                        if not chunk:
                            break
                        stream.write(chunk)
                        offset += len(chunk)
                        if offset > size:
                            raise ValueError("Download exceeded published model size")
                        if time.monotonic() - last_report >= 10:
                            emit(stage="download", file=target.name, bytes=offset, total=size,
                                 percent=round(offset * 100 / size, 1))
                            last_report = time.monotonic()
            # Even an interrupted successful HTTP response may have made useful
            # progress. The next iteration resumes at the actual local length.
            if offset <= before:
                raise OSError("Download made no progress")
            if offset == size:
                if file_hash(partial) != expected_hash:
                    raise ValueError(f"Downloaded model hash mismatch; preserved: {partial}")
                partial.replace(target)
                emit(stage="verified", file=target.name, bytes=size)
                return
            if not response.headers.get("Content-Range") and status == 200:
                raise OSError("Server ignored the range and closed the download")
            failures = 0
        except (OSError, TimeoutError, HTTPException) as error:
            failures += 1
            emit(stage="retry", file=target.name, attempt=failures, error=str(error))
            if failures >= 8:
                raise


def download_parallel(url: str, target: Path, partial: Path, size: int, expected_hash: str) -> None:
    """Fetch bounded ranges concurrently; merge in order and verify the whole file."""
    offset = partial.stat().st_size if partial.exists() else 0
    if offset > size:
        raise ValueError(f"Oversized partial model preserved: {partial}")
    block = 2 * 1024 * 1024
    ranges = [(start, min(start + block - 1, size - 1)) for start in range(offset, size, block)]

    def fetch(bounds: tuple[int, int]) -> Path:
        start, end = bounds
        chunk_path = partial.with_name(partial.name + f".range-{start}-{end}")
        existing = chunk_path.stat().st_size if chunk_path.exists() else 0
        if existing > end - start + 1:
            raise ValueError(f"Oversized range cache preserved: {chunk_path}")
        failures = 0
        while existing < end - start + 1:
            current = start + existing
            request = urllib.request.Request(url + f"?download=true&trial={time.time_ns()}",
                headers={"User-Agent": "LMSC-Inferno-Trial", "Range": f"bytes={current}-{end}"})
            before = existing
            try:
                with urllib.request.urlopen(request, timeout=120) as response:
                    expected_range = f"bytes {current}-{end}/{size}"
                    if response.status != 206 or response.headers.get("Content-Range") != expected_range:
                        raise ValueError("Server did not return the requested bounded range")
                    with chunk_path.open("ab" if existing else "wb") as stream:
                        while True:
                            data = response.read(min(1024 * 1024, end - start + 1 - existing))
                            if not data:
                                break
                            stream.write(data)
                            existing += len(data)
                            if existing == end - start + 1:
                                break
                if existing == before:
                    raise OSError("Range download made no progress")
                failures = 0
            except (OSError, TimeoutError, HTTPException):
                if existing > before:
                    failures = 0
                else:
                    failures += 1
                if failures >= 8:
                    raise
        return chunk_path

    last_report = time.monotonic()
    with ThreadPoolExecutor(max_workers=8) as pool:
        for bounds, chunk_path in zip(ranges, pool.map(fetch, ranges)):
            if partial.exists() and partial.stat().st_size != bounds[0]:
                raise ValueError("Partial model length changed during download")
            with partial.open("ab") as stream, chunk_path.open("rb") as source:
                for data in iter(lambda: source.read(1024 * 1024), b""):
                    stream.write(data)
            if time.monotonic() - last_report >= 10:
                emit(stage="download", file=target.name, bytes=partial.stat().st_size,
                     total=size, percent=round(partial.stat().st_size * 100 / size, 1))
                last_report = time.monotonic()
    if partial.stat().st_size != size or file_hash(partial) != expected_hash:
        raise ValueError(f"Downloaded model hash mismatch; all partials preserved: {partial}")
    partial.replace(target)
    # Only our fully merged range files are removed; interrupted ones stay resumable.
    for start, end in ranges:
        target.with_name(target.name + f".part.range-{start}-{end}").unlink()
    emit(stage="verified", file=target.name, bytes=size)


def fetch_model(cache: Path, model: str, notes_only: bool = True) -> dict:
    started = time.monotonic()
    revision = REVISIONS[model]
    request = urllib.request.Request(
        f"https://huggingface.co/api/models/{REPO}/revision/{revision}?blobs=true",
        headers={"User-Agent": "LMSC-Inferno-Trial"},
    )
    with urllib.request.urlopen(request, timeout=60) as response:
        metadata = json.load(response)
    if metadata.get("sha") != revision:
        raise ValueError("Model repository revision mismatch")
    records = []
    for item in metadata.get("siblings", []):
        name = item["rfilename"]
        if not name.endswith((".h5", ".pkl")):
            continue
        if Path(name).name != name or "/" in name or "\\" in name:
            raise ValueError("Unexpected model path")
        lfs = item.get("lfs") or {}
        expected_hash = lfs.get("sha256", "")
        if len(expected_hash) != 64 or lfs.get("size") != item.get("size"):
            raise ValueError(f"No verified upstream hash for {name}")
        records.append({"name": name, "size": item["size"], "sha256": expected_hash})
    required = {"notes_class_dict.pkl", "onehot_encoder_beats.pkl", "onehot_encoder_events.pkl"}
    names = {record["name"] for record in records}
    prefixes = ("tf_model_enc_", "tf_model_autoenc_", "tf_model_mapper_", "tf_beat_gen_", "tf_event_gen_")
    if not required.issubset(names) or not all(sum(n.startswith(p) for n in names) == 1 for p in prefixes):
        raise ValueError("Upstream model bundle is incomplete or ambiguous")
    target_dir = cache / model
    target_dir.mkdir(parents=True, exist_ok=True)
    skipped = [record for record in records if notes_only and record["name"].startswith("tf_event_gen_")]
    records = [record for record in records if record not in skipped]
    for record in records:
        download(f"https://huggingface.co/{REPO}/resolve/{revision}/{record['name']}",
                 target_dir / record["name"], record["size"], record["sha256"])
    manifest = {"model": model, "repo": REPO, "revision": revision, "complete": True,
                "componentMode": "notes-only" if notes_only else "full", "skippedFiles": skipped,
                "files": records, "downloadSeconds": round(time.monotonic() - started, 2)}
    temporary = target_dir / ".trial-model-manifest.json.tmp"
    temporary.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(temporary, target_dir / ".trial-model-manifest.json")
    emit(stage="model-ready", model=model, revision=revision,
         totalBytes=sum(record["size"] for record in records))
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--model", action="append", choices=sorted(REVISIONS))
    parser.add_argument("--with-lighting", action="store_true", help="Also download the unused lighting generator")
    args = parser.parse_args()
    for model in args.model or list(REVISIONS):
        fetch_model(args.cache.resolve(), model, not args.with_lighting)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
