"""Pinned model downloads are tested with synthetic HTTP responses only."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("fetch_infernosaber_models", Path(__file__).parents[1] / "scripts/fetch-infernosaber-models.py")
fetch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fetch)


class Response(io.BytesIO):
    def __init__(self, data, content_range=None, status=206):
        super().__init__(data)
        self.status = status
        self.headers = {"Content-Range": content_range} if content_range else {}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def model_metadata(model="easy_15"):
    names = ["notes_class_dict.pkl", "onehot_encoder_beats.pkl", "onehot_encoder_events.pkl",
             "tf_model_enc_fixture.h5", "tf_model_autoenc_fixture.h5", "tf_model_mapper_fixture.h5",
             "tf_beat_gen_fixture.h5", "tf_event_gen_fixture.h5"]
    return {"sha": fetch.REVISIONS[model], "siblings": [
        {"rfilename": name, "size": 6, "lfs": {"sha256": digest(b"abcdef"), "size": 6}} for name in names]}


class DownloadTests(unittest.TestCase):
    def test_bad_existing_file_is_preserved_without_download(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / "model.h5"
            target.write_bytes(b"wrong!")
            with mock.patch.object(fetch.urllib.request, "urlopen") as network:
                with self.assertRaisesRegex(ValueError, "preserved"):
                    fetch.download("https://fixture.invalid/model", target, 6, digest(b"abcdef"))
                network.assert_not_called()
            self.assertEqual(target.read_bytes(), b"wrong!")

    def test_valid_existing_file_uses_cache(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / "model.h5"
            target.write_bytes(b"abcdef")
            with mock.patch.object(fetch.urllib.request, "urlopen") as network, contextlib.redirect_stdout(io.StringIO()):
                fetch.download("https://fixture.invalid/model", target, 6, digest(b"abcdef"))
                network.assert_not_called()

    def test_incorrect_content_range_refuses_append(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / "model.h5"
            partial = target.with_name("model.h5.part")
            partial.write_bytes(b"abc")
            with mock.patch.object(fetch.urllib.request, "urlopen", return_value=Response(b"def", "bytes 0-2/6")):
                with self.assertRaisesRegex(ValueError, "range"):
                    fetch.download("https://fixture.invalid/model", target, 6, digest(b"abcdef"))
            self.assertEqual(partial.read_bytes(), b"abc")
            self.assertFalse(target.exists())

    def test_incorrect_range_end_is_rejected_without_modifying_partial(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / "model.h5"
            partial = target.with_name("model.h5.part")
            partial.write_bytes(b"abc")
            with mock.patch.object(fetch.urllib.request, "urlopen", return_value=Response(b"de", "bytes 3-4/6")):
                with self.assertRaisesRegex(ValueError, "range"):
                    fetch.download("https://fixture.invalid/model", target, 6, digest(b"abcdef"))
            self.assertEqual(partial.read_bytes(), b"abc")
            self.assertFalse(target.exists())

    def test_interrupted_range_resumes_at_actual_local_length(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / "model.h5"
            target.with_name("model.h5.part").write_bytes(b"abc")
            requests = []
            def network(request, timeout):
                requests.append(request.get_header("Range"))
                return Response(b"d", "bytes 3-5/6") if len(requests) == 1 else Response(b"ef", "bytes 4-5/6")
            with mock.patch.object(fetch.urllib.request, "urlopen", side_effect=network), contextlib.redirect_stdout(io.StringIO()):
                fetch.download("https://fixture.invalid/model", target, 6, digest(b"abcdef"))
            self.assertEqual(requests, ["bytes=3-5", "bytes=4-5"])
            self.assertEqual(target.read_bytes(), b"abcdef")
            self.assertFalse(target.with_name("model.h5.part").exists())

    def test_completed_partial_requires_hash_before_promotion(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / "model.h5"
            partial = target.with_name("model.h5.part")
            partial.write_bytes(b"wrong!")
            with mock.patch.object(fetch.urllib.request, "urlopen") as network:
                with self.assertRaisesRegex(ValueError, "hash mismatch"):
                    fetch.download("https://fixture.invalid/model", target, 6, digest(b"abcdef"))
                network.assert_not_called()
            self.assertEqual(partial.read_bytes(), b"wrong!")


class ModelManifestTests(unittest.TestCase):
    def test_notes_only_manifest_skips_exactly_lighting(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            requests = []
            def network(request, timeout):
                requests.append(request.full_url)
                if "/api/models/" in request.full_url:
                    return Response(json.dumps(model_metadata()).encode(), status=200)
                return Response(b"abcdef", "bytes 0-5/6")
            with mock.patch.object(fetch.urllib.request, "urlopen", side_effect=network), contextlib.redirect_stdout(io.StringIO()):
                manifest = fetch.fetch_model(cache, "easy_15")
            self.assertEqual(manifest["componentMode"], "notes-only")
            self.assertEqual(len(manifest["files"]), 7)
            self.assertEqual(manifest["skippedFiles"][0]["name"], "tf_event_gen_fixture.h5")
            self.assertFalse(any("resolve/" in url and "tf_event_gen_" in url for url in requests))
            saved = json.loads((cache / "easy_15/.trial-model-manifest.json").read_text())
            self.assertTrue(saved["complete"])
            self.assertEqual(saved, manifest)

    def test_bad_range_never_marks_bundle_complete(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            def network(request, timeout):
                if "/api/models/" in request.full_url:
                    return Response(json.dumps(model_metadata()).encode(), status=200)
                return Response(b"abcdef", "bytes 1-5/6")
            with mock.patch.object(fetch.urllib.request, "urlopen", side_effect=network):
                with self.assertRaises(ValueError):
                    fetch.fetch_model(cache, "easy_15")
            self.assertFalse((cache / "easy_15/.trial-model-manifest.json").exists())

    def test_metadata_revision_mismatch_stops_before_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            metadata = model_metadata()
            metadata["sha"] = "0" * 40
            with mock.patch.object(fetch.urllib.request, "urlopen", return_value=Response(json.dumps(metadata).encode(), status=200)):
                with self.assertRaisesRegex(ValueError, "revision mismatch"):
                    fetch.fetch_model(Path(temporary), "easy_15")
            self.assertEqual(list(Path(temporary).iterdir()), [])


if __name__ == "__main__":
    unittest.main()
