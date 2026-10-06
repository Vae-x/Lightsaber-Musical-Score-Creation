#!/usr/bin/env python3
"""Isolated, offline InfernoSaber trial runner; never edits source projects."""
from __future__ import annotations

import argparse
import bisect
import hashlib
import json
import math
import os
from pathlib import Path
import random
import re
import shutil
import signal
import subprocess
import sys
import time
import traceback
import zipfile

UPSTREAM_COMMIT = "60780a5acda4da67d0c67d01a4705719a77a91e2"
PROFILES = {"Hard": ("easy_15", 2.5), "Expert": ("expert_15", 4.0)}
MODEL_REVISIONS = {"easy_15": "e04b14ad772a4e091a0df65780ddc4011f43409b",
                   "expert_15": "0cfd41f330f47eba81106684255a916934da3ae7"}
MODEL_PREFIXES = ("tf_model_enc_", "tf_model_mapper_", "tf_beat_gen_", "tf_event_gen_")
MODEL_PICKLES = ("notes_class_dict.pkl", "onehot_encoder_beats.pkl", "onehot_encoder_events.pkl")
RHYTHM_GAP_POLICY = {"mode": "model_actions_with_local_rhythm_gap_fill_v1", "minimumGapSeconds": 4.0,
                     "minimumGapBeats": 4.0, "minimumReferenceEvents": 3, "minimumReferenceSpanSeconds": 3.0,
                     "nearEventToleranceSeconds": 1 / 16, "edgeToleranceSeconds": 0.15}


class TrialError(RuntimeError):
    pass


class TrialCancelled(TrialError):
    pass


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def write_json(path, value):
    """Atomic replacement, only inside the caller's newly allocated trial output."""
    path = Path(path)
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")
    os.replace(temporary, path)


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def status(stage, **details):
    print(json.dumps({"stage": stage, **details}, ensure_ascii=False), flush=True)


def verify_model(model_dir, model):
    model_dir = Path(model_dir).resolve(strict=True)
    manifest = read_json(model_dir / ".trial-model-manifest.json")
    if (manifest.get("complete") is not True or manifest.get("model") != model
            or manifest.get("repo") != "BierHerr/InfernoSaber"
            or manifest.get("revision") != MODEL_REVISIONS[model]):
        raise TrialError("模型缓存来源或完整性标记不正确")
    entries = manifest.get("files", [])
    names = [entry.get("name") for entry in entries]
    if len(names) != len(set(names)) or len(names) < 7:
        raise TrialError("模型清单不完整或包含重复路径")
    for required in MODEL_PICKLES:
        if required not in names:
            raise TrialError("模型缺少 " + required)
    prefixes = MODEL_PREFIXES[:3] if manifest.get("componentMode") == "notes-only" else MODEL_PREFIXES
    if manifest.get("componentMode", "full") not in ("full", "notes-only"):
        raise TrialError("未知模型组件模式")
    if manifest.get("componentMode") == "notes-only":
        skipped = manifest.get("skippedFiles", [])
        if len(skipped) != 1 or not skipped[0].get("name", "").startswith("tf_event_gen_"):
            raise TrialError("仅音符模型清单必须明确记录跳过的灯光模型")
    for prefix in prefixes:
        if sum(isinstance(name, str) and name.startswith(prefix) for name in names) != 1:
            raise TrialError("模型文件无法唯一确定: " + prefix)
        actual = [path.name for path in model_dir.iterdir() if path.name.startswith(prefix)]
        if actual != [name for name in names if name.startswith(prefix)]:
            raise TrialError("模型目录含未核对的重复或临时文件: " + prefix)
    for entry in entries:
        name = entry["name"]
        if not isinstance(name, str) or Path(name).name != name or "/" in name or "\\" in name:
            raise TrialError("模型清单含不安全路径")
        path = model_dir / name
        if not path.is_file() or path.stat().st_size != entry["size"] or sha256(path) != entry["sha256"]:
            raise TrialError("模型文件未完成或校验失败: " + name)
    return manifest


def verify_source(source):
    source = Path(source).resolve(strict=True)
    for command in (["rev-parse", "HEAD"], ["status", "--porcelain", "--untracked-files=no"]):
        completed = subprocess.run(["git", "-c", "core.fsmonitor=false", "-C", str(source), *command],
                                   capture_output=True, text=True, check=False)
        if completed.returncode:
            raise TrialError("无法核对固定上游源码 Git 状态")
        value = completed.stdout.strip()
        if command[0] == "rev-parse" and value != UPSTREAM_COMMIT:
            raise TrialError("上游源码版本与固定提交不一致")
        if command[0] == "status" and value:
            raise TrialError("上游源码有本地改动，拒绝使用未核对版本")
    digest = hashlib.sha256()
    for file in sorted(source.rglob("*.py")):
        if ".git" in file.relative_to(source).parts:
            continue
        digest.update(file.relative_to(source).as_posix().encode())
        digest.update(bytes.fromhex(sha256(file)))
    return {"commit": UPSTREAM_COMMIT, "pythonSourceSha256": digest.hexdigest()}


def process_environment(threads=4, runtime=None, tools=None):
    environment = os.environ.copy()
    environment.update(CUDA_VISIBLE_DEVICES="-1", TF_CPP_MIN_LOG_LEVEL="2",
                       TF_NUM_INTRAOP_THREADS=str(threads), TF_NUM_INTEROP_THREADS="1",
                       OMP_NUM_THREADS=str(threads), OPENBLAS_NUM_THREADS=str(threads),
                       HF_HUB_OFFLINE="1", TRANSFORMERS_OFFLINE="1",
                       PYTHONUNBUFFERED="1", PYTHONIOENCODING="utf-8", PYTHONNOUSERSITE="1",
                       PYTHONDONTWRITEBYTECODE="1")
    environment.pop("PYTHONHOME", None)
    environment.pop("PYTHONPATH", None)
    prefixes = []
    if runtime:
        prefixes.extend([Path(runtime) / "env/Library/bin", Path(runtime) / "env/Scripts", Path(runtime) / "env"])
    if tools:
        prefixes.append(Path(tools))
    environment["PATH"] = os.pathsep.join([str(path) for path in prefixes] + [environment.get("PATH", "")])
    return environment


def stop_process(process):
    # No shell, and only the PID created by this runner is targeted.
    if process.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    else:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


class ProcessMemorySampler:
    """Read the child process's Windows peak working set without optional packages."""
    def __init__(self, pid):
        self.handle = None
        self.peak = None
        if os.name != "nt":
            return
        try:
            import ctypes
            from ctypes import wintypes
            class Counters(ctypes.Structure):
                _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD)] + [
                    (field, ctypes.c_size_t) for field in ("PeakWorkingSetSize", "WorkingSetSize",
                    "QuotaPeakPagedPoolUsage", "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage",
                    "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage")]
            self.ctypes, self.Counters = ctypes, Counters
            self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            self.kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
            self.kernel.OpenProcess.restype = wintypes.HANDLE
            self.kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
            self.query = ctypes.WinDLL("psapi", use_last_error=True).GetProcessMemoryInfo
            self.query.argtypes = (wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD)
            self.query.restype = wintypes.BOOL
            self.handle = self.kernel.OpenProcess(0x0400 | 0x0010, False, pid)
        except (AttributeError, OSError):
            self.handle = None

    def sample(self):
        if self.handle:
            counters = self.Counters()
            counters.cb = self.ctypes.sizeof(counters)
            if self.query(self.handle, self.ctypes.byref(counters), counters.cb) and counters.PeakWorkingSetSize > 0:
                self.peak = max(self.peak or 0, counters.PeakWorkingSetSize)

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


def run_process(command, log_path, timeout=900, cancel_file=None, cwd=None, environment=None):
    started = time.monotonic()
    options = {"cwd": cwd, "env": environment, "stdin": subprocess.DEVNULL}
    if os.name == "nt":
        options["creationflags"] = subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP
    else:
        options["start_new_session"] = True
    with Path(log_path).open("xb") as log:
        process = subprocess.Popen([str(item) for item in command], stdout=log, stderr=subprocess.STDOUT, **options)
        memory = ProcessMemorySampler(process.pid)
        try:
            while process.poll() is None:
                memory.sample()
                if cancel_file and Path(cancel_file).exists():
                    raise TrialCancelled("已取消，保留本次已完成结果")
                if time.monotonic() - started >= timeout:
                    raise TrialError("处理超过 %s 秒，已停止当前子进程" % timeout)
                time.sleep(0.1)
            if process.returncode != 0:
                raise TrialError("子进程失败（退出码 %d），详情见 %s" % (process.returncode, log_path))
        except BaseException:
            stop_process(process)
            raise
        finally:
            memory.sample()
            memory.close()
            write_json(str(log_path) + ".metrics.json", {"elapsedSeconds": time.monotonic() - started,
                "peakWorkingSetBytes": memory.peak, "memoryMeasurementAvailable": memory.peak is not None,
                "returnCode": process.poll(), "measurementScope": "child_process"})
    return time.monotonic() - started


def file_inventory(directory):
    directory = Path(directory)
    return [{"name": item.relative_to(directory).as_posix(), "size": item.stat().st_size,
             "sha256": sha256(item)} for item in sorted(directory.rglob("*")) if item.is_file()]


def inventory_matches(directory, inventory):
    directory = Path(directory)
    try:
        # Require the exact file set, and reject paths escaping the recorded directory.
        for entry in inventory:
            item = directory / entry["name"]
            if not item.resolve().is_relative_to(directory.resolve()):
                return False
        return file_inventory(directory) == inventory
    except (OSError, KeyError, TypeError):
        return False


def load_generator(source, notes_only):
    """Patch exact pinned blocks in memory; never change the upstream checkout."""
    import importlib
    import types
    source_file = Path(source) / "map_creation/gen_beats.py"
    text = source_file.read_text(encoding="utf-8")
    begin = "    if True:\n        # (TODO: add furious_lighting to increase effect frequency)"
    end = "    if config.bs_mapping_version != \"v3\":"
    crop = ("    map_times = map_times[config.lstm_len:]     # required by lstm start-up\n"
            "    map_times = map_times[:len(y_class_map)]    # rest from sectioning into lstm len batches\n")
    if text.count(crop) != 1:
        raise TrialError("上游动作窗口配对与固定版本不一致，拒绝猜测修改")
    patched = text.replace(crop, "    assert len(map_times) == len(y_class_map), 'Padded action timing mismatch'\n")
    preprocessing = ("    song_ar, rm_index = run_music_preprocessing(name_ar, time_ar=[map_times], save_file=False,\n"
                     "                                                song_combined=False, predict_path=True)  # 7.8\n")
    if patched.count(preprocessing) != 1:
        raise TrialError("上游音乐窗口调用与固定版本不一致，拒绝猜测修改")
    patched = patched.replace(preprocessing, "    map_times = trial_complete_rhythm(map_times)\n" + preprocessing)
    if notes_only:
        if patched.count(begin) != 1 or patched.count(end) != 1:
            raise TrialError("上游灯光代码与固定版本不一致，拒绝猜测修改")
        start_index = patched.index(begin)
        end_index = patched.index(end, start_index)
        replacement = "    events = np.zeros((len(map_times), 2), dtype=int)\n"
        patched = patched[:start_index] + replacement + patched[end_index:]
    parent = importlib.import_module("map_creation")
    module = types.ModuleType("map_creation.gen_beats")
    module.__file__ = str(source_file)
    module.__package__ = "map_creation"
    sys.modules[module.__name__] = module
    exec(compile(patched, str(source_file), "exec"), module.__dict__)
    module.trial_complete_rhythm = lambda timings: timings
    parent.gen_beats = module
    return module, {"componentMode": "notes-only" if notes_only else "full",
                    "lightingPolicy": "跳过灯光模型，仅生成空灯光事件" if notes_only else "上游灯光模型",
                    "actionBoundaryPolicy": "首16事件复制首特征作上下文；尾复制末特征补齐；输出按原事件时间配对",
                    "rhythmGapHookPolicy": "调用者先更新 map_times，再创建音乐窗口和编码；无参考时保持原模型时间",
                    "compatibilitySourceSha256": hashlib.sha256(text.encode()).hexdigest(),
                    "compatibilityPatchedSha256": hashlib.sha256(patched.encode()).hexdigest()}


def action_padding_indices(timings, lstm_len):
    """The trained previous-window -> next-window decoder needs one prefix block."""
    timings = [float(value) for value in timings]
    if not timings or lstm_len < 1 or any(not math.isfinite(value) for value in timings):
        raise TrialError("动作输入时间为空或无效")
    if any(second < first for first, second in zip(timings, timings[1:])):
        raise TrialError("动作输入事件时间必须升序")
    deltas = sorted(second - first for first, second in zip(timings, timings[1:]) if second > first)
    step = deltas[len(deltas) // 2] if deltas else 1.0
    tail = (-len(timings)) % lstm_len
    indices = [0] * lstm_len + list(range(len(timings))) + [len(timings) - 1] * tail
    # The original first real event has time_diff=1. Keep that value exactly.
    prefix = [timings[0] - 1.0 - (lstm_len - 1 - index) * step for index in range(lstm_len)]
    suffix = [timings[-1] + (index + 1) * step for index in range(tail)]
    return indices, prefix + timings + suffix, {"prefixContextEvents": lstm_len, "tailPaddingEvents": tail,
                                               "contextIntervalSeconds": step, "outputEvents": len(timings)}


def guard_action_generation(generate, diagnostics):
    """Pad feature context only, trim synthetic targets, preserve all real timings."""
    def guarded(features, timings, model_path, lstm_len, encoder_file):
        if Path(model_path).name.startswith("tf_model_mapper_"):
            count = len(timings)
            diagnostics.update(actionModelInputEvents=count, encodedAudioWindows=len(features),
                               requiredActionInputEvents=2 * lstm_len,
                               usableActionSequenceBlocks=(count + lstm_len - 1) // lstm_len)
            status("action_input", **diagnostics)
            if min(count, len(features)) < 2 * lstm_len:
                raise TrialError("当前音频在静音过滤和边界窗口检查后仅剩 %d 个动作输入事件，"
                                 "模型至少需要 %d 个；保留失败记录，请使用更长或包含更多有效音乐变化的音频"
                                 % (min(count, len(features)), 2 * lstm_len))
            if len(features) != count:
                raise TrialError("编码音乐窗口与动作事件数量不一致")
            import numpy as np
            indices, padded_times, padding = action_padding_indices(timings, lstm_len)
            diagnostics.update(padding)
            decoded = generate(np.asarray(features)[indices], np.asarray(padded_times),
                               model_path, lstm_len, encoder_file)
            expected = count + padding["tailPaddingEvents"]
            if len(decoded) != expected:
                raise TrialError("补齐动作输出数量不一致: 预期 %d，实际 %d" % (expected, len(decoded)))
            status("action_padding", **padding)
            return decoded[:count]
        return generate(features, timings, model_path, lstm_len, encoder_file)
    return guarded


def pcm_silence_intervals(rms_frames, frame_seconds=0.05, duration=None, threshold_dbfs=-70.0,
                          minimum_seconds=0.20, padding_seconds=0.10):
    """Conservative absolute PCM silence; protect both edges of each quiet run."""
    if duration is None:
        duration = len(rms_frames) * frame_seconds
    threshold = 10 ** (threshold_dbfs / 20.0)
    intervals = []
    start = None
    for index in range(len(rms_frames) + 1):
        quiet = index < len(rms_frames) and float(rms_frames[index]) <= threshold
        if quiet and start is None:
            start = index * frame_seconds
        elif not quiet and start is not None:
            end = min(index * frame_seconds, duration)
            if end - start + 1e-9 >= minimum_seconds and end - start > 2 * padding_seconds:
                intervals.append((start + padding_seconds, end - padding_seconds))
            start = None
    return intervals


def install_pcm_silence_filter(module, audio, diagnostics):
    """Analyze the read-only job audio without normalization or changing its bytes."""
    import aubio
    import numpy as np
    samplerate = 16000
    source = aubio.source(str(audio), samplerate=samplerate, channels=0)
    chunks = []
    try:
        while True:
            samples, read = source.do_multi()
            samples = np.asarray(samples)
            if samples.shape != (source.channels, source.hop_size):
                raise TrialError("PCM 声道形状不符合 aubio 固定接口")
            if read:
                chunks.append(samples[:, :read].copy())
            if read < source.hop_size:
                break
    finally:
        source.close()
    channels = source.channels
    pcm = np.concatenate(chunks, axis=1) if chunks else np.zeros((channels, 0))
    if not pcm.shape[1]:
        raise TrialError("音乐解码为空，不能判断真实静音")
    frame = int(samplerate * 0.05)
    rms = [pcm_frame_rms(pcm[:, start:start + frame]) for start in range(0, pcm.shape[1], frame)]
    duration = pcm.shape[1] / samplerate
    intervals = pcm_silence_intervals(rms, duration=duration)
    diagnostics.update(policy="actual_pcm_rms", thresholdDbfs=-70.0, frameSeconds=0.05,
                       minimumSilenceSeconds=0.20, boundaryProtectionSeconds=0.10,
                       sampleRate=samplerate, sourceChannels=channels, channelPolicy="mean_square_before_channel_average",
                       audioDurationSeconds=duration, silenceIntervals=intervals,
                       silenceSeconds=sum(end - start for start, end in intervals))

    def silent_times(_spectrogram, timings):
        times = np.asarray(timings)
        mask = np.zeros(len(times), dtype=bool)
        for start, end in intervals:
            mask |= (times >= start) & (times <= end)
        return times[mask]

    def remove_silent_times(timings, _silent_samples):
        # Use the actual protected intervals, without the old +/-30ms expansion.
        times = np.asarray(timings)
        mask = np.zeros(len(times), dtype=bool)
        for start, end in intervals:
            mask |= (times >= start) & (times <= end)
        diagnostics.update(inputEvents=len(times), removedEvents=int(mask.sum()), retainedEvents=int((~mask).sum()))
        status("pcm_silence", **diagnostics)
        return times[~mask]

    module.get_silent_times = silent_times
    module.remove_silent_times = remove_silent_times


def pcm_frame_rms(channel_samples):
    import numpy as np
    samples = np.asarray(channel_samples, dtype=float)
    return float(np.sqrt(np.mean(samples ** 2)))


def load_rhythm_reference(directory, audio_sha256, bpm, difficulty):
    """Accept a same-audio, fixed-BPM, zero-offset Standard v2 reference."""
    if type(bpm) not in (int, float) or not math.isfinite(bpm) or bpm <= 0 or difficulty not in PROFILES:
        raise TrialError("节奏参考需要有效 BPM 和目标难度")
    directory = Path(directory).resolve(strict=True)
    info_path = directory / "info.dat"
    def read_snapshot(path):
        content = path.read_bytes()
        value = json.loads(content.decode("utf-8-sig"))
        if not isinstance(value, dict):
            raise TrialError("节奏参考 JSON 根必须为对象")
        return value, hashlib.sha256(content).hexdigest()
    info, info_sha256 = read_snapshot(info_path)
    if not str(info.get("_version", "")).startswith("2."):
        raise TrialError("节奏参考必须为基础 v2 歌曲")
    reference_bpm = info.get("_beatsPerMinute")
    if (type(reference_bpm) not in (int, float) or not math.isfinite(reference_bpm)
            or abs(reference_bpm - bpm) > max(1e-7, abs(bpm) * 1e-9)):
        raise TrialError("节奏参考 BPM 与当前音乐不一致")
    offset = info.get("_songTimeOffset", 0)
    if type(offset) not in (int, float) or not math.isfinite(offset) or abs(offset) > 1e-7:
        raise TrialError("节奏参考只接受零偏移，不得重复平移首拍")

    def asset(name):
        if not isinstance(name, str) or not name or Path(name).is_absolute():
            raise TrialError("节奏参考资源路径无效")
        path = (directory / name).resolve(strict=True)
        if not path.is_relative_to(directory) or not path.is_file():
            raise TrialError("节奏参考资源必须位于歌曲目录内")
        return path

    reference_audio = asset(info.get("_songFilename"))
    if sha256(reference_audio) != audio_sha256:
        raise TrialError("节奏参考音频 SHA256 与原音乐不一致")
    entries = [entry for group in info.get("_difficultyBeatmapSets", [])
               if group.get("_beatmapCharacteristicName") == "Standard"
               for entry in group.get("_difficultyBeatmaps", []) if entry.get("_difficulty") == difficulty]
    if len(entries) != 1:
        raise TrialError("节奏参考缺少唯一目标 Standard 难度")
    chart_path = asset(entries[0].get("_beatmapFilename"))
    chart, chart_sha256 = read_snapshot(chart_path)
    if not str(chart.get("_version", "")).startswith("2."):
        raise TrialError("节奏参考谱必须为基础 v2")
    def reject_timing_changes(value):
        if isinstance(value, dict):
            for key, child in value.items():
                if key.lower().lstrip("_") in ("bpmchanges", "bpmevents") and child:
                    raise TrialError("节奏参考必须为固定 BPM，不能包含变速事件")
                reject_timing_changes(child)
        elif isinstance(value, list):
            for child in value:
                reject_timing_changes(child)
    reject_timing_changes(info)
    reject_timing_changes(chart)
    if any(event.get("_type") == 100 for event in chart.get("_events", [])):
        raise TrialError("节奏参考不能包含 BPM 灯光事件")
    times = []
    for note in chart.get("_notes", []):
        if note.get("_type") not in (0, 1):
            continue
        beat = note.get("_time")
        if type(beat) not in (int, float) or not math.isfinite(beat) or beat < 0:
            raise TrialError("节奏参考包含无效音符时间")
        seconds = beat * 60 / bpm
        if not math.isfinite(seconds):
            raise TrialError("节奏参考音符时间超出有效范围")
        times.append(seconds)
    times.sort()
    unique = []
    for value in times:
        if not unique or value - unique[-1] > 1e-7:
            unique.append(value)
    identity = {"audioSha256": audio_sha256, "infoSha256": info_sha256, "chartSha256": chart_sha256,
                "difficulty": difficulty, "bpm": reference_bpm, "songTimeOffset": offset,
                "eventCount": len(unique), "policy": RHYTHM_GAP_POLICY}
    return {"directory": str(directory), "identity": identity, "eventSeconds": unique}


def fill_long_rhythm_gaps(timings, reference, bpm, duration, silence_intervals):
    """Only use actual local events inside a supported long hole in model timing."""
    if not math.isfinite(bpm) or bpm <= 0 or not math.isfinite(duration) or duration <= 0:
        raise TrialError("音乐补缺需要有效 BPM 和音频时长")
    original = sorted(float(value) for value in timings)
    if any(not math.isfinite(value) for value in original):
        raise TrialError("模型音乐点包含无效时间")
    anchors = sorted(set(value for value in original if 0 < value < duration))
    boundaries = [0.0] + anchors + [duration]
    threshold = max(RHYTHM_GAP_POLICY["minimumGapSeconds"], RHYTHM_GAP_POLICY["minimumGapBeats"] * 60 / bpm)
    merged = original[:]
    intervals, total = [], 0
    reference = sorted(set(float(value) for value in reference))
    if any(not math.isfinite(value) for value in reference):
        raise TrialError("本地参考音乐点包含无效时间")
    for start, end in zip(boundaries, boundaries[1:]):
        if end - start < threshold - 1e-7:
            continue
        left = start + (RHYTHM_GAP_POLICY["edgeToleranceSeconds"] if start else 0)
        right = end - (RHYTHM_GAP_POLICY["edgeToleranceSeconds"] if end < duration else 0)
        candidates = reference[bisect.bisect_left(reference, left):bisect.bisect_right(reference, right)]
        candidates = [value for value in candidates if 0 < value < duration and math.isfinite(value)
                      and not any(silent_start <= value <= silent_end for silent_start, silent_end in silence_intervals)]
        added = []
        for value in candidates:
            insertion = bisect.bisect_left(merged, value)
            neighbors = merged[max(0, insertion - 1):insertion + 1]
            if any(abs(value - neighbor) < RHYTHM_GAP_POLICY["nearEventToleranceSeconds"] for neighbor in neighbors):
                continue
            if added and value - added[-1] < RHYTHM_GAP_POLICY["nearEventToleranceSeconds"]:
                continue
            added.append(value)
        if (len(added) < RHYTHM_GAP_POLICY["minimumReferenceEvents"] or
                added[-1] - added[0] < RHYTHM_GAP_POLICY["minimumReferenceSpanSeconds"] - 1e-7):
            continue
        for value in added:
            bisect.insort(merged, value)
        total += len(added)
        intervals.append({"startSeconds": start, "endSeconds": end, "addedEvents": len(added),
                          "firstAddedSeconds": added[0], "lastAddedSeconds": added[-1],
                          "referenceSpanSeconds": added[-1] - added[0]})
    return merged, {"policy": RHYTHM_GAP_POLICY, "thresholdSeconds": threshold, "addedEvents": total,
                    "inputEvents": len(original), "outputEvents": len(merged), "filledIntervals": intervals}


def timing_summary(timings):
    values = sorted(float(value) for value in timings if float(value) > 0)
    gaps = sorted(({"seconds": second - first, "from": first, "to": second}
                   for first, second in zip(values, values[1:])), key=lambda item: item["seconds"], reverse=True)
    return {"events": len(values), "firstSeconds": values[0] if values else None,
            "lastSeconds": values[-1] if values else None, "firstEventSeconds": values[:24], "largestGaps": gaps[:8]}


def install_generation_diagnostics(module, diagnostics):
    def record(stage, timings, **details):
        entry = {"stage": stage, **timing_summary(timings), **details}
        diagnostics.append(entry)
        status("timing_diagnostic", timingStage=stage, **{key: value for key, value in entry.items() if key != "stage"})

    thresholding = module.apply_first_beat_thresholding
    def threshold(values):
        import numpy as np
        scores = np.asarray(values).reshape(-1)
        record("beat_probability_above_base_threshold",
               np.flatnonzero(scores > module.config.thresh_beat) / module.config.beat_spacing,
               baseThreshold=module.config.thresh_beat, introThresholdMultiplier=module.config.threshold_start,
               initialAudioWindowExclusionSeconds=module.config.window)
        result = thresholding(values)
        record("beat_probability_after_intro_threshold",
               np.flatnonzero(np.asarray(result).reshape(-1)) / module.config.beat_spacing)
        return result
    module.apply_first_beat_thresholding = threshold
    for name in ("sanity_check_timing2", "add_start_end_beats", "fill_map_times_scale", "remove_silent_times", "trial_complete_rhythm"):
        delegate = getattr(module, name)
        def wrapped(*args, _delegate=delegate, _name=name, **kwargs):
            before = args[1] if _name == "sanity_check_timing2" else args[0]
            record(_name + "_before", before)
            result = _delegate(*args, **kwargs)
            record(_name + "_after", result, removedEvents=len(before) - len(result))
            return result
        setattr(module, name, wrapped)
    preprocessing = module.run_music_preprocessing
    def preprocess(*args, **kwargs):
        before = kwargs["time_ar"][0]
        record("audio_window_before", before, requiredWindowSeconds=module.config.window)
        result = preprocessing(*args, **kwargs)
        removed = set(result[1][0])
        record("audio_window_after", [value for index, value in enumerate(before) if index not in removed],
               removedEvents=len(removed))
        return result
    module.run_music_preprocessing = preprocess
    generate = module.generate
    def action(features, timings, *args):
        record("action_input", timings)
        result = generate(features, timings, *args)
        record("action_output_aligned", timings, decodedEvents=len(result))
        return result
    module.generate = action
    import map_creation.map_creator_deprecated as creator
    safety = creator.sanity_check_notes
    current_timings = []
    def active(values, timings):
        return [timings[index] for index, note in enumerate(values)
                if any(note[offset] in (0, 1) for offset in range(2, len(note), 4))]
    def note_safety(notes, timings):
        nonlocal current_timings
        current_timings = timings
        before = active(notes, timings)
        record("decoded_before_safe_rules", before)
        result = safety(notes, timings)
        after = active(result, timings)
        record("after_safe_rules", after, removedEvents=len(before) - len(after))
        return result
    creator.sanity_check_notes = note_safety
    import map_creation.sanity_check as safety_module
    for name in ("correct_notes", "turn_notes_single"):
        delegate = getattr(safety_module, name)
        def safe_stage(notes, *args, _delegate=delegate, _name=name, **kwargs):
            before = active(notes, current_timings)
            result = _delegate(notes, *args, **kwargs)
            after = active(result[0] if _name == "turn_notes_single" else result, current_timings)
            record(_name + "_after", after, removedEvents=len(before) - len(after))
            return result
        setattr(safety_module, name, safe_stage)


def bind_generated_difficulty(song_directory, difficulty):
    """Upstream v2 metadata assumes Expert+, even when its writer emits Hard.dat."""
    song_directory = Path(song_directory)
    expected = difficulty + ".dat"
    if not (song_directory / expected).is_file():
        raise TrialError("上游未写出本次难度文件: " + expected)
    info = read_json(song_directory / "info.dat")
    sets = info.get("_difficultyBeatmapSets", [])
    if len(sets) != 1 or sets[0].get("_beatmapCharacteristicName") != "Standard":
        raise TrialError("上游未产生唯一基础 Standard 难度集")
    entries = sets[0].get("_difficultyBeatmaps", [])
    if not entries:
        raise TrialError("上游 Info 没有难度元数据")
    entry = next((entry for entry in entries if entry.get("_difficulty") == difficulty), entries[0])
    entry["_difficulty"] = difficulty
    entry["_difficultyRank"] = {"Hard": 5, "Expert": 7}[difficulty]
    entry["_beatmapFilename"] = expected
    sets[0]["_difficultyBeatmaps"] = [entry]
    write_json(song_directory / "info.dat", info)


def worker(args):
    """Called in one fresh Python process per chart, with an isolated writable CWD."""
    if not args.notes_only:
        raise TrialError("本轮仅验证 notes-only；灯光模型的窗口配对未验证，不支持 --no-notes-only")
    source = Path(args.source or Path(args.runtime) / "source").resolve(strict=True)
    audio = Path(args.audio).resolve(strict=True)
    if args.ffmpeg:
        args.ffmpeg = str(Path(args.ffmpeg).resolve(strict=True))
    job = Path(args.job_dir).resolve()
    if not source.joinpath("main.py").is_file():
        raise TrialError("上游源码目录缺少 main.py")
    source_identity = verify_source(source)
    if job == source or job.is_relative_to(source) or source.is_relative_to(job):
        raise TrialError("工作目录必须与上游源码分离")
    if audio.is_relative_to(job):
        raise TrialError("输入音频不能位于尚未建立的工作目录")
    manifest = verify_model(args.model_dir, args.model)
    if not args.notes_only and manifest.get("componentMode") == "notes-only":
        raise TrialError("此缓存未下载灯光模型，只支持 --notes-only")
    source_audio_sha256 = sha256(audio)
    reference = load_rhythm_reference(args.rhythm_reference, source_audio_sha256, args.bpm, args.difficulty) if args.rhythm_reference else None
    job.mkdir(parents=True, exist_ok=False)
    started = time.monotonic()
    result = {"complete": False, "upstreamCommit": UPSTREAM_COMMIT, "sourceIdentity": source_identity, "model": args.model,
              "modelRevision": manifest["revision"], "difficulty": args.difficulty,
              "strength": args.strength, "seed": args.seed, "bpm": args.bpm,
              "sourceAudioSha256": source_audio_sha256, "audioTimeShiftSeconds": 0,
              "firstBeatPolicy": "以原音频绝对秒生成；未按工程首拍再次平移", "cpuThreads": args.threads}
    result["rhythmPolicy"] = "模型动作＋本地补缺节奏" if reference else "原模型节奏（首尾窗口修复）"
    result["rhythmReference"] = {key: value for key, value in reference.items() if key != "eventSeconds"} if reference else None
    try:
        status("prepare_audio", model=args.model, difficulty=args.difficulty)
        input_directory = job / "prediction" / "songs_predict"
        output_directory = job / "prediction" / "new_map"
        input_directory.mkdir(parents=True)
        output_directory.mkdir(parents=True)
        copied_audio = input_directory / "song.egg"
        with audio.open("rb") as stream:
            ogg = stream.read(4) == b"OggS"
        if ogg:
            shutil.copyfile(audio, copied_audio)
            result["audioPreparation"] = "原 Ogg 字节复制"
        else:
            if not args.ffmpeg:
                raise TrialError("非 Ogg 输入需要 --ffmpeg")
            run_process([args.ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-i", audio,
                         "-vn", "-c:a", "libvorbis", "-q:a", "6", "-f", "ogg", copied_audio],
                        job / "audio-convert.log", timeout=120, environment=process_environment(args.threads))
            result["audioPreparation"] = "FFmpeg 转 Ogg；未归一化或平移"
        result["preparedAudioSha256"] = sha256(copied_audio)
        if ogg and result["preparedAudioSha256"] != source_audio_sha256:
            raise TrialError("音乐在只读复制期间改变，停止本次模型生成")
        os.environ.update(process_environment(args.threads, args.runtime, Path(args.ffmpeg).parent if args.ffmpeg else None))
        dll_directories = []
        if os.name == "nt" and hasattr(os, "add_dll_directory"):
            for directory in (Path(args.runtime) / "env/Library/bin", Path(args.runtime) / "env"):
                if directory.is_dir():
                    dll_directories.append(os.add_dll_directory(str(directory.resolve())))
        os.chdir(source)
        sys.dont_write_bytecode = True
        sys.path.insert(0, str(source))
        from tools.config import config, paths
        config.use_mapper_selection = args.model
        config.bs_mapping_version = "v2"
        config.general_diff = args.difficulty
        config.create_expert_flag = False
        config.max_speed = config.max_speed_orig = args.strength * 4.0
        config.use_fixed_bpm = args.bpm
        config.enable_auto_metadata = False
        config.normalize_song_flag = False
        config.increase_volume_flag = False
        config.auto_move_song_afterwards = False
        config.add_obstacle_flag = False
        config.add_slider_flag = False
        config.allow_dot_notes = True
        config.add_breaks_flag = False
        config.num_workers = 1
        config.random_seed = args.seed
        # Redirect every writable upstream path, including failure cleanup paths.
        paths.dir_path = job.as_posix() + "/"
        paths.bs_song_path = paths.bs_input_path = ""
        for field, relative in {
            "pred_path": "prediction", "train_path": "training", "temp_path": "temp",
            "copy_path_song": "training/songs_egg", "copy_path_map": "training/maps",
            "dict_all_path": "training/maps_dict_all", "songs_pred": "prediction/songs_predict",
            "new_map_path": "prediction/new_map", "fail_path": "training/fail_list",
            "diff_path": "training/songs_diff", "song_data": "training/song_data",
            "ml_input_path": "training/ml_input",
        }.items():
            directory = job / relative
            directory.mkdir(parents=True, exist_ok=True)
            setattr(paths, field, directory.as_posix() + "/")
        for field, relative in {
            "diff_ar_file": "training/songs_diff/diff_ar.npy", "name_ar_file": "training/songs_diff/name_ar.npy",
            "ml_input_beat_file": "training/ml_input/beat_ar.npy", "ml_input_song_file": "training/ml_input/song_ar.npy",
            "black_list_file": "training/fail_list/black_list.txt",
        }.items():
            setattr(paths, field, (job / relative).as_posix())
        paths.model_path = Path(args.model_dir).resolve().as_posix() + "/"
        paths.notes_classify_dict_file = paths.model_path + "notes_class_dict.pkl"
        paths.beats_classify_encoder_file = paths.model_path + "onehot_encoder_beats.pkl"
        paths.events_classify_encoder_file = paths.model_path + "onehot_encoder_events.pkl"
        from PIL import Image
        cover = Path(args.cover).resolve(strict=True) if args.cover else source / "app_helper/cover.jpg"
        if not cover.is_file():
            raise TrialError("找不到试用封面；请提供 --cover")
        with Image.open(cover) as image:
            image.convert("RGB").save(output_directory / "cover.jpg", "JPEG", quality=95)
        random.seed(args.seed)
        import numpy as np
        import tensorflow as tf
        np.random.seed(args.seed)
        tf.keras.utils.set_random_seed(args.seed)
        tf.config.set_visible_devices([], "GPU")
        tf.config.threading.set_intra_op_parallelism_threads(args.threads)
        tf.config.threading.set_inter_op_parallelism_threads(1)
        os.chdir(job)
        gen_beats, compatibility = load_generator(source, args.notes_only)
        result.update(compatibility)
        if hasattr(gen_beats, "generate"):
            result["inputDiagnostics"] = {}
            gen_beats.generate = guard_action_generation(gen_beats.generate, result["inputDiagnostics"])
        if hasattr(gen_beats, "get_silent_times"):
            result["silenceDiagnostics"] = {}
            install_pcm_silence_filter(gen_beats, copied_audio, result["silenceDiagnostics"])
            if reference:
                def complete_rhythm(timings):
                    completed, details = fill_long_rhythm_gaps(timings, reference["eventSeconds"], args.bpm,
                        result["silenceDiagnostics"]["audioDurationSeconds"], result["silenceDiagnostics"]["silenceIntervals"])
                    result["rhythmGapFill"] = details
                    status("rhythm_gap_fill", **details)
                    return np.asarray(completed)
                gen_beats.trial_complete_rhythm = complete_rhythm
            result["timingDiagnostics"] = []
            install_generation_diagnostics(gen_beats, result["timingDiagnostics"])
        result["syntheticBreaksEnabled"] = False
        status("generate", model=args.model, difficulty=args.difficulty)
        if gen_beats.main(["song"]):
            raise TrialError("上游未产生足够音符，当前歌曲生成失败")
        songs = [path.parent for path in output_directory.rglob("info.dat")]
        if len(songs) != 1:
            raise TrialError("上游输出歌曲目录无法唯一确定")
        song_directory = songs[0]
        bind_generated_difficulty(song_directory, args.difficulty)
        result.update(complete=True, outputSongDirectory=str(song_directory),
                      outputFiles=file_inventory(song_directory), elapsedSeconds=time.monotonic() - started)
        status("complete", outputSongDirectory=str(song_directory), elapsedSeconds=result["elapsedSeconds"])
    except BaseException as error:
        result.update(error=str(error), exceptionType=type(error).__name__, elapsedSeconds=time.monotonic() - started)
        traceback.print_exc()
        raise
    finally:
        write_json(job / "worker-result.json", result)
    return result


def output_fingerprint(describe, model_manifest, args, difficulty, rhythm_reference_identity=None):
    audio = Path(describe["audioPath"])
    cover = Path(describe["coverPath"]) if describe.get("coverPath") else None
    model, strength = PROFILES[difficulty]
    material = {"runnerSha256": sha256(__file__), "coreSha256": sha256(args.core_cli),
                "projectSha256": sha256(describe["project"]), "audioSha256": sha256(audio),
                "coverSha256": sha256(cover) if cover and cover.is_file() else None,
                "sourceHashes": describe.get("sourceHashes"),
                "modelManifest": {key: model_manifest.get(key) for key in
                                  ("model", "repo", "revision", "componentMode", "files", "skippedFiles")},
                "model": model, "strength": strength, "difficulty": difficulty, "seed": args.seed,
                "upstreamCommit": UPSTREAM_COMMIT, "threads": args.threads,
                "rhythmReferenceIdentity": rhythm_reference_identity,
                "rhythmPolicy": RHYTHM_GAP_POLICY if rhythm_reference_identity else "model_rhythm_boundary_fixed",
                "sourceIdentity": args.source_identity, "environmentInfoSha256": args.environment_info_sha256}
    return hashlib.sha256(json.dumps(material, sort_keys=True, ensure_ascii=False).encode()).hexdigest()


def reusable_result(previous_run, fingerprint):
    if not previous_run:
        return None
    root = Path(previous_run).resolve()
    try:
        report = read_json(root / "trial-report.json")
        for entry in report["results"]:
            if entry.get("complete") and entry.get("fingerprint") == fingerprint:
                song = root / entry["songRelativePath"]
                if song.resolve().is_relative_to(root) and inventory_matches(song, entry["outputFiles"]):
                    return entry, song
    except (OSError, ValueError, KeyError, TypeError):
        pass
    return None


def archive_song(song_directory, destination):
    with zipfile.ZipFile(destination, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for entry in file_inventory(song_directory):
            archive.write(Path(song_directory) / entry["name"], entry["name"])
    with zipfile.ZipFile(destination) as archive:
        if archive.testzip() is not None:
            raise TrialError("ZIP 内容校验失败")
    return {"name": Path(destination).name, "sha256": sha256(destination), "size": Path(destination).stat().st_size}


def song_statistics(song_directory):
    """Observable distributions only; these numbers do not prove enjoyable play."""
    song_directory = Path(song_directory)
    info = read_json(song_directory / "info.dat")
    entry = info["_difficultyBeatmapSets"][0]["_difficultyBeatmaps"][0]
    chart = read_json(song_directory / entry["_beatmapFilename"])
    notes = sorted((note for note in chart.get("_notes", []) if note.get("_type") in (0, 1)),
                   key=lambda note: (note["_time"], note["_type"], note["_lineIndex"], note["_lineLayer"]))
    directions = {str(index): 0 for index in range(9)}
    positions = {"%d,%d" % (x, y): 0 for x in range(4) for y in range(3)}
    hands = {"0": 0, "1": 0}
    signatures = []
    for note in notes:
        directions[str(note["_cutDirection"])] = directions.get(str(note["_cutDirection"]), 0) + 1
        position = "%d,%d" % (note["_lineIndex"], note["_lineLayer"])
        positions[position] = positions.get(position, 0) + 1
        hands[str(note["_type"])] += 1
        signatures.append((note["_type"], note["_lineIndex"], note["_lineLayer"], note["_cutDirection"]))
    loop_16 = sum(signatures[index:index + 16] == signatures[index + 16:index + 32]
                  for index in range(max(0, len(signatures) - 31)))
    phrases = {}
    for note in notes:
        beat = note["_time"]
        phrase = math.floor(beat / 8)
        phrases.setdefault(phrase, []).append((round(beat - phrase * 8, 4), note["_type"],
                                              note["_lineIndex"], note["_lineLayer"], note["_cutDirection"]))
    longest = streak = 1 if phrases else 0
    previous = None
    previous_index = None
    for index, phrase in sorted(phrases.items()):
        streak = streak + 1 if index == (previous_index or 0) + 1 and phrase == previous else 1
        longest = max(longest, streak)
        previous, previous_index = phrase, index
    return {"noteCount": len(notes), "directionCounts": directions, "positionCounts": positions,
            "handCounts": hands, "adjacentIdentical16NoteWindows": loop_16,
            "longestIdentical8BeatPhraseRun": longest}


def combined_package(output, project_dir, results, index, algorithm):
    """Combine Hard/Expert only when both complete copies use identical audio/time metadata."""
    relevant = [item for item in results if item["algorithm"] == algorithm and item["complete"]]
    if len(relevant) != 2:
        return None
    destination = project_dir / (algorithm + "-combined")
    first_song = output / relevant[0]["songRelativePath"]
    second_song = output / relevant[1]["songRelativePath"]
    first = read_json(first_song / "info.dat")
    second = read_json(second_song / "info.dat")
    for field in ("_beatsPerMinute", "_songTimeOffset"):
        if first.get(field, 0) != second.get(field, 0):
            raise TrialError("两张难度的时间映射不同，保留单难度包")
    if sha256(first_song / first["_songFilename"]) != sha256(second_song / second["_songFilename"]):
        raise TrialError("两张难度的歌曲资源不同，保留单难度包")
    first_cover = first.get("_coverImageFilename")
    second_cover = second.get("_coverImageFilename")
    if bool(first_cover) != bool(second_cover):
        raise TrialError("两张难度的封面存在性不同，保留单难度包")
    if first_cover and sha256(first_song / first_cover) != sha256(second_song / second_cover):
        raise TrialError("两张难度的歌曲资源不同，保留单难度包")
    shutil.copytree(first_song, destination)
    target_entries = first["_difficultyBeatmapSets"][0]["_difficultyBeatmaps"]
    for entry in second["_difficultyBeatmapSets"][0]["_difficultyBeatmaps"]:
        source_file = second_song / entry["_beatmapFilename"]
        target_file = destination / entry["_beatmapFilename"]
        if target_file.exists():
            raise TrialError("两张难度谱文件重名，保留单难度包")
        shutil.copyfile(source_file, target_file)
        target_entries.append(entry)
    first["_songName"] = relevant[0]["songName"]
    first["_songSubName"] = "纯本地算法对照" if algorithm == "local" else "InfernoSaber 模型动作＋本地补缺节奏"
    write_json(destination / "info.dat", first)
    readiness = all(item["playableReady"] for item in relevant)
    category = "ready" if readiness else "editor-only"
    folder = output / category
    folder.mkdir(exist_ok=True)
    song_name = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "_", relevant[0]["songName"]).strip(" .")[:70] or "歌曲"
    label = "本地算法" if algorithm == "local" else "现成模型"
    zip_path = folder / ("%02d-%s-%s.zip" % (index, song_name, label))
    archive = archive_song(destination, zip_path)
    archive["relativePath"] = zip_path.relative_to(output).as_posix()
    return {"songName": relevant[0]["songName"], "algorithm": algorithm, "playableReady": readiness,
            "difficulties": [item["difficulty"] for item in relevant], "zip": archive}


def batch(args):
    workspace = Path(args.workspace).resolve(strict=True)
    output = Path(args.output).resolve()
    # User songs/results are only allowed in the repository's existing ignored areas.
    if not any(output.is_relative_to(workspace / part) for part in ("build", "dist", "projects", "samples/beatmaps")):
        raise TrialError("试用输出必须位于仓库已有忽略目录 build/dist/projects/samples/beatmaps")
    output.mkdir(parents=True, exist_ok=False)
    runtime = Path(args.runtime).resolve(strict=True)
    args.core_cli = str(Path(args.core_cli).resolve(strict=True))
    args.tools = str(Path(args.tools).resolve(strict=True))
    interpreter = Path(args.python or runtime / "env/python.exe").resolve(strict=True)
    source = Path(args.source or runtime / "source").resolve(strict=True)
    args.source_identity = verify_source(source)
    environment_info = runtime / "environment-info.json"
    args.environment_info_sha256 = sha256(environment_info) if environment_info.is_file() else None
    model_root = Path(args.models or workspace / "build/infernosaber-trial/models").resolve(strict=True)
    projects = args.projects if args.command == "batch" else [args.project]
    cancel_file = Path(args.cancel_file) if args.cancel_file else output / "cancel.request"
    report = {"schemaVersion": 1, "seed": args.seed, "upstreamCommit": UPSTREAM_COMMIT,
              "runtime": str(runtime), "sourceIdentity": args.source_identity, "models": {}, "results": [],
              "comparisonPackages": [], "complete": False,
              "cancelFile": str(cancel_file), "desktopVerified": False, "headsetVerified": False}
    start = time.monotonic()
    failures = 0
    try:
        for difficulty, (model, _) in PROFILES.items():
            report["models"][model] = verify_model(model_root / model, model)
        for index, project in enumerate(projects, 1):
            if cancel_file.exists():
                raise TrialCancelled("已取消，保留本次已完成结果")
            project_dir = output / ("song-%02d" % index)
            project_dir.mkdir()
            describe_path = project_dir / "describe.json"
            run_process([args.core_cli, "describe", "--project", project, "--report", describe_path],
                        project_dir / "describe.log", timeout=120, cancel_file=cancel_file,
                        environment=process_environment(args.threads))
            describe = read_json(describe_path)
            bpm = describe.get("baseBpm") or describe.get("bpm")
            if not isinstance(bpm, (float, int)) or not math.isfinite(bpm) or bpm <= 0:
                raise TrialError("工程未提供有效 BPM")
            project_results = []
            for difficulty, (model, strength) in PROFILES.items():
                fingerprint = output_fingerprint(describe, report["models"][model], args, difficulty)
                for algorithm in ("local", "model"):
                    if cancel_file.exists():
                        raise TrialCancelled("已取消，保留本次已完成结果")
                    job = project_dir / (algorithm + "-" + difficulty)
                    job.mkdir()
                    result = {"songName": describe.get("songName", describe.get("title", "歌曲")),
                              "difficulty": difficulty, "algorithm": algorithm, "complete": False,
                              "fingerprint": fingerprint + ":" + algorithm}
                    report["results"].append(result)
                    project_results.append(result)
                    status("chart_start", song=result["songName"], difficulty=difficulty, algorithm=algorithm)
                    try:
                        if algorithm == "model":
                            local_result = next((item for item in project_results if item["algorithm"] == "local"
                                                 and item["difficulty"] == difficulty and item["complete"]), None)
                            if not local_result:
                                raise TrialError("同难度本地节奏基准未生成成功，不能进行已验证的混合补缺")
                            rhythm_reference = output / local_result["songRelativePath"]
                            reference = load_rhythm_reference(rhythm_reference, sha256(describe["audioPath"]), bpm, difficulty)
                            result["rhythmReferenceIdentity"] = reference["identity"]
                            result["rhythmPolicy"] = "模型动作＋本地补缺节奏"
                            result["fingerprint"] = output_fingerprint(describe, report["models"][model], args,
                                                                      difficulty, reference["identity"]) + ":model"
                        prior = reusable_result(args.resume, result["fingerprint"])
                        if prior:
                            old, old_song = prior
                            song = job / "song"
                            shutil.copytree(old_song, song)
                            result.update({key: value for key, value in old.items()
                                           if key not in ("songRelativePath", "zip", "outputFiles")})
                            result["reused"] = True
                        else:
                            check_report = job / "core-report.json"
                            song = job / "song"
                            chart_start = time.monotonic()
                            if algorithm == "local":
                                run_process([args.core_cli, "local", "--project", project, "--difficulty", difficulty,
                                             "--seed", args.seed, "--output", song, "--report", check_report,
                                             "--tools", args.tools], job / "core.log", timeout=args.timeout,
                                            cancel_file=cancel_file, environment=process_environment(args.threads))
                            else:
                                command = [interpreter, Path(__file__).resolve(), "worker", "--runtime", runtime,
                                           "--source", source, "--model-dir", model_root / model, "--model", model,
                                           "--audio", describe["audioPath"], "--job-dir", job / "worker",
                                           "--rhythm-reference", rhythm_reference,
                                           "--difficulty", difficulty, "--strength", strength,
                                           "--bpm", bpm, "--seed", args.seed,
                                           "--ffmpeg", Path(args.tools) / "ffmpeg.exe", "--threads", args.threads]
                                if describe.get("coverPath"):
                                    command.extend(["--cover", describe["coverPath"]])
                                run_process(command, job / "model.log", timeout=args.timeout, cancel_file=cancel_file,
                                            environment=process_environment(args.threads, runtime, args.tools))
                                generated = read_json(job / "worker/worker-result.json")
                                if (generated.get("rhythmReference") or {}).get("identity") != reference["identity"]:
                                    raise TrialError("节奏参考在任务期间改变，原输出已保留，本次不应用")
                                result["modelGeneration"] = generated
                                remaining = max(1, args.timeout - (time.monotonic() - chart_start))
                                run_process([args.core_cli, "check", "--song", generated["outputSongDirectory"],
                                             "--difficulty", difficulty, "--output", song, "--report", check_report,
                                             "--tools", args.tools], job / "core.log", timeout=remaining,
                                            cancel_file=cancel_file, environment=process_environment(args.threads))
                            result["coreCheck"] = read_json(check_report)
                            result["elapsedSeconds"] = time.monotonic() - chart_start
                            result["processMetrics"] = {}
                            for stage, name in (("model", "model.log"), ("core", "core.log")):
                                metrics_path = job / (name + ".metrics.json")
                                if metrics_path.is_file():
                                    result["processMetrics"][stage] = read_json(metrics_path)
                        readiness = bool(result.get("coreCheck", {}).get("playableReady", False))
                        if result.get("coreCheck", {}).get("formatValid") is not True:
                            raise TrialError("输出格式未通过校验，原输出已保留，未制作导入包")
                        category = "ready" if readiness else "editor-only"
                        zip_directory = output / "chart-zips" / category
                        zip_directory.mkdir(parents=True, exist_ok=True)
                        destination = zip_directory / ("song-%02d-%s-%s.zip" % (index, algorithm, difficulty))
                        statistics = song_statistics(song)
                        result.update(complete=True, playableReady=readiness,
                                      songRelativePath=song.relative_to(output).as_posix(),
                                      outputFiles=file_inventory(song), zip=archive_song(song, destination))
                        result["statistics"] = statistics
                        result["zip"]["relativePath"] = destination.relative_to(output).as_posix()
                    except TrialCancelled:
                        raise
                    except (TrialError, OSError, ValueError, KeyError) as error:
                        result["error"] = str(error)
                        failures += 1
                        status("chart_failed", **result)
                    write_json(output / "trial-report.json", report)
            for algorithm in ("local", "model"):
                try:
                    package = combined_package(output, project_dir, project_results, index, algorithm)
                    if package:
                        report["comparisonPackages"].append(package)
                except (TrialError, OSError, ValueError, KeyError) as error:
                    report.setdefault("packagingWarnings", []).append(str(error))
            write_json(output / "trial-report.json", report)
        report.update(complete=True, failures=failures)
    except (TrialCancelled, KeyboardInterrupt) as error:
        report.update(cancelled=True, error=str(error) or "已取消")
    finally:
        report["elapsedSeconds"] = time.monotonic() - start
        write_json(output / "trial-report.json", report)
    status("batch_complete", complete=report["complete"], failures=failures,
           cancelled=report.get("cancelled", False), report=str(output / "trial-report.json"))
    return 130 if report.get("cancelled") else (2 if failures else 0)


def parser():
    result = argparse.ArgumentParser(description="现成模型本地试用；原工程和歌曲只读")
    commands = result.add_subparsers(dest="command", required=True)
    for name in ("batch", "run"):
        command = commands.add_parser(name)
        command.add_argument("--runtime", default="E:/lmsc-infernosaber-runtime")
        command.add_argument("--core-cli", required=True)
        command.add_argument("--workspace", required=True)
        command.add_argument("--output", required=True)
        command.add_argument("--tools", required=True)
        if name == "batch":
            command.add_argument("--projects", nargs="+", required=True)
        else:
            command.add_argument("--project", required=True)
        command.add_argument("--source")
        command.add_argument("--python")
        command.add_argument("--models")
        command.add_argument("--resume")
        command.add_argument("--cancel-file")
        command.add_argument("--timeout", type=float, default=900)
        command.add_argument("--threads", type=int, default=4)
        command.add_argument("--seed", type=int, default=20261005)
    command = commands.add_parser("worker")
    command.add_argument("--runtime", default="E:/lmsc-infernosaber-runtime")
    command.add_argument("--source")
    command.add_argument("--model-dir", required=True)
    command.add_argument("--model", choices=MODEL_REVISIONS, required=True)
    command.add_argument("--audio", required=True)
    command.add_argument("--job-dir", required=True)
    command.add_argument("--difficulty", choices=PROFILES, required=True)
    command.add_argument("--strength", type=float, required=True)
    command.add_argument("--bpm", type=float, required=True)
    command.add_argument("--seed", type=int, default=20261005)
    command.add_argument("--ffmpeg")
    command.add_argument("--cover")
    command.add_argument("--rhythm-reference", help="同音频、同 BPM、同难度的本地基准歌曲目录；只补音乐长空段")
    command.add_argument("--threads", type=int, default=4)
    command.add_argument("--notes-only", action=argparse.BooleanOptionalAction, default=True)
    return result


def main(argv=None):
    args = parser().parse_args(argv)
    if args.threads < 1 or args.seed < 0 or args.seed >= 2**32:
        raise TrialError("threads 至少为 1，seed 必须为 uint32")
    if args.command == "worker":
        if not math.isfinite(args.bpm) or args.bpm <= 0 or not math.isfinite(args.strength) or args.strength <= 0:
            raise TrialError("BPM 和强度必须为有限正数")
        worker(args)
        return 0
    if args.timeout <= 0 or not math.isfinite(args.timeout):
        raise TrialError("超时必须为有限正数")
    return batch(args)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (TrialError, OSError, ValueError, KeyError) as error:
        status("error", error=str(error))
        sys.exit(2)
