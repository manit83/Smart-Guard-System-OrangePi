#!/usr/bin/env python3

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace

import numpy


MODULE_PATH = (
    Path(__file__).resolve().parents[1] / "vision" / "vision_app.py"
)
SPEC = importlib.util.spec_from_file_location("vision_app", MODULE_PATH)
VISION_APP = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VISION_APP)


class VisionConfigTests(unittest.TestCase):
    def base_environment(self):
        return {
            "STUDENT_ID": "402000000",
            "CAMERA_SOURCE": "v4l2",
            "CAMERA_DEVICE": "/dev/video0",
            "LATEST_FRAME_PATH": "/run/smart-guard/latest.jpg",
            "VISION_STATE_PATH": "/run/smart-guard/vision-state.json",
        }

    def test_default_usb_config(self):
        config = VISION_APP.VisionConfig.from_environment(
            self.base_environment()
        )
        self.assertEqual(config.camera_source, "v4l2")
        self.assertEqual(config.camera_backend, "auto")
        self.assertEqual(config.detector_mode, "face")
        self.assertEqual(config.camera_width, 640)
        self.assertEqual(config.output_fps, 2.0)
        self.assertEqual(config.detection_width, 320)

    def test_auto_and_stable_v4l2_device_paths_are_valid(self):
        for device in (
            "auto",
            "/dev/v4l/by-id/usb-camera-video-index0",
            "/dev/v4l/by-path/platform-usb-video-index0",
        ):
            values = self.base_environment()
            values["CAMERA_DEVICE"] = device
            config = VISION_APP.VisionConfig.from_environment(values)
            self.assertEqual(config.camera_device, device)

    def test_invalid_camera_backend(self):
        values = self.base_environment()
        values["CAMERA_BACKEND"] = "invalid"
        with self.assertRaisesRegex(ValueError, "CAMERA_BACKEND"):
            VISION_APP.VisionConfig.from_environment(values)

    def test_ffmpeg_camera_backend_is_valid(self):
        values = self.base_environment()
        values["CAMERA_BACKEND"] = "ffmpeg"
        config = VISION_APP.VisionConfig.from_environment(values)
        self.assertEqual(config.camera_backend, "ffmpeg")

    def test_invalid_camera_input_format(self):
        values = self.base_environment()
        values["CAMERA_INPUT_FORMAT"] = "h264"
        with self.assertRaisesRegex(ValueError, "CAMERA_INPUT_FORMAT"):
            VISION_APP.VisionConfig.from_environment(values)

    def test_invalid_camera_source(self):
        values = self.base_environment()
        values["CAMERA_SOURCE"] = "unknown"
        with self.assertRaisesRegex(ValueError, "CAMERA_SOURCE"):
            VISION_APP.VisionConfig.from_environment(values)

    def test_invalid_detector(self):
        values = self.base_environment()
        values["VISION_DETECTOR"] = "recognition"
        with self.assertRaisesRegex(ValueError, "VISION_DETECTOR"):
            VISION_APP.VisionConfig.from_environment(values)

    def test_stream_only_detector(self):
        values = self.base_environment()
        values["VISION_DETECTOR"] = "none"
        config = VISION_APP.VisionConfig.from_environment(values)
        detector = VISION_APP.create_detector(None, config)
        self.assertEqual(detector.detect(None), [])
        self.assertEqual(detector.state_name, "disabled")

    def test_runtime_paths_are_restricted(self):
        values = self.base_environment()
        values["LATEST_FRAME_PATH"] = "/tmp/latest.jpg"
        with self.assertRaisesRegex(ValueError, "/run/smart-guard"):
            VISION_APP.VisionConfig.from_environment(values)


class OutputTests(unittest.TestCase):
    def test_atomic_byte_write_replaces_file(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            VISION_APP.atomic_write_bytes(path, b'{"persons":1}\n')
            VISION_APP.atomic_write_bytes(path, b'{"persons":2}\n')
            state = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(state["persons"], 2)

    def test_onvif_credentials_are_url_encoded(self):
        uri = VISION_APP.add_uri_credentials(
            "rtsp://192.0.2.20:554/live",
            "camera user",
            "p@ss/word",
        )
        self.assertEqual(
            uri,
            "rtsp://camera%20user:p%40ss%2Fword@192.0.2.20:554/live",
        )

    def test_publish_result_writes_jpeg_and_state(self):
        class FakeCv2:
            IMWRITE_JPEG_QUALITY = 1

            @staticmethod
            def imencode(extension, frame, parameters):
                self_check = extension == ".jpg" and frame.shape == (8, 8, 3)
                self_check = self_check and parameters == [1, 82]
                encoded = numpy.frombuffer(b"\xff\xd8test\xff\xd9", dtype="uint8")
                return self_check, encoded

        environment = {
            "STUDENT_ID": "402000000",
            "CAMERA_SOURCE": "v4l2",
            "CAMERA_DEVICE": "/dev/video0",
        }
        config = VISION_APP.VisionConfig.from_environment(environment)
        detector = SimpleNamespace(
            label="FACE", state_name="opencv_haar_frontal_face"
        )

        with tempfile.TemporaryDirectory() as directory:
            config = replace(
                config,
                frame_path=Path(directory) / "latest.jpg",
                state_path=Path(directory) / "vision-state.json",
            )
            frame = numpy.zeros((8, 8, 3), dtype="uint8")
            timestamp = VISION_APP.datetime.now().astimezone()
            VISION_APP.publish_result(
                FakeCv2,
                frame,
                [(1, 1, 3, 3)],
                config,
                1.75,
                timestamp,
                detector,
            )

            self.assertEqual(
                config.frame_path.read_bytes(), b"\xff\xd8test\xff\xd9"
            )
            state = json.loads(
                config.state_path.read_text(encoding="utf-8")
            )
            self.assertEqual(state["persons"], 1)
            self.assertEqual(state["faces"], 1)
            self.assertEqual(state["fps"], 1.75)
            self.assertEqual(
                state["detector"], "opencv_haar_frontal_face"
            )


class CameraOpeningTests(unittest.TestCase):
    class FakeCapture:
        def __init__(self, cv2):
            self.cv2 = cv2
            self.opened = False
            self.properties = {}

        def open(self, source, backend):
            self.opened = source == 0 and backend == self.cv2.CAP_V4L2
            return self.opened

        def isOpened(self):
            return self.opened

        def set(self, key, value):
            self.properties[key] = value
            return True

        def get(self, key):
            defaults = {
                self.cv2.CAP_PROP_FRAME_WIDTH: 640,
                self.cv2.CAP_PROP_FRAME_HEIGHT: 480,
                self.cv2.CAP_PROP_FPS: 10,
                self.cv2.CAP_PROP_FOURCC: self.cv2.VideoWriter_fourcc(
                    *"MJPG"
                ),
            }
            return self.properties.get(key, defaults.get(key, 0))

        def getBackendName(self):
            return "V4L2"

        def read(self):
            if not self.opened:
                return False, None
            return True, numpy.zeros((480, 640, 3), dtype=numpy.uint8)

        def release(self):
            self.opened = False

    class FakeCv2:
        CAP_ANY = 0
        CAP_V4L2 = 200
        CAP_GSTREAMER = 1800
        CAP_PROP_FRAME_WIDTH = 3
        CAP_PROP_FRAME_HEIGHT = 4
        CAP_PROP_FPS = 5
        CAP_PROP_FOURCC = 6
        CAP_PROP_BUFFERSIZE = 38
        error = RuntimeError

        def __init__(self):
            self.captures = []

        @staticmethod
        def VideoWriter_fourcc(*characters):
            return sum(
                ord(character) << (8 * index)
                for index, character in enumerate(characters)
            )

        def VideoCapture(self):
            capture = CameraOpeningTests.FakeCapture(self)
            self.captures.append(capture)
            return capture

    def test_path_failure_falls_back_to_numeric_v4l2_index(self):
        environment = {
            "STUDENT_ID": "402000000",
            "CAMERA_SOURCE": "v4l2",
            "CAMERA_DEVICE": "/dev/video0",
            "CAMERA_BACKEND": "auto",
        }
        config = VISION_APP.VisionConfig.from_environment(environment)
        fake_cv2 = self.FakeCv2()

        with mock.patch.object(
            VISION_APP, "preflight_v4l2_device", return_value=True
        ):
            capture, frame = VISION_APP.open_v4l2_capture(fake_cv2, config)

        self.assertIsNotNone(capture)
        self.assertEqual(frame.shape, (480, 640, 3))
        self.assertTrue(
            any(
                item.opened
                for item in fake_cv2.captures
            )
        )
        capture.release()

    def test_failed_access_preflight_skips_capture_attempts(self):
        environment = {
            "STUDENT_ID": "402000000",
            "CAMERA_SOURCE": "v4l2",
            "CAMERA_DEVICE": "/dev/video0",
            "CAMERA_BACKEND": "auto",
        }
        config = VISION_APP.VisionConfig.from_environment(environment)
        fake_cv2 = self.FakeCv2()

        with mock.patch.object(
            VISION_APP, "preflight_v4l2_device", return_value=False
        ):
            capture, frame = VISION_APP.open_v4l2_capture(fake_cv2, config)

        self.assertIsNone(capture)
        self.assertIsNone(frame)
        self.assertEqual(fake_cv2.captures, [])

    def test_ffmpeg_attempt_uses_v4l2_and_raw_bgr_output(self):
        environment = {
            "STUDENT_ID": "402000000",
            "CAMERA_SOURCE": "v4l2",
            "CAMERA_DEVICE": "/dev/video0",
            "CAMERA_BACKEND": "ffmpeg",
            "CAMERA_INPUT_FORMAT": "mjpeg",
        }
        config = VISION_APP.VisionConfig.from_environment(environment)

        attempts = VISION_APP.ffmpeg_capture_attempts(
            "/dev/video0", config
        )

        self.assertEqual(len(attempts), 1)
        command, label = attempts[0]
        self.assertIn("video4linux2", command)
        self.assertIn("mjpeg", command)
        self.assertIn("bgr24", command)
        self.assertEqual(command[-1], "pipe:1")
        self.assertIn("FFmpeg V4L2", label)

    def test_ffmpeg_capture_reads_one_complete_bgr_frame(self):
        frame_bytes = bytes(range(12))
        process = subprocess.Popen(
            [
                sys.executable,
                "-c",
                (
                    "import sys; "
                    f"sys.stdout.buffer.write({frame_bytes!r}); "
                    "sys.stdout.buffer.flush()"
                ),
            ],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
        )
        capture = VISION_APP.FfmpegCapture(
            process,
            width=2,
            height=2,
            fps=10,
            read_timeout=1,
        )

        success, frame = capture.read()

        self.assertTrue(success)
        self.assertEqual(frame.shape, (2, 2, 3))
        self.assertEqual(frame.tobytes(), frame_bytes)
        self.assertEqual(capture.getBackendName(), "FFMPEG-CLI")
        capture.release()


class SystemdNotifierTests(unittest.TestCase):
    def test_ready_is_sent_before_the_first_frame(self):
        notifier = VISION_APP.SystemdNotifier()
        messages = []
        notifier.send = messages.append

        notifier.service_started()
        notifier.service_started()
        notifier.heartbeat("Camera unavailable; retrying")

        self.assertEqual(
            sum("READY=1" in message for message in messages),
            1,
        )
        self.assertIn("waiting for a camera frame", messages[0])
        self.assertIn("WATCHDOG=1", messages[1])


if __name__ == "__main__":
    unittest.main()
