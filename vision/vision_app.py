#!/usr/bin/env python3

import argparse
import errno
import glob
import json
import logging
import os
import re
import select
import signal
import socket
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Mapping
from urllib.parse import quote, urlsplit, urlunsplit


LOGGER = logging.getLogger("smart-guard-vision")
RUNNING = True


def env_text(values: Mapping[str, str], name: str, default: str) -> str:
    value = values.get(name, default).strip()
    return value if value else default


def env_int(
    values: Mapping[str, str],
    name: str,
    default: int,
    minimum: int,
    maximum: int,
) -> int:
    text = values.get(name, str(default)).strip()
    try:
        value = int(text)
    except ValueError as error:
        raise ValueError(f"{name} must be an integer") from error
    if value < minimum or value > maximum:
        raise ValueError(f"{name} must be between {minimum} and {maximum}")
    return value


def env_float(
    values: Mapping[str, str],
    name: str,
    default: float,
    minimum: float,
    maximum: float,
) -> float:
    text = values.get(name, str(default)).strip()
    try:
        value = float(text)
    except ValueError as error:
        raise ValueError(f"{name} must be a number") from error
    if value < minimum or value > maximum:
        raise ValueError(f"{name} must be between {minimum} and {maximum}")
    return value


@dataclass(frozen=True)
class VisionConfig:
    student_id: str
    detector_mode: str
    camera_source: str
    camera_device: str
    camera_backend: str
    camera_width: int
    camera_height: int
    camera_input_fps: int
    camera_input_format: str
    output_fps: float
    jpeg_quality: int
    detection_width: int
    scale_factor: float
    min_neighbors: int
    min_face_size: int
    cascade_path: str
    frame_path: Path
    state_path: Path
    reconnect_delay_sec: float
    onvif_host: str
    onvif_port: int
    onvif_username: str
    onvif_password_file: Path
    onvif_profile_token: str

    @classmethod
    def from_environment(
        cls, values: Mapping[str, str] = os.environ
    ) -> "VisionConfig":
        camera_source = env_text(values, "CAMERA_SOURCE", "v4l2").lower()
        if camera_source not in {"v4l2", "onvif"}:
            raise ValueError("CAMERA_SOURCE must be v4l2 or onvif")

        student_id = env_text(values, "STUDENT_ID", "")
        if not student_id or student_id == "YOUR_STUDENT_ID":
            raise ValueError("STUDENT_ID must contain the real student ID")

        output_fps_default = values.get("CAMERA_WEB_FPS", "2")
        merged = dict(values)
        merged.setdefault("VISION_OUTPUT_FPS", output_fps_default)

        config = cls(
            student_id=student_id,
            detector_mode=env_text(
                values, "VISION_DETECTOR", "face"
            ).lower(),
            camera_source=camera_source,
            camera_device=env_text(values, "CAMERA_DEVICE", "/dev/video0"),
            camera_backend=env_text(
                values, "CAMERA_BACKEND", "auto"
            ).lower(),
            camera_width=env_int(values, "CAMERA_WIDTH", 640, 160, 3840),
            camera_height=env_int(values, "CAMERA_HEIGHT", 480, 120, 2160),
            camera_input_fps=env_int(
                values, "CAMERA_INPUT_FPS", 10, 1, 60
            ),
            camera_input_format=env_text(
                values, "CAMERA_INPUT_FORMAT", "auto"
            ).lower(),
            output_fps=env_float(
                merged, "VISION_OUTPUT_FPS", 2.0, 0.2, 30.0
            ),
            jpeg_quality=env_int(
                values, "VISION_JPEG_QUALITY", 82, 30, 100
            ),
            detection_width=env_int(
                values, "VISION_DETECTION_WIDTH", 320, 160, 1280
            ),
            scale_factor=env_float(
                values, "FACE_SCALE_FACTOR", 1.1, 1.01, 1.8
            ),
            min_neighbors=env_int(
                values, "FACE_MIN_NEIGHBORS", 5, 1, 20
            ),
            min_face_size=env_int(
                values, "FACE_MIN_SIZE", 36, 16, 500
            ),
            cascade_path=env_text(values, "FACE_CASCADE_PATH", "auto"),
            frame_path=Path(
                env_text(
                    values,
                    "LATEST_FRAME_PATH",
                    "/run/smart-guard/latest.jpg",
                )
            ),
            state_path=Path(
                env_text(
                    values,
                    "VISION_STATE_PATH",
                    "/run/smart-guard/vision-state.json",
                )
            ),
            reconnect_delay_sec=env_float(
                values, "CAMERA_RECONNECT_DELAY_SEC", 2.0, 0.2, 30.0
            ),
            onvif_host=env_text(values, "ONVIF_HOST", ""),
            onvif_port=env_int(values, "ONVIF_PORT", 80, 1, 65535),
            onvif_username=env_text(values, "ONVIF_USERNAME", ""),
            onvif_password_file=Path(
                env_text(
                    values,
                    "ONVIF_PASSWORD_FILE",
                    "/etc/smart-guard/onvif-password",
                )
            ),
            onvif_profile_token=env_text(
                values, "ONVIF_PROFILE_TOKEN", ""
            ),
        )
        config.validate()
        return config

    def validate(self) -> None:
        if self.detector_mode not in {"none", "face", "person_hog"}:
            raise ValueError(
                "VISION_DETECTOR must be none, face, or person_hog"
            )
        if self.camera_source == "v4l2":
            if self.camera_device != "auto" and not (
                self.camera_device.startswith("/dev/video")
                or self.camera_device.startswith("/dev/v4l/by-id/")
                or self.camera_device.startswith("/dev/v4l/by-path/")
            ):
                raise ValueError(
                    "CAMERA_DEVICE must be auto or a V4L2 path under /dev"
                )
            if self.camera_backend not in {
                "auto",
                "v4l2",
                "gstreamer",
                "any",
                "ffmpeg",
            }:
                raise ValueError(
                    "CAMERA_BACKEND must be auto, v4l2, gstreamer, "
                    "any, or ffmpeg"
                )
            if self.camera_input_format not in {
                "auto",
                "mjpeg",
                "mjpg",
                "yuyv",
                "yuyv422",
            }:
                raise ValueError(
                    "CAMERA_INPUT_FORMAT must be auto, mjpeg, or yuyv422"
                )
        else:
            if not self.onvif_host:
                raise ValueError("ONVIF_HOST is required in onvif mode")
            if not self.onvif_username:
                raise ValueError("ONVIF_USERNAME is required in onvif mode")
            if not self.onvif_password_file.is_file():
                raise ValueError(
                    f"ONVIF password file is missing: "
                    f"{self.onvif_password_file}"
                )

        if self.frame_path == self.state_path:
            raise ValueError("Frame and state paths must be different")
        for path in (self.frame_path, self.state_path):
            if path.parent != Path("/run/smart-guard"):
                raise ValueError(
                    f"{path.name} must be stored inside /run/smart-guard"
                )


def add_uri_credentials(uri: str, username: str, password: str) -> str:
    parts = urlsplit(uri)
    if parts.scheme.lower() != "rtsp" or not parts.hostname:
        raise ValueError("ONVIF GetStreamUri did not return a valid RTSP URI")

    host = parts.hostname
    if ":" in host and not host.startswith("["):
        host = f"[{host}]"
    if parts.port is not None:
        host = f"{host}:{parts.port}"

    credentials = f"{quote(username, safe='')}:{quote(password, safe='')}@"
    return urlunsplit(
        (parts.scheme, credentials + host, parts.path, parts.query, parts.fragment)
    )


def resolve_onvif_stream_uri(config: VisionConfig) -> str:
    try:
        from onvif import ONVIFCamera
    except ImportError as error:
        raise RuntimeError(
            "ONVIF mode needs the onvif-zeep Python package"
        ) from error

    password = config.onvif_password_file.read_text(
        encoding="utf-8"
    ).strip()
    if not password:
        raise RuntimeError("ONVIF password file is empty")

    camera = ONVIFCamera(
        config.onvif_host,
        config.onvif_port,
        config.onvif_username,
        password,
    )
    media = camera.create_media_service()
    profiles = media.GetProfiles()
    if not profiles:
        raise RuntimeError("The ONVIF camera returned no media profiles")

    selected = None
    if config.onvif_profile_token:
        for profile in profiles:
            if str(profile.token) == config.onvif_profile_token:
                selected = profile
                break
        if selected is None:
            raise RuntimeError("ONVIF_PROFILE_TOKEN was not found")
    else:
        selected = profiles[0]

    request = {
        "StreamSetup": {
            "Stream": "RTP-Unicast",
            "Transport": {"Protocol": "RTSP"},
        },
        "ProfileToken": selected.token,
    }
    response = media.GetStreamUri(request)
    return add_uri_credentials(
        str(response.Uri), config.onvif_username, password
    )


def resolve_cascade_path(cv2, configured_path: str) -> str:
    candidates = []
    if configured_path != "auto":
        candidates.append(configured_path)

    data = getattr(cv2, "data", None)
    if data is not None and getattr(data, "haarcascades", None):
        candidates.append(
            os.path.join(
                data.haarcascades, "haarcascade_frontalface_default.xml"
            )
        )

    candidates.extend(
        [
            "/usr/share/opencv4/haarcascades/"
            "haarcascade_frontalface_default.xml",
            "/usr/share/opencv/haarcascades/"
            "haarcascade_frontalface_default.xml",
        ]
    )
    for candidate in candidates:
        if candidate and os.path.isfile(candidate):
            return candidate
    raise RuntimeError(
        "Haar cascade was not found; install python3-opencv and opencv-data"
    )


class FaceDetector:
    label = "FACE"
    state_name = "opencv_haar_frontal_face"

    def __init__(self, cv2, config: VisionConfig):
        self.cv2 = cv2
        cascade_path = resolve_cascade_path(cv2, config.cascade_path)
        self.classifier = cv2.CascadeClassifier(cascade_path)
        if self.classifier.empty():
            raise RuntimeError(f"Could not load cascade: {cascade_path}")
        self.detection_width = config.detection_width
        self.scale_factor = config.scale_factor
        self.min_neighbors = config.min_neighbors
        self.min_face_size = config.min_face_size
        LOGGER.info("Loaded face cascade: %s", cascade_path)

    def detect(self, frame):
        height, width = frame.shape[:2]
        target_width = min(width, self.detection_width)
        ratio = width / float(target_width)
        target_height = max(1, int(round(height / ratio)))

        if target_width != width:
            small = self.cv2.resize(
                frame,
                (target_width, target_height),
                interpolation=self.cv2.INTER_AREA,
            )
        else:
            small = frame

        gray = self.cv2.cvtColor(small, self.cv2.COLOR_BGR2GRAY)
        gray = self.cv2.equalizeHist(gray)
        minimum = max(12, int(round(self.min_face_size / ratio)))
        faces = self.classifier.detectMultiScale(
            gray,
            scaleFactor=self.scale_factor,
            minNeighbors=self.min_neighbors,
            minSize=(minimum, minimum),
        )

        boxes = []
        for x, y, box_width, box_height in faces:
            boxes.append(
                (
                    int(round(x * ratio)),
                    int(round(y * ratio)),
                    int(round(box_width * ratio)),
                    int(round(box_height * ratio)),
                )
            )
        return boxes


class HogPersonDetector:
    label = "PERSON"
    state_name = "opencv_hog_person"

    def __init__(self, cv2, config: VisionConfig):
        self.cv2 = cv2
        self.detection_width = max(256, config.detection_width)
        self.classifier = cv2.HOGDescriptor()
        self.classifier.setSVMDetector(
            cv2.HOGDescriptor_getDefaultPeopleDetector()
        )
        LOGGER.info("Loaded OpenCV HOG people detector")

    def detect(self, frame):
        height, width = frame.shape[:2]
        target_width = min(width, self.detection_width)
        ratio = width / float(target_width)
        target_height = max(128, int(round(height / ratio)))

        if target_width != width:
            small = self.cv2.resize(
                frame,
                (target_width, target_height),
                interpolation=self.cv2.INTER_AREA,
            )
        else:
            small = frame

        people, weights = self.classifier.detectMultiScale(
            small,
            winStride=(8, 8),
            padding=(8, 8),
            scale=1.05,
        )
        del weights
        boxes = []
        for x, y, box_width, box_height in people:
            boxes.append(
                (
                    int(round(x * ratio)),
                    int(round(y * ratio)),
                    int(round(box_width * ratio)),
                    int(round(box_height * ratio)),
                )
            )
        return boxes


def create_detector(cv2, config: VisionConfig):
    if config.detector_mode == "none":
        return NoopDetector()
    if config.detector_mode == "person_hog":
        return HogPersonDetector(cv2, config)
    return FaceDetector(cv2, config)


class NoopDetector:
    label = "STREAM"
    state_name = "disabled"

    def detect(self, frame):
        return []


class FpsMeter:
    def __init__(self):
        self.previous_time = None
        self.value = 0.0

    def update(self, current_time: float) -> float:
        if self.previous_time is not None:
            elapsed = current_time - self.previous_time
            if elapsed > 0:
                instant = 1.0 / elapsed
                self.value = (
                    instant
                    if self.value == 0.0
                    else 0.2 * instant + 0.8 * self.value
                )
        self.previous_time = current_time
        return self.value


def draw_overlay(
    cv2,
    frame,
    boxes,
    config: VisionConfig,
    fps: float,
    now,
    detection_label: str,
):
    for index, (x, y, width, height) in enumerate(boxes, start=1):
        cv2.rectangle(
            frame, (x, y), (x + width, y + height), (62, 217, 139), 2
        )
        label_y = max(22, y - 7)
        cv2.putText(
            frame,
            f"{detection_label} {index}",
            (x, label_y),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.55,
            (62, 217, 139),
            2,
            cv2.LINE_AA,
        )

    overlay_height = 58
    cv2.rectangle(
        frame, (0, 0), (frame.shape[1], overlay_height), (0, 0, 0), -1
    )
    timestamp = now.strftime("%Y-%m-%d %H:%M:%S")
    cv2.putText(
        frame,
        f"ID: {config.student_id}   {timestamp}",
        (10, 22),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.55,
        (255, 255, 255),
        1,
        cv2.LINE_AA,
    )
    cv2.putText(
        frame,
        f"People: {len(boxes)}   FPS: {fps:.1f}",
        (10, 47),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.62,
        (62, 217, 139),
        2,
        cv2.LINE_AA,
    )
    return frame


def atomic_write_bytes(path: Path, data: bytes, mode: int = 0o640) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", dir=str(path.parent)
    )
    try:
        with os.fdopen(descriptor, "wb") as temporary:
            temporary.write(data)
            temporary.flush()
            os.fsync(temporary.fileno())
        os.chmod(temporary_name, mode)
        os.replace(temporary_name, path)
    except Exception:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass
        raise


def publish_result(
    cv2,
    frame,
    boxes,
    config: VisionConfig,
    fps: float,
    timestamp: datetime,
    detector,
) -> None:
    success, encoded = cv2.imencode(
        ".jpg",
        frame,
        [int(cv2.IMWRITE_JPEG_QUALITY), config.jpeg_quality],
    )
    if not success:
        raise RuntimeError("OpenCV could not encode the output JPEG")

    state = {
        "persons": len(boxes),
        "faces": len(boxes) if detector.label == "FACE" else None,
        "timestamp": timestamp.astimezone().isoformat(timespec="seconds"),
        "fps": round(fps, 2),
        "detector": detector.state_name,
        "camera_source": config.camera_source,
        "frame_width": int(frame.shape[1]),
        "frame_height": int(frame.shape[0]),
    }
    atomic_write_bytes(config.frame_path, encoded.tobytes())
    atomic_write_bytes(
        config.state_path,
        (json.dumps(state, separators=(",", ":")) + "\n").encode("utf-8"),
    )


class SystemdNotifier:
    def __init__(self):
        self.address = os.environ.get("NOTIFY_SOCKET", "")
        self.ready_sent = False

    def send(self, message: str) -> None:
        if not self.address:
            return
        address = self.address
        if address.startswith("@"):
            address = "\0" + address[1:]
        client = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        try:
            client.connect(address)
            client.sendall(message.encode("utf-8"))
        except OSError as error:
            LOGGER.warning("systemd notification failed: %s", error)
        finally:
            client.close()

    def service_started(self) -> None:
        if not self.ready_sent:
            self.send(
                "READY=1\n"
                "STATUS=Vision service started; waiting for a camera frame"
            )
            self.ready_sent = True

    def heartbeat(self, status: str) -> None:
        self.send(f"WATCHDOG=1\nSTATUS={status}")

    def frame_published(self, count: int, fps: float) -> None:
        self.heartbeat(f"Faces: {count}; output FPS: {fps:.1f}")

    def stopping(self) -> None:
        self.send("STOPPING=1\nSTATUS=Vision pipeline is stopping")


def camera_device_candidates(config: VisionConfig):
    if config.camera_device != "auto":
        return [config.camera_device]

    candidates = []
    candidates.extend(sorted(glob.glob("/dev/v4l/by-id/*-video-index0")))
    candidates.extend(sorted(glob.glob("/dev/video[0-9]*")))
    unique = []
    seen = set()
    for candidate in candidates:
        resolved = os.path.realpath(candidate)
        if resolved not in seen:
            seen.add(resolved)
            unique.append(candidate)
    return unique


def video_index_from_path(path: str):
    resolved = os.path.realpath(path)
    match = re.fullmatch(r"/dev/video([0-9]+)", resolved)
    return int(match.group(1)) if match else None


def fourcc_to_text(value) -> str:
    try:
        number = int(value)
    except (TypeError, ValueError):
        return "unknown"
    text = "".join(chr((number >> (8 * index)) & 0xFF) for index in range(4))
    return text if text.isprintable() else "unknown"


def capture_backend_name(capture) -> str:
    try:
        return str(capture.getBackendName())
    except (AttributeError, TypeError, RuntimeError):
        return "unknown"


def configure_capture(cv2, capture, config: VisionConfig, fourcc_text) -> None:
    if fourcc_text:
        capture.set(
            cv2.CAP_PROP_FOURCC,
            cv2.VideoWriter_fourcc(*fourcc_text),
        )
    capture.set(cv2.CAP_PROP_FRAME_WIDTH, config.camera_width)
    capture.set(cv2.CAP_PROP_FRAME_HEIGHT, config.camera_height)
    capture.set(cv2.CAP_PROP_FPS, config.camera_input_fps)
    capture.set(cv2.CAP_PROP_BUFFERSIZE, 1)


def log_capture_details(cv2, capture, description: str) -> None:
    LOGGER.info(
        "Camera ready via %s: backend=%s format=%s size=%dx%d fps=%.2f",
        description,
        capture_backend_name(capture),
        fourcc_to_text(capture.get(cv2.CAP_PROP_FOURCC)),
        int(capture.get(cv2.CAP_PROP_FRAME_WIDTH)),
        int(capture.get(cv2.CAP_PROP_FRAME_HEIGHT)),
        float(capture.get(cv2.CAP_PROP_FPS)),
    )


def format_candidates(config: VisionConfig):
    formats = {
        "mjpeg": "MJPG",
        "mjpg": "MJPG",
        "yuyv": "YUYV",
        "yuyv422": "YUYV",
    }
    if config.camera_input_format in formats:
        return [formats[config.camera_input_format]]
    if config.camera_input_format != "auto":
        raise ValueError(
            "CAMERA_INPUT_FORMAT must be auto, mjpeg, or yuyv422"
        )
    return ["MJPG", "YUYV", None]


def preflight_v4l2_device(device: str) -> bool:
    flags = os.O_RDWR | getattr(os, "O_NONBLOCK", 0)
    try:
        descriptor = os.open(device, flags)
    except OSError as error:
        errno_name = errno.errorcode.get(error.errno, "UNKNOWN")
        LOGGER.error(
            "V4L2 access preflight failed for %s: errno=%s (%s): %s",
            device,
            error.errno,
            errno_name,
            error.strerror,
        )
        return False
    else:
        os.close(descriptor)
        LOGGER.info("V4L2 read/write access preflight passed for %s", device)
        return True


def opencv_capture_attempts(cv2, device: str, config: VisionConfig):
    index = video_index_from_path(device)
    sources = [(device, f"path {device}")]
    if index is not None:
        sources.append((index, f"index {index}"))

    if config.camera_backend == "v4l2":
        backends = [(cv2.CAP_V4L2, "V4L2")]
    elif config.camera_backend == "any":
        backends = [(cv2.CAP_ANY, "AUTO")]
    else:
        backends = [
            (cv2.CAP_V4L2, "V4L2"),
            (cv2.CAP_ANY, "AUTO"),
        ]

    for source, source_name in sources:
        for backend, backend_name in backends:
            for pixel_format in format_candidates(config):
                label = (
                    f"OpenCV {backend_name}, {source_name}, "
                    f"format={pixel_format or 'driver-default'}"
                )
                yield source, backend, pixel_format, label


def gstreamer_pipelines(device: str, config: VisionConfig):
    if config.camera_backend not in {"auto", "gstreamer"}:
        return []

    pipelines = []
    for pixel_format in format_candidates(config):
        if pixel_format == "MJPG":
            media = (
                "image/jpeg,"
                f"width={config.camera_width},"
                f"height={config.camera_height},"
                f"framerate={config.camera_input_fps}/1 ! "
                "jpegdec ! "
            )
        elif pixel_format == "YUYV":
            media = (
                "video/x-raw,format=YUY2,"
                f"width={config.camera_width},"
                f"height={config.camera_height},"
                f"framerate={config.camera_input_fps}/1 ! "
            )
        else:
            media = "decodebin ! "

        pipeline = (
            f"v4l2src device={device} ! {media}"
            "videoconvert ! video/x-raw,format=BGR ! "
            "appsink drop=true max-buffers=1 sync=false"
        )
        label = f"OpenCV GStreamer, {device}, format={pixel_format or 'auto'}"
        pipelines.append((pipeline, label))
    return pipelines


class FfmpegCapture:
    def __init__(
        self,
        process,
        width: int,
        height: int,
        fps: int,
        read_timeout: float = 5.0,
    ):
        self.process = process
        self.width = width
        self.height = height
        self.fps = fps
        self.read_timeout = read_timeout
        self.frame_size = width * height * 3

    def isOpened(self) -> bool:
        return self.process is not None and self.process.poll() is None

    def _read_exact(self, size: int):
        if not self.isOpened() or self.process.stdout is None:
            return None
        descriptor = self.process.stdout.fileno()
        deadline = time.monotonic() + self.read_timeout
        chunks = []
        received = 0
        while received < size:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            ready, _, _ = select.select([descriptor], [], [], remaining)
            if not ready:
                return None
            chunk = os.read(descriptor, size - received)
            if not chunk:
                return None
            chunks.append(chunk)
            received += len(chunk)
        return b"".join(chunks)

    def read(self):
        data = self._read_exact(self.frame_size)
        if data is None:
            return False, None
        try:
            import numpy
        except ImportError:
            return False, None
        frame = numpy.frombuffer(data, dtype=numpy.uint8)
        return True, frame.reshape((self.height, self.width, 3))

    def getBackendName(self) -> str:
        return "FFMPEG-CLI"

    def get(self, property_id):
        properties = {
            3: self.width,
            4: self.height,
            5: self.fps,
            6: sum(
                ord(character) << (8 * index)
                for index, character in enumerate("BGR3")
            ),
        }
        return properties.get(property_id, 0)

    def release(self) -> None:
        if self.process is None:
            return
        process = self.process
        self.process = None
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
        if process.stdout is not None:
            process.stdout.close()
        if process.stderr is not None:
            process.stderr.close()

    def error_text(self) -> str:
        if self.process is None or self.process.stderr is None:
            return ""
        descriptor = self.process.stderr.fileno()
        ready, _, _ = select.select([descriptor], [], [], 0)
        if not ready:
            return ""
        try:
            data = os.read(descriptor, 8192)
        except OSError:
            return ""
        return data.decode("utf-8", errors="replace").strip()


def ffmpeg_capture_attempts(device: str, config: VisionConfig):
    if config.camera_backend not in {"auto", "ffmpeg"}:
        return []
    attempts = []
    input_formats = {
        "MJPG": "mjpeg",
        "YUYV": "yuyv422",
        None: None,
    }
    for pixel_format in format_candidates(config):
        command = [
            "/usr/bin/ffmpeg",
            "-hide_banner",
            "-nostdin",
            "-loglevel",
            "error",
            "-f",
            "video4linux2",
        ]
        input_format = input_formats[pixel_format]
        if input_format:
            command.extend(["-input_format", input_format])
        command.extend(
            [
                "-framerate",
                str(config.camera_input_fps),
                "-video_size",
                f"{config.camera_width}x{config.camera_height}",
                "-i",
                device,
                "-an",
                "-c:v",
                "rawvideo",
                "-pix_fmt",
                "bgr24",
                "-f",
                "rawvideo",
                "pipe:1",
            ]
        )
        label = (
            f"FFmpeg V4L2, {device}, "
            f"format={pixel_format or 'driver-default'}"
        )
        attempts.append((command, label))
    return attempts


def open_ffmpeg_capture(cv2, device: str, config: VisionConfig):
    if not os.path.isfile("/usr/bin/ffmpeg"):
        if config.camera_backend == "ffmpeg":
            LOGGER.error(
                "CAMERA_BACKEND=ffmpeg requires /usr/bin/ffmpeg"
            )
        return None, None

    for command, label in ffmpeg_capture_attempts(device, config):
        capture = None
        try:
            process = subprocess.Popen(
                command,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                bufsize=0,
            )
            capture = FfmpegCapture(
                process,
                config.camera_width,
                config.camera_height,
                config.camera_input_fps,
            )
            success, frame = capture.read()
            if not success or frame is None:
                error_text = capture.error_text()
                if error_text:
                    LOGGER.warning("%s failed: %s", label, error_text)
                else:
                    LOGGER.warning(
                        "%s failed: no complete frame within timeout",
                        label,
                    )
                capture.release()
                continue
            log_capture_details(cv2, capture, label)
            return capture, frame
        except (OSError, RuntimeError) as error:
            LOGGER.warning("%s failed: %s", label, error)
            if capture is not None:
                capture.release()
    return None, None


def open_v4l2_capture(cv2, config: VisionConfig):
    devices = camera_device_candidates(config)
    if not devices:
        LOGGER.error(
            "No V4L2 capture candidates were found for CAMERA_DEVICE=%s",
            config.camera_device,
        )
        return None, None

    for device in devices:
        LOGGER.info("Trying camera device %s", device)
        if not preflight_v4l2_device(device):
            continue

        if config.camera_backend not in {"gstreamer", "ffmpeg"}:
            for source, backend, pixel_format, label in opencv_capture_attempts(
                cv2, device, config
            ):
                capture = cv2.VideoCapture()
                keep_capture = False
                try:
                    opened = capture.open(source, backend)
                    if not opened or not capture.isOpened():
                        LOGGER.warning("Open failed: %s", label)
                        continue
                    configure_capture(
                        cv2, capture, config, pixel_format
                    )
                    success, frame = capture.read()
                    if not success or frame is None:
                        LOGGER.warning("First frame failed: %s", label)
                        continue
                    log_capture_details(cv2, capture, label)
                    keep_capture = True
                    return capture, frame
                except (cv2.error, OSError, RuntimeError) as error:
                    LOGGER.warning("%s failed: %s", label, error)
                finally:
                    if not keep_capture:
                        capture.release()

        for pipeline, label in gstreamer_pipelines(device, config):
            capture = cv2.VideoCapture()
            keep_capture = False
            try:
                opened = capture.open(pipeline, cv2.CAP_GSTREAMER)
                if not opened or not capture.isOpened():
                    LOGGER.warning("Open failed: %s", label)
                    continue
                success, frame = capture.read()
                if not success or frame is None:
                    LOGGER.warning("First frame failed: %s", label)
                    continue
                log_capture_details(cv2, capture, label)
                keep_capture = True
                return capture, frame
            except (cv2.error, OSError, RuntimeError) as error:
                LOGGER.warning("%s failed: %s", label, error)
            finally:
                if not keep_capture:
                    capture.release()

        capture, frame = open_ffmpeg_capture(cv2, device, config)
        if capture is not None:
            return capture, frame

    return None, None


def open_capture(cv2, config: VisionConfig):
    if config.camera_source == "v4l2":
        return open_v4l2_capture(cv2, config)

    LOGGER.info(
        "Resolving ONVIF media profile on %s:%d",
        config.onvif_host,
        config.onvif_port,
    )
    uri = resolve_onvif_stream_uri(config)
    capture = cv2.VideoCapture(uri, cv2.CAP_FFMPEG)
    capture.set(cv2.CAP_PROP_BUFFERSIZE, 1)
    success, frame = capture.read() if capture.isOpened() else (False, None)
    if not success or frame is None:
        capture.release()
        return None, None
    return capture, frame


def remove_runtime_outputs(config: VisionConfig) -> None:
    for path in (config.frame_path, config.state_path):
        try:
            path.unlink()
        except FileNotFoundError:
            pass


def signal_handler(signum, frame) -> None:
    del signum, frame
    global RUNNING
    RUNNING = False


def run_pipeline(cv2, config: VisionConfig) -> int:
    detector = create_detector(cv2, config)
    notifier = SystemdNotifier()
    notifier.service_started()
    fps_meter = FpsMeter()
    output_interval = 1.0 / config.output_fps
    next_output = 0.0
    remove_runtime_outputs(config)

    while RUNNING:
        capture, first_frame = open_capture(cv2, config)
        if capture is None:
            LOGGER.error(
                "All camera opening methods failed; retrying in %.1fs",
                config.reconnect_delay_sec,
            )
            notifier.heartbeat("Camera unavailable; retrying")
            time.sleep(config.reconnect_delay_sec)
            continue

        frame = first_frame
        while RUNNING:
            if frame is None:
                success, frame = capture.read()
                if not success or frame is None:
                    LOGGER.error("Camera frame read failed; reopening")
                    notifier.heartbeat("Camera frame failed; reopening")
                    break

            monotonic_now = time.monotonic()
            if monotonic_now < next_output:
                frame = None
                continue
            next_output = monotonic_now + output_interval

            boxes = detector.detect(frame)
            fps = fps_meter.update(monotonic_now)
            timestamp = datetime.now().astimezone()
            annotated = draw_overlay(
                cv2,
                frame,
                boxes,
                config,
                fps,
                timestamp,
                detector.label,
            )
            publish_result(
                cv2,
                annotated,
                boxes,
                config,
                fps,
                timestamp,
                detector,
            )
            notifier.frame_published(len(boxes), fps)
            frame = None

        capture.release()
        if RUNNING:
            time.sleep(config.reconnect_delay_sec)

    notifier.stopping()
    return 0


def run_self_test(cv2, config: VisionConfig) -> int:
    try:
        import numpy
    except ImportError as error:
        raise RuntimeError("numpy is required") from error

    detector = create_detector(cv2, config)
    frame = numpy.zeros(
        (config.camera_height, config.camera_width, 3), dtype=numpy.uint8
    )
    boxes = detector.detect(frame)
    now = datetime.now().astimezone()
    annotated = draw_overlay(
        cv2, frame, boxes, config, 2.0, now, detector.label
    )
    success, encoded = cv2.imencode(".jpg", annotated)
    if not success or len(encoded) < 100:
        raise RuntimeError("OpenCV self-test could not encode a JPEG")
    print("OpenCV vision detector self-test passed")
    return 0


def probe_camera(cv2, config: VisionConfig) -> int:
    capture, frame = open_capture(cv2, config)
    if capture is None or frame is None:
        LOGGER.error("Camera probe failed")
        return 1
    print(
        "Camera probe passed: "
        f"{frame.shape[1]}x{frame.shape[0]} "
        f"via {capture_backend_name(capture)}"
    )
    capture.release()
    return 0


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Smart Guard OpenCV face-detection service"
    )
    parser.add_argument(
        "--check-config",
        action="store_true",
        help="validate configuration and the OpenCV cascade",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run the detector and encoder on a synthetic frame",
    )
    parser.add_argument(
        "--probe-camera",
        action="store_true",
        help="try every configured camera method and read one real frame",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    logging.basicConfig(
        level=os.environ.get("LOG_LEVEL", "INFO").upper(),
        format="%(asctime)s %(levelname)s %(message)s",
    )

    try:
        import cv2
    except ImportError:
        LOGGER.error(
            "OpenCV is missing; install python3-opencv and opencv-data"
        )
        return 1

    try:
        config = VisionConfig.from_environment()
        if config.detector_mode == "face":
            resolve_cascade_path(cv2, config.cascade_path)
        if config.camera_source == "onvif":
            try:
                import onvif
                del onvif
            except ImportError as error:
                raise RuntimeError(
                    "ONVIF mode needs the onvif-zeep Python package"
                ) from error

        if arguments.check_config:
            print("Vision configuration is valid")
            return 0
        if arguments.self_test:
            return run_self_test(cv2, config)
        if arguments.probe_camera:
            return probe_camera(cv2, config)

        signal.signal(signal.SIGTERM, signal_handler)
        signal.signal(signal.SIGINT, signal_handler)
        return run_pipeline(cv2, config)
    except (OSError, RuntimeError, ValueError) as error:
        LOGGER.error("%s", error)
        return 1


if __name__ == "__main__":
    sys.exit(main())
