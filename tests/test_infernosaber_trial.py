"""Offline contract checks; no third-party models, GPU, network or songs needed."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("infernosaber_trial", Path(__file__).parents[1] / "scripts/infernosaber_trial.py")
trial = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trial)


def create_model(directory, model="easy_15", notes_only=False):
    directory.mkdir(parents=True)
    names = [prefix + "fixture.h5" for prefix in trial.MODEL_PREFIXES if not (notes_only and prefix == "tf_event_gen_")]
    names += list(trial.MODEL_PICKLES) + ["tf_model_autoenc_fixture.h5"]
    entries = []
    for name in names:
        file = directory / name
        file.write_bytes((name + " synthetic fixture").encode())
        entries.append({"name": name, "size": file.stat().st_size, "sha256": trial.sha256(file)})
    manifest = {"complete": True, "model": model, "repo": "BierHerr/InfernoSaber",
                "revision": trial.MODEL_REVISIONS[model], "files": entries}
    if notes_only:
        manifest.update(componentMode="notes-only", skippedFiles=[{"name": "tf_event_gen_fixture.h5", "size": 1,
                                                                  "sha256": "0" * 64}])
    trial.write_json(directory / ".trial-model-manifest.json", manifest)
    return manifest


def create_song(directory, difficulty="Hard"):
    directory.mkdir(parents=True)
    (directory / "song.egg").write_bytes(b"OggS synthetic audio")
    (directory / "cover.jpg").write_bytes(b"synthetic cover")
    (directory / (difficulty + ".dat")).write_text(json.dumps({"_version": "2.2.0", "_notes": [
        {"_time": 2, "_lineIndex": 0, "_lineLayer": 1, "_type": 0, "_cutDirection": 1}], "_obstacles": [],
        "_events": []}), encoding="utf-8")
    trial.write_json(directory / "info.dat", {"_version": "2.0.0", "_songName": "合成测试",
        "_beatsPerMinute": 120, "_songFilename": "song.egg", "_coverImageFilename": "cover.jpg",
        "_difficultyBeatmapSets": [{"_beatmapCharacteristicName": "Standard", "_difficultyBeatmaps": [
            {"_difficulty": difficulty, "_difficultyRank": 5, "_beatmapFilename": difficulty + ".dat"}]}]})


class ModelCacheTests(unittest.TestCase):
    def test_full_and_notes_only_cache(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for notes_only in (False, True):
                path = root / str(notes_only)
                manifest = create_model(path, notes_only=notes_only)
                self.assertEqual(trial.verify_model(path, "easy_15"), manifest)

    def test_partial_and_changed_cache_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "model"
            manifest = create_model(path)
            (path / manifest["files"][0]["name"]).write_bytes(b"interrupted download")
            with self.assertRaises(trial.TrialError):
                trial.verify_model(path, "easy_15")
            manifest["complete"] = False
            trial.write_json(path / ".trial-model-manifest.json", manifest)
            with self.assertRaises(trial.TrialError):
                trial.verify_model(path, "easy_15")

    def test_wrong_revision_and_traversal_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "model"
            manifest = create_model(path)
            manifest["revision"] = "0" * 40
            trial.write_json(path / ".trial-model-manifest.json", manifest)
            with self.assertRaises(trial.TrialError):
                trial.verify_model(path, "easy_15")
            manifest["revision"] = trial.MODEL_REVISIONS["easy_15"]
            manifest["files"][-1]["name"] = "../outside.h5"
            trial.write_json(path / ".trial-model-manifest.json", manifest)
            with self.assertRaises(trial.TrialError):
                trial.verify_model(path, "easy_15")

    def test_unverified_model_with_same_prefix_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "model"
            create_model(path)
            (path / "tf_model_mapper_unverified.h5.part").write_bytes(b"partial")
            with self.assertRaisesRegex(trial.TrialError, "未核对"):
                trial.verify_model(path, "easy_15")


class ProcessTests(unittest.TestCase):
    def test_nonzero_process_has_log(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "process.log"
            with self.assertRaises(trial.TrialError):
                trial.run_process([sys.executable, "-c", "print('fixture failure'); raise SystemExit(7)"], log)
            self.assertIn("fixture failure", log.read_text())
            measurements = trial.read_json(str(log) + ".metrics.json")
            self.assertEqual(measurements["returnCode"], 7)
            self.assertGreater(measurements["elapsedSeconds"], 0)
            self.assertEqual(measurements["memoryMeasurementAvailable"], measurements["peakWorkingSetBytes"] is not None)
            self.assertEqual(measurements["measurementScope"], "child_process")

    def test_timeout_terminates_child(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(trial.TrialError, "超过"):
                trial.run_process([sys.executable, "-c", "import time; time.sleep(60)"],
                                  Path(temporary) / "timeout.log", timeout=0.02)

    def test_cancel_keeps_log_and_terminates_child(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "cancel").touch()
            with self.assertRaises(trial.TrialCancelled):
                trial.run_process([sys.executable, "-c", "import time; time.sleep(60)"],
                                  root / "cancel.log", cancel_file=root / "cancel")
            self.assertTrue((root / "cancel.log").exists())


class GeneratorCompatibilityTests(unittest.TestCase):
    def test_hard_info_binds_actual_filename_instead_of_expert_plus(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "song"
            create_song(root, "Hard")
            info = trial.read_json(root / "info.dat")
            entry = info["_difficultyBeatmapSets"][0]["_difficultyBeatmaps"][0]
            entry.update(_difficulty="ExpertPlus", _difficultyRank=9, _beatmapFilename="ExpertPlus.dat")
            entry["_noteJumpMovementSpeed"] = 15.5
            trial.write_json(root / "info.dat", info)
            trial.bind_generated_difficulty(root, "Hard")
            actual = trial.read_json(root / "info.dat")["_difficultyBeatmapSets"][0]["_difficultyBeatmaps"]
            self.assertEqual(len(actual), 1)
            self.assertEqual(actual[0]["_beatmapFilename"], "Hard.dat")
            self.assertEqual(actual[0]["_difficultyRank"], 5)
            self.assertEqual(actual[0]["_noteJumpMovementSpeed"], 15.5)

    def test_expert_info_removes_unwritten_expert_plus_placeholder(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "song"
            create_song(root, "Expert")
            info = trial.read_json(root / "info.dat")
            info["_difficultyBeatmapSets"][0]["_difficultyBeatmaps"].append(
                {"_difficulty": "ExpertPlus", "_difficultyRank": 9, "_beatmapFilename": "ExpertPlus.dat"})
            trial.write_json(root / "info.dat", info)
            trial.bind_generated_difficulty(root, "Expert")
            actual = trial.read_json(root / "info.dat")["_difficultyBeatmapSets"][0]["_difficultyBeatmaps"]
            self.assertEqual(len(actual), 1)
            self.assertEqual(actual[0]["_beatmapFilename"], "Expert.dat")
            self.assertEqual(actual[0]["_difficultyRank"], 7)

    def test_insufficient_mapper_input_is_explained_before_empty_reshape(self):
        delegate = mock.Mock()
        diagnostics = {}
        guarded = trial.guard_action_generation(delegate, diagnostics)
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(trial.TrialError, "模型至少需要 32"):
                guarded([0] * 24, [0] * 24, "tf_model_mapper_fixture.h5", 16, "encoder.pkl")
        delegate.assert_not_called()
        self.assertEqual(diagnostics["actionModelInputEvents"], 24)
        self.assertEqual(diagnostics["usableActionSequenceBlocks"], 2)

    def test_mapper_padding_preserves_all_original_event_indices(self):
        import numpy as np
        features, timings = np.arange(35).reshape(-1, 1), np.arange(35, dtype=float) + 2.0
        def decode(padded_features, padded_times, model, length, encoder):
            self.assertEqual(padded_features.shape, (64, 1))
            self.assertEqual(padded_times[16] - padded_times[15], 1.0)
            return padded_features[16:, 0]
        guarded = trial.guard_action_generation(decode, {})
        with contextlib.redirect_stdout(io.StringIO()):
            actual = guarded(features, timings, "tf_model_mapper_fixture.h5", 16, "encoder.pkl")
        self.assertEqual(actual.tolist(), list(range(35)))
        self.assertEqual(features[:, 0].tolist(), list(range(35)))
        self.assertEqual(timings.tolist(), list(range(2, 37)))

    def test_padding_alignment_for_multiple_window_lengths(self):
        for count in (1, 15, 16, 17, 31, 32, 35, 48, 64, 65):
            times = [2.0 + index * 0.4 for index in range(count)]
            indices, padded, metadata = trial.action_padding_indices(times, 16)
            self.assertEqual(len(indices) % 16, 0)
            self.assertEqual(indices[16:16 + count], list(range(count)))
            self.assertEqual(padded[16:16 + count], times)
            self.assertAlmostEqual(padded[16] - padded[15], 1)
            # Stock targets = windows[1:], not windows[:-1] used as model inputs.
            target_windows = [indices[start:start + 16] for start in range(16, len(indices), 16)]
            self.assertEqual([index for window in target_windows for index in window][:count], list(range(count)))
            self.assertEqual(metadata["tailPaddingEvents"], (-count) % 16)

    def test_invalid_padding_and_wrong_decoder_length_are_rejected(self):
        for times in ([], [1, 0], [float('nan')]):
            with self.assertRaises(trial.TrialError):
                trial.action_padding_indices(times, 16)
        guarded = trial.guard_action_generation(mock.Mock(return_value=[0] * 31), {})
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(trial.TrialError, "补齐动作输出"):
            guarded([0] * 32, list(range(32)), "tf_model_mapper_fixture.h5", 16, "encoder.pkl")

    def test_true_pcm_silence_retains_soft_intro_and_protects_edges(self):
        # These are nonzero low-volume musical frames, followed by a true rest.
        frames = [0.001] * 200 + [0.0] * 20 + [0.002] * 200
        self.assertEqual(trial.pcm_silence_intervals(frames), [(10.1, 10.9)])
        self.assertEqual(trial.pcm_silence_intervals([0.0004] * 400), [])
        self.assertEqual(trial.pcm_silence_intervals([0.01] * 20 + [0.0] * 3 + [0.01] * 20), [])

    def test_pcm_detector_uses_actual_decoded_duration_and_finite_frames(self):
        self.assertEqual(trial.pcm_silence_intervals([0] * 10, duration=0.48), [(0.1, 0.38)])

    def test_opposite_phase_stereo_music_remains_audible(self):
        samples = [[0.01, -0.01] * 400, [-0.01, 0.01] * 400]
        rms = trial.pcm_frame_rms(samples)
        self.assertAlmostEqual(rms, 0.01)
        self.assertEqual(trial.pcm_silence_intervals([rms] * 400), [])

    def test_pcm_filter_retains_quiet_music_and_true_rest_boundary_notes(self):
        import numpy as np
        class AudioSource:
            channels, hop_size = 2, 800
            closed = False
            def __init__(self):
                self.index = 0
            def do_multi(self):
                amplitude = 0.001 if self.index < 20 else (0 if self.index < 40 else 0.002)
                read = 800 if self.index < 60 else 0
                self.index += 1
                return np.asarray([[amplitude] * 800, [-amplitude] * 800]), read
            def close(self):
                self.closed = True
        source = AudioSource()
        aubio = types.SimpleNamespace(source=mock.Mock(return_value=source))
        module, diagnostics = types.SimpleNamespace(), {}
        with mock.patch.dict(sys.modules, {"aubio": aubio}), contextlib.redirect_stdout(io.StringIO()):
            trial.install_pcm_silence_filter(module, "readonly.ogg", diagnostics)
            times = [0.5, 1.0, 1.05, 1.2, 1.8, 1.95, 2.05]
            actual = module.remove_silent_times(times, module.get_silent_times(None, times))
        self.assertEqual(actual.tolist(), [0.5, 1.0, 1.05, 1.95, 2.05])
        self.assertEqual(diagnostics["removedEvents"], 2)
        self.assertEqual(diagnostics["sourceChannels"], 2)
        self.assertEqual(diagnostics["silenceIntervals"], [(1.1, 1.9)])
        self.assertTrue(source.closed)
        aubio.source.assert_called_once_with("readonly.ogg", samplerate=16000, channels=0)

    def test_timing_diagnostics_records_threshold_and_safe_deletions(self):
        import numpy as np
        creator = types.ModuleType("map_creation.map_creator_deprecated")
        creator.sanity_check_notes = lambda notes, times: [[], notes[1]]
        safety = types.ModuleType("map_creation.sanity_check")
        safety.correct_notes = lambda notes, times: notes
        safety.turn_notes_single = lambda notes: (notes, [])
        package = types.ModuleType("map_creation")
        package.map_creator_deprecated, package.sanity_check = creator, safety
        module = types.SimpleNamespace(config=types.SimpleNamespace(thresh_beat=.4, beat_spacing=10,
            threshold_start=1.1, window=2), apply_first_beat_thresholding=lambda values: np.asarray(values) > .4,
            sanity_check_timing2=lambda name, times: times, add_start_end_beats=lambda times, *args: times,
            fill_map_times_scale=lambda times, *args: times, remove_silent_times=lambda times, *args: times,
            trial_complete_rhythm=lambda times: times,
            run_music_preprocessing=lambda *args, **kwargs: ([], [[0]]),
            generate=lambda features, times, *args: [0] * len(times))
        diagnostics = []
        with mock.patch.dict(sys.modules, {"map_creation": package, "map_creation.map_creator_deprecated": creator,
                "map_creation.sanity_check": safety}), contextlib.redirect_stdout(io.StringIO()):
            trial.install_generation_diagnostics(module, diagnostics)
            module.apply_first_beat_thresholding(np.asarray([.1, .9, .1]))
            creator.sanity_check_notes([[0, 0, 0, 1], [3, 0, 1, 0]], [1, 2])
        self.assertEqual(diagnostics[0]["stage"], "beat_probability_above_base_threshold")
        self.assertEqual(diagnostics[0]["initialAudioWindowExclusionSeconds"], 2)
        self.assertEqual(diagnostics[-1]["removedEvents"], 1)
        self.assertEqual(diagnostics[-1]["firstSeconds"], 2)

    def test_lighting_omission_changes_only_exact_block_in_memory(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "map_creation").mkdir()
            text = ('import numpy as np\n'
                    'def main():\n'
                    '    map_times = [1, 2]\n'
                    '    song_ar, rm_index = run_music_preprocessing(name_ar, time_ar=[map_times], save_file=False,\n'
                    '                                                song_combined=False, predict_path=True)  # 7.8\n'
                    '    y_class_map = [4, 5]\n'
                    '    map_times = map_times[config.lstm_len:]     # required by lstm start-up\n'
                    '    map_times = map_times[:len(y_class_map)]    # rest from sectioning into lstm len batches\n'
                    '    if True:\n'
                    '        # (TODO: add furious_lighting to increase effect frequency)\n'
                    '        raise RuntimeError("must not load lighting")\n'
                    '    else:\n'
                    '        events = []\n'
                    '    if config.bs_mapping_version != "v3":\n'
                    '        return events\n')
            path = root / "map_creation/gen_beats.py"
            path.write_text(text, encoding="utf-8")
            fake_np = types.SimpleNamespace(zeros=lambda shape, dtype: [[0, 0] for _ in range(shape[0])])
            package = types.ModuleType("map_creation")
            with mock.patch.dict(sys.modules, {"map_creation": package, "numpy": fake_np}):
                module, metadata = trial.load_generator(root, True)
                module.config = types.SimpleNamespace(bs_mapping_version="v2")
                module.name_ar = ["fixture"]
                module.run_music_preprocessing = lambda *args, **kwargs: ([], [[]])
                self.assertEqual(module.main(), [[0, 0], [0, 0]])
                self.assertEqual(metadata["componentMode"], "notes-only")
                self.assertNotEqual(metadata["compatibilityPatchedSha256"], metadata["compatibilitySourceSha256"])
            self.assertEqual(path.read_text(encoding="utf-8"), text)

    def test_rhythm_hook_updates_caller_before_audio_windows_and_keeps_alignment(self):
        import numpy as np
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "map_creation").mkdir()
            text = ('import numpy as np\n'
                    'def main():\n'
                    '    map_times = np.asarray([1., 9.])\n'
                    '    song_ar, rm_index = run_music_preprocessing(name_ar, time_ar=[map_times], save_file=False,\n'
                    '                                                song_combined=False, predict_path=True)  # 7.8\n'
                    '    map_times = np.delete(map_times, rm_index[0])\n'
                    '    y_class_map = song_ar\n'
                    '    map_times = map_times[config.lstm_len:]     # required by lstm start-up\n'
                    '    map_times = map_times[:len(y_class_map)]    # rest from sectioning into lstm len batches\n'
                    '    if True:\n'
                    '        # (TODO: add furious_lighting to increase effect frequency)\n'
                    '        raise RuntimeError("must not load lighting")\n'
                    '    if config.bs_mapping_version != "v3":\n'
                    '        return map_times.tolist(), y_class_map\n')
            source_file = root / "map_creation/gen_beats.py"
            source_file.write_text(text, encoding="utf-8")
            observed = []
            def preprocess(*args, **kwargs):
                times = kwargs["time_ar"][0].tolist()
                observed.extend(times)
                return [value for index, value in enumerate(times) if index != 2], [[2]]
            with mock.patch.dict(sys.modules, {"map_creation": types.ModuleType("map_creation"), "numpy": np}):
                module, _ = trial.load_generator(root, True)
                module.config = types.SimpleNamespace(bs_mapping_version="v2")
                module.name_ar = ["fixture"]
                module.trial_complete_rhythm = lambda times: np.asarray([1., 3., 6., 9.])
                module.run_music_preprocessing = preprocess
                times, features = module.main()
            self.assertEqual(observed, [1., 3., 6., 9.])
            self.assertEqual(times, features)
            self.assertEqual(times, [1., 3., 9.])
            self.assertEqual(source_file.read_text(encoding="utf-8"), text)

    def test_unverified_full_lighting_mode_is_rejected_before_accessing_inputs(self):
        args = types.SimpleNamespace(notes_only=False)
        with self.assertRaisesRegex(trial.TrialError, "notes-only"):
            trial.worker(args)

    def test_unknown_upstream_code_refuses_patch(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "map_creation").mkdir()
            (root / "map_creation/gen_beats.py").write_text("# changed upstream\n")
            with self.assertRaises(trial.TrialError):
                trial.load_generator(root, True)

    def test_worker_input_read_only_and_all_writable_paths_isolated(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            (source / "main.py").write_text("# pinned fixture")
            audio = root / "original audio.ogg"
            audio.write_bytes(b"OggS original bytes")
            cover = root / "cover.jpg"
            cover.write_bytes(b"original cover")
            original_audio = trial.sha256(audio)
            model = root / "model"
            create_model(model, notes_only=True)
            job = root / "job"
            args = trial.parser().parse_args(["worker", "--runtime", str(root / "runtime"), "--source", str(source),
                "--model-dir", str(model), "--model", "easy_15", "--audio", str(audio), "--cover", str(cover),
                "--job-dir", str(job), "--difficulty", "Hard", "--strength", "2.5", "--bpm", "120"])
            config = types.SimpleNamespace()
            paths = types.SimpleNamespace()
            tools = types.ModuleType("tools")
            configuration = types.ModuleType("tools.config")
            configuration.config, configuration.paths = config, paths
            tools.config = configuration
            fake_np = types.SimpleNamespace(random=types.SimpleNamespace(seed=mock.Mock()))
            fake_tf = types.SimpleNamespace(keras=types.SimpleNamespace(utils=types.SimpleNamespace(set_random_seed=mock.Mock())),
                config=types.SimpleNamespace(set_visible_devices=mock.Mock(), threading=types.SimpleNamespace(
                    set_intra_op_parallelism_threads=mock.Mock(), set_inter_op_parallelism_threads=mock.Mock())))
            fake_pil = types.ModuleType("PIL")
            fake_pil.Image = mock.MagicMock()
            def generate(names):
                self.assertEqual(names, ["song"])
                self.assertFalse(config.normalize_song_flag)
                self.assertFalse(config.auto_move_song_afterwards)
                self.assertFalse(config.enable_auto_metadata)
                self.assertFalse(config.add_obstacle_flag)
                self.assertFalse(config.add_slider_flag)
                self.assertFalse(config.create_expert_flag)
                self.assertFalse(config.add_breaks_flag)
                self.assertEqual(config.max_speed, 10)
                self.assertEqual(config.bs_mapping_version, "v2")
                for field, value in vars(paths).items():
                    if field not in ("model_path", "notes_classify_dict_file", "beats_classify_encoder_file",
                                     "events_classify_encoder_file", "bs_song_path", "bs_input_path"):
                        self.assertTrue(Path(value).resolve().is_relative_to(job.resolve()), field)
                self.assertEqual((Path(paths.songs_pred) / "song.egg").read_bytes(), audio.read_bytes())
                create_song(Path(paths.new_map_path) / "output", "Hard")
                return False
            original_cwd = os.getcwd()
            try:
                with mock.patch.dict(sys.modules, {"tools": tools, "tools.config": configuration,
                        "numpy": fake_np, "tensorflow": fake_tf, "PIL": fake_pil}), \
                        mock.patch.dict(os.environ, {}, clear=False), \
                        mock.patch.object(trial, "verify_source", return_value={"commit": trial.UPSTREAM_COMMIT}), \
                        mock.patch.object(trial, "load_generator", return_value=(types.SimpleNamespace(main=generate), {})), \
                        contextlib.redirect_stdout(io.StringIO()):
                    result = trial.worker(args)
                self.assertTrue(result["complete"])
                self.assertEqual(result["sourceAudioSha256"], original_audio)
                self.assertEqual(trial.sha256(audio), original_audio)
                self.assertEqual(result["audioTimeShiftSeconds"], 0)
                self.assertEqual(result["preparedAudioSha256"], original_audio)
            finally:
                os.chdir(original_cwd)


class RhythmReferenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.song = self.root / "song"
        create_song(self.song)
        self.audio_hash = trial.sha256(self.song / "song.egg")

    def tearDown(self):
        self.temporary.cleanup()

    def load(self, bpm=120, difficulty="Hard", audio_hash=None):
        return trial.load_rhythm_reference(self.song, audio_hash or self.audio_hash, bpm, difficulty)

    def edit(self, name, change):
        path = self.song / name
        value = trial.read_json(path)
        change(value)
        trial.write_json(path, value)

    def test_same_audio_fixed_bpm_and_difficulty_deduplicate_simultaneous_hands(self):
        self.edit("Hard.dat", lambda value: value["_notes"].extend([
            {"_time": 2, "_type": 1}, {"_time": 8, "_type": 0}, {"_time": 3, "_type": 3}]))
        reference = self.load()
        self.assertEqual(reference["eventSeconds"], [1.0, 4.0])
        self.assertEqual(reference["identity"]["eventCount"], 2)
        self.assertEqual(reference["identity"]["chartSha256"], trial.sha256(self.song / "Hard.dat"))

    def test_audio_bpm_offset_and_difficulty_mismatch_are_rejected(self):
        for options in ({"audio_hash": "0" * 64}, {"bpm": 121}, {"difficulty": "Expert"}, {"bpm": 0}):
            with self.subTest(options=options), self.assertRaises(trial.TrialError):
                self.load(**options)
        self.edit("info.dat", lambda value: value.update(_songTimeOffset=.1))
        with self.assertRaisesRegex(trial.TrialError, "零偏移"):
            self.load()

    def test_timing_changes_in_nested_custom_fields_and_light_events_are_rejected(self):
        for name, change in (("info.dat", lambda value: value.update(_customData={"_BPMChanges": [{"_BPM": 130}]})),
                             ("Hard.dat", lambda value: value.update(_customData={"bpmEvents": [{"b": 4}]})),
                             ("Hard.dat", lambda value: value.update(_events=[{"_type": 100, "_time": 4}]))):
            with self.subTest(name=name):
                original = (self.song / name).read_bytes()
                self.edit(name, change)
                with self.assertRaises(trial.TrialError):
                    self.load()
                (self.song / name).write_bytes(original)

    def test_path_escape_duplicate_difficulty_and_nonfinite_time_are_rejected(self):
        (self.root / "outside.egg").write_bytes((self.song / "song.egg").read_bytes())
        self.edit("info.dat", lambda value: value.update(_songFilename="../outside.egg"))
        with self.assertRaisesRegex(trial.TrialError, "目录内"):
            self.load()
        self.edit("info.dat", lambda value: value.update(_songFilename="song.egg"))
        self.edit("info.dat", lambda value: value["_difficultyBeatmapSets"][0]["_difficultyBeatmaps"].append(
            dict(value["_difficultyBeatmapSets"][0]["_difficultyBeatmaps"][0])))
        with self.assertRaisesRegex(trial.TrialError, "唯一"):
            self.load()
        self.edit("info.dat", lambda value: value["_difficultyBeatmapSets"][0].update(
            _difficultyBeatmaps=value["_difficultyBeatmapSets"][0]["_difficultyBeatmaps"][:1]))
        # JSON decoder accepts NaN, but the timing contract must not.
        (self.song / "Hard.dat").write_text('{"_version":"2.2.0","_notes":[{"_type":0,"_time":NaN}]}')
        with self.assertRaisesRegex(trial.TrialError, "无效"):
            self.load()

    def test_fill_uses_four_second_or_four_beat_threshold_and_retains_existing_times(self):
        original = [1., 5., 5.5, 10.]
        merged, details = trial.fill_long_rhythm_gaps(original, [1.3, 2.8, 4.5, 5.8, 7.3, 9.3], 120, 10.5, [])
        self.assertEqual(details["thresholdSeconds"], 4.)
        self.assertEqual(details["addedEvents"], 6)
        self.assertTrue(all(value in merged for value in original))
        slower, details = trial.fill_long_rhythm_gaps([1., 6.], [1.3, 3, 5.5], 40, 6.5, [])
        self.assertEqual(details["thresholdSeconds"], 6.)
        self.assertEqual(slower, [1., 6.])

    def test_fill_requires_three_independent_events_spanning_three_seconds(self):
        for reference in ([2, 5], [2, 2, 5], [2, 3, 4.99]):
            with self.subTest(reference=reference):
                merged, details = trial.fill_long_rhythm_gaps([1, 8], reference, 120, 9, [])
                self.assertEqual(merged, [1, 8])
                self.assertEqual(details["addedEvents"], 0)
        merged, details = trial.fill_long_rhythm_gaps([1, 8], [2, 3, 5], 120, 9, [])
        self.assertEqual(merged, [1, 2, 3, 5, 8])
        self.assertEqual(details["addedEvents"], 3)

    def test_fill_excludes_actual_silence_and_near_duplicate_events(self):
        reference = [0.96, 1, 1.04, 2, 2.04, 3, 5, 6, 7, 7.96]
        merged, details = trial.fill_long_rhythm_gaps([1, 8], reference, 120, 9, [(2.5, 5.5)])
        self.assertEqual(merged, [1, 2, 6, 7, 8])
        self.assertEqual(details["addedEvents"], 3)

    def test_silent_leading_middle_and_trailing_holes_stay_empty(self):
        merged, details = trial.fill_long_rhythm_gaps([8, 15], [1, 3, 5, 9, 11, 13, 16, 18, 20],
                                                    120, 22, [(0, 7), (8.5, 14), (15.5, 22)])
        self.assertEqual(merged, [8, 15])
        self.assertEqual(details["addedEvents"], 0)


class TrialBatchTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.runtime = self.root / "runtime"
        (self.runtime / "env").mkdir(parents=True)
        (self.runtime / "env/python.exe").write_bytes(b"interpreter fixture")
        (self.runtime / "source").mkdir()
        self.project = self.root / "project.lmsc"
        self.project.write_text("synthetic project")
        self.audio = self.root / "audio.ogg"
        self.audio.write_bytes(b"OggS original")
        self.cover = self.root / "cover.jpg"
        self.cover.write_bytes(b"original cover")
        self.core = self.root / "core.exe"
        self.core.write_bytes(b"core fixture")
        self.models = self.root / "build/infernosaber-trial/models"
        for model in trial.MODEL_REVISIONS:
            create_model(self.models / model, model=model, notes_only=True)
        self.commands = []
        self.cancel_after = None
        self.fail_model = False
        self.base_bpm_only = False
        self.no_cover = False
        self.one_difficulty_cover = False
        self.changed_reference_during_worker = False

    def tearDown(self):
        self.temporary.cleanup()

    def args(self, name="first", resume=None):
        args = trial.parser().parse_args(["batch", "--runtime", str(self.runtime), "--core-cli", str(self.core),
            "--workspace", str(self.root), "--output", str(self.root / "build" / name),
            "--projects", str(self.project), "--tools", str(self.root)])
        args.resume = str(resume) if resume else None
        return args

    def fake_process(self, command, log_path, **options):
        command = [str(item) for item in command]
        self.commands.append(command)
        Path(log_path).write_text("synthetic log")
        value = lambda flag: command[command.index(flag) + 1]
        action = command[1]
        if action == "describe":
            description = {"project": str(self.project), "songName": "中文 歌曲",
                "audioPath": str(self.audio), "coverPath": str(self.cover), "bpm": 120, "baseBpm": 120,
                "firstBeatSeconds": 0.25, "sourceHashes": {"audioSha256": trial.sha256(self.audio)}}
            if self.base_bpm_only:
                del description["bpm"]
            trial.write_json(value("--report"), description)
        elif "worker" in command:
            if self.fail_model:
                raise trial.TrialError("synthetic model failure")
            job = Path(value("--job-dir"))
            job.mkdir()
            create_song(job / "raw", value("--difficulty"))
            reference = trial.load_rhythm_reference(value("--rhythm-reference"), trial.sha256(self.audio),
                                                    120, value("--difficulty"))
            if self.changed_reference_during_worker:
                reference["identity"]["chartSha256"] = "0" * 64
            trial.write_json(job / "worker-result.json", {"complete": True, "outputSongDirectory": str(job / "raw"),
                                                         "rhythmReference": {"identity": reference["identity"]}})
        else:
            song = Path(value("--output"))
            difficulty = value("--difficulty")
            create_song(song, difficulty)
            (song / "song.egg").write_bytes(self.audio.read_bytes())
            if self.no_cover or (self.one_difficulty_cover and difficulty == "Expert"):
                info = trial.read_json(song / "info.dat")
                del info["_coverImageFilename"]
                trial.write_json(song / "info.dat", info)
                (song / "cover.jpg").unlink()
            trial.write_json(value("--report"), {"formatValid": True, "playableReady": action == "local",
                                                  "metrics": {"averageNps": 2.5}})
        if self.cancel_after and len(self.commands) == self.cancel_after:
            Path(options["cancel_file"]).touch()
        return 0.01

    def run_batch(self, args):
        with mock.patch.object(trial, "run_process", side_effect=self.fake_process), \
                mock.patch.object(trial, "verify_source", return_value={"commit": trial.UPSTREAM_COMMIT}), \
                contextlib.redirect_stdout(io.StringIO()):
            return trial.batch(args)

    def test_four_charts_new_jobs_and_editor_only_separation(self):
        args = self.args()
        original = trial.sha256(self.audio)
        self.assertEqual(self.run_batch(args), 0)
        report = trial.read_json(Path(args.output) / "trial-report.json")
        self.assertEqual(len(report["results"]), 4)
        self.assertTrue(all(result["complete"] for result in report["results"]))
        self.assertEqual(len(list((Path(args.output) / "chart-zips/ready").glob("*.zip"))), 2)
        self.assertEqual(len(list((Path(args.output) / "chart-zips/editor-only").glob("*.zip"))), 2)
        self.assertEqual(len(report["comparisonPackages"]), 2)
        self.assertEqual(len(list((Path(args.output) / "ready").glob("*.zip"))), 1)
        self.assertEqual(len(list((Path(args.output) / "editor-only").glob("*.zip"))), 1)
        combined_info = trial.read_json(Path(args.output) / "song-01/model-combined/info.dat")
        self.assertEqual(combined_info["_songName"], "中文 歌曲")
        self.assertEqual(len(combined_info["_difficultyBeatmapSets"][0]["_difficultyBeatmaps"]), 2)
        workers = [command for command in self.commands if "worker" in command]
        self.assertEqual(len(workers), 2)
        self.assertTrue(all("--rhythm-reference" in command for command in workers))
        self.assertNotEqual(workers[0][workers[0].index("--job-dir") + 1], workers[1][workers[1].index("--job-dir") + 1])
        self.assertEqual(trial.sha256(self.audio), original)
        with self.assertRaises(FileExistsError):
            self.run_batch(args)

    def test_resume_requires_exact_hash_and_copies_into_new_run(self):
        first = self.args()
        self.run_batch(first)
        self.commands.clear()
        second = self.args("second", Path(first.output))
        self.assertEqual(self.run_batch(second), 0)
        self.assertEqual(len(self.commands), 1)  # describe, then four verified cached outputs
        report = trial.read_json(Path(second.output) / "trial-report.json")
        self.assertTrue(all(item["reused"] for item in report["results"]))
        (Path(first.output) / "song-01/local-Hard/song/song.egg").write_bytes(b"changed")
        self.commands.clear()
        third = self.args("third", Path(first.output))
        self.run_batch(third)
        self.assertEqual(len(self.commands), 2)  # corrupt local Hard must be regenerated
        self.audio.write_bytes(b"OggS changed source")
        self.commands.clear()
        self.run_batch(self.args("fourth", Path(second.output)))
        self.assertEqual(len(self.commands), 7)

    def test_changed_reference_chart_invalidates_only_its_model_cache(self):
        first = self.args()
        self.assertEqual(self.run_batch(first), 0)
        path = Path(first.output)
        chart = path / "song-01/local-Hard/song/Hard.dat"
        value = trial.read_json(chart)
        value["_notes"][0]["_time"] = 4
        trial.write_json(chart, value)
        # A valid baseline cache with altered content must still invalidate the model cache.
        report = trial.read_json(path / "trial-report.json")
        entry = next(item for item in report["results"] if item["algorithm"] == "local" and item["difficulty"] == "Hard")
        entry["outputFiles"] = trial.file_inventory(chart.parent)
        trial.write_json(path / "trial-report.json", report)
        self.commands.clear()
        second = self.args("second", path)
        self.assertEqual(self.run_batch(second), 0)
        self.assertEqual(len(self.commands), 3)  # describe plus Hard worker/check
        actual = trial.read_json(Path(second.output) / "trial-report.json")
        hard = next(item for item in actual["results"] if item["algorithm"] == "model" and item["difficulty"] == "Hard")
        expert = next(item for item in actual["results"] if item["algorithm"] == "model" and item["difficulty"] == "Expert")
        self.assertNotIn("reused", hard)
        self.assertTrue(expert["reused"])

    def test_failure_preserves_completed_baselines(self):
        args = self.args()
        self.fail_model = True
        self.assertEqual(self.run_batch(args), 2)
        report = trial.read_json(Path(args.output) / "trial-report.json")
        self.assertEqual(report["failures"], 2)
        self.assertEqual(sum(item["complete"] for item in report["results"]), 2)
        self.assertEqual(len(list((Path(args.output) / "chart-zips/ready").glob("*.zip"))), 2)

    def test_reference_change_during_worker_preserves_outputs_without_packaging_them(self):
        args = self.args()
        self.changed_reference_during_worker = True
        self.assertEqual(self.run_batch(args), 2)
        report = trial.read_json(Path(args.output) / "trial-report.json")
        failed = [item for item in report["results"] if not item["complete"]]
        self.assertEqual(len(failed), 2)
        self.assertTrue(all("任务期间改变" in item["error"] for item in failed))
        self.assertTrue((Path(args.output) / "song-01/model-Hard/worker/raw/Hard.dat").exists())
        self.assertFalse((Path(args.output) / "chart-zips/editor-only").exists())

    def test_cancel_retains_first_chart(self):
        args = self.args()
        self.cancel_after = 2  # describe, then local Hard
        self.assertEqual(self.run_batch(args), 130)
        report = trial.read_json(Path(args.output) / "trial-report.json")
        self.assertTrue(report["cancelled"])
        self.assertEqual(sum(item["complete"] for item in report["results"]), 1)

    def test_output_must_remain_in_ignored_data_directory(self):
        args = self.args()
        args.output = str(self.root / "tracked")
        with self.assertRaises(trial.TrialError):
            self.run_batch(args)

    def test_describe_base_bpm_alone_is_sufficient(self):
        args = self.args()
        self.base_bpm_only = True
        self.assertEqual(self.run_batch(args), 0)
        report = trial.read_json(Path(args.output) / "trial-report.json")
        self.assertTrue(all(item["complete"] for item in report["results"]))

    def test_two_difficulties_without_covers_can_be_combined(self):
        args = self.args()
        self.no_cover = True
        self.assertEqual(self.run_batch(args), 0)
        report = trial.read_json(Path(args.output) / "trial-report.json")
        self.assertEqual(len(report["comparisonPackages"]), 2)
        info = trial.read_json(Path(args.output) / "song-01/local-combined/info.dat")
        self.assertNotIn("_coverImageFilename", info)
        self.assertFalse((Path(args.output) / "song-01/local-combined/cover.jpg").exists())

    def test_one_difficulty_cover_mismatch_keeps_single_difficulty_packages(self):
        args = self.args()
        self.one_difficulty_cover = True
        self.assertEqual(self.run_batch(args), 0)
        report = trial.read_json(Path(args.output) / "trial-report.json")
        self.assertEqual(report["comparisonPackages"], [])
        self.assertEqual(len(report["packagingWarnings"]), 2)
        self.assertTrue(all("封面存在性不同" in warning for warning in report["packagingWarnings"]))
        self.assertEqual(len(list((Path(args.output) / "chart-zips/ready").glob("*.zip"))), 2)


if __name__ == "__main__":
    unittest.main()
