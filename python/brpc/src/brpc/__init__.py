"""brpc — High-performance bRPC bindings for Python (ctypes-based)."""
import ctypes
import os

__all__ = ["JsonParser", "JsonValue", "JsonWriter", "Stream", "Channel", "Profiler"]
__version__ = "0.1.0"

_lib_path = os.path.join(os.path.dirname(__file__), "_libbrpc.so")
lib = ctypes.CDLL(_lib_path)

class json_arena_t(ctypes.Structure):
    _fields_ = [
        ("base", ctypes.POINTER(ctypes.c_uint8)),
        ("capacity", ctypes.c_size_t),
        ("offset", ctypes.c_size_t),
    ]

class json_parser_t(ctypes.Structure):
    _fields_ = [
        ("input", ctypes.c_char_p),
        ("input_len", ctypes.c_size_t),
        ("pos", ctypes.c_size_t),
        ("arena", ctypes.POINTER(json_arena_t)),
        ("error", ctypes.c_char * 256),
        ("error_code", ctypes.c_int),
    ]

class json_value_t(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int)]

class json_writer_t(ctypes.Structure):
    _fields_ = [
        ("buf", ctypes.c_char_p),
        ("capacity", ctypes.c_size_t),
        ("len", ctypes.c_size_t),
        ("error", ctypes.c_int),
        ("need_comma_mask", ctypes.c_uint),
        ("depth", ctypes.c_int),
    ]

class brpc_stream_t(ctypes.Structure):
    _fields_ = [
        ("stream_id", ctypes.c_uint32),
        ("state", ctypes.c_int),
    ]

# ── Function signatures ──────────────────────────────────────────────────

lib.json_arena_init.argtypes = [ctypes.POINTER(json_arena_t), ctypes.c_void_p, ctypes.c_size_t]
lib.json_arena_init.restype = None
lib.json_arena_reset.argtypes = [ctypes.POINTER(json_arena_t)]
lib.json_arena_reset.restype = None
lib.json_parse.argtypes = [ctypes.POINTER(json_parser_t), ctypes.c_char_p, ctypes.c_size_t,
                           ctypes.POINTER(json_arena_t), ctypes.POINTER(ctypes.c_void_p)]
lib.json_parse.restype = ctypes.c_int
lib.json_obj_get.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
lib.json_obj_get.restype = ctypes.c_void_p
lib.json_get_int.argtypes = [ctypes.c_void_p, ctypes.c_int64]
lib.json_get_int.restype = ctypes.c_int64
lib.json_get_float.argtypes = [ctypes.c_void_p, ctypes.c_double]
lib.json_get_float.restype = ctypes.c_double
lib.json_get_str.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t)]
lib.json_get_str.restype = ctypes.c_void_p
lib.json_get_bool.argtypes = [ctypes.c_void_p, ctypes.c_int]
lib.json_get_bool.restype = ctypes.c_int
lib.json_array_len.argtypes = [ctypes.c_void_p]
lib.json_array_len.restype = ctypes.c_size_t
lib.json_array_get.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
lib.json_array_get.restype = ctypes.c_void_p
lib.json_writer_init.argtypes = [ctypes.POINTER(json_writer_t), ctypes.c_char_p, ctypes.c_size_t]
lib.json_writer_init.restype = None
lib.json_writer_finish.argtypes = [ctypes.POINTER(json_writer_t)]
lib.json_writer_finish.restype = ctypes.c_size_t
lib.json_write_null.argtypes = [ctypes.POINTER(json_writer_t)]
lib.json_write_null.restype = None
lib.json_write_bool.argtypes = [ctypes.POINTER(json_writer_t), ctypes.c_int]
lib.json_write_bool.restype = None
lib.json_write_int.argtypes = [ctypes.POINTER(json_writer_t), ctypes.c_int64]
lib.json_write_int.restype = None
lib.json_write_float.argtypes = [ctypes.POINTER(json_writer_t), ctypes.c_double]
lib.json_write_float.restype = None
lib.json_write_str.argtypes = [ctypes.POINTER(json_writer_t), ctypes.c_char_p, ctypes.c_size_t]
lib.json_write_str.restype = None
lib.json_write_obj_start.argtypes = [ctypes.POINTER(json_writer_t)]
lib.json_write_obj_start.restype = None
lib.json_write_obj_key.argtypes = [ctypes.POINTER(json_writer_t), ctypes.c_char_p, ctypes.c_size_t]
lib.json_write_obj_key.restype = None
lib.json_write_obj_end.argtypes = [ctypes.POINTER(json_writer_t)]
lib.json_write_obj_end.restype = None
lib.json_write_arr_start.argtypes = [ctypes.POINTER(json_writer_t)]
lib.json_write_arr_start.restype = None
lib.json_write_arr_end.argtypes = [ctypes.POINTER(json_writer_t)]
lib.json_write_arr_end.restype = None
lib.json_serialize.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t,
                               ctypes.POINTER(ctypes.c_size_t)]
lib.json_serialize.restype = ctypes.c_int

lib.brpc_stream_init.argtypes = [ctypes.POINTER(brpc_stream_t), ctypes.c_uint32, ctypes.c_size_t]
lib.brpc_stream_init.restype = ctypes.c_int
lib.brpc_stream_destroy.argtypes = [ctypes.POINTER(brpc_stream_t)]
lib.brpc_stream_destroy.restype = None
lib.brpc_stream_write.argtypes = [ctypes.POINTER(brpc_stream_t), ctypes.c_char_p, ctypes.c_size_t]
lib.brpc_stream_write.restype = ctypes.c_int
lib.brpc_stream_read.argtypes = [ctypes.POINTER(brpc_stream_t), ctypes.c_char_p, ctypes.c_size_t]
lib.brpc_stream_read.restype = ctypes.c_int
lib.brpc_stream_close.argtypes = [ctypes.POINTER(brpc_stream_t)]
lib.brpc_stream_close.restype = None
lib.brpc_stream_available_read.argtypes = [ctypes.POINTER(brpc_stream_t)]
lib.brpc_stream_available_read.restype = ctypes.c_size_t
lib.brpc_stream_available_write.argtypes = [ctypes.POINTER(brpc_stream_t)]
lib.brpc_stream_available_write.restype = ctypes.c_size_t

CHANNEL_SIZE = 88  # sizeof(brpc_channel_t) with dynamic stream table
STREAM_COUNT_OFFSET = 20  # offsetof(brpc_channel_t, stream_count)
lib.brpc_channel_init.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_uint32]
lib.brpc_channel_init.restype = ctypes.c_int
lib.brpc_channel_destroy.argtypes = [ctypes.c_void_p]
lib.brpc_channel_destroy.restype = None
lib.brpc_channel_open_stream.argtypes = [ctypes.c_void_p]
lib.brpc_channel_open_stream.restype = ctypes.c_void_p
lib.brpc_channel_send_data.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                       ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int]
lib.brpc_channel_send_data.restype = ctypes.c_int
lib.brpc_channel_send_ping.argtypes = [ctypes.c_void_p]
lib.brpc_channel_send_ping.restype = ctypes.c_int
lib.brpc_channel_close.argtypes = [ctypes.c_void_p]
lib.brpc_channel_close.restype = ctypes.c_int
lib.brpc_channel_recv.argtypes = [ctypes.c_void_p]
lib.brpc_channel_recv.restype = ctypes.c_int
lib.brpc_channel_pump.argtypes = [ctypes.c_void_p]
lib.brpc_channel_pump.restype = ctypes.c_int

lib.brpc_prof_init.argtypes = []
lib.brpc_prof_init.restype = None
lib.brpc_prof_reset.argtypes = []
lib.brpc_prof_reset.restype = None
lib.brpc_prof_print.argtypes = []
lib.brpc_prof_print.restype = None


class JsonParser:
    def __init__(self, arena_size: int = 65536):
        self._arena_buf = (ctypes.c_uint8 * arena_size)()
        self._arena = (json_arena_t * 1)()
        lib.json_arena_init(self._arena, self._arena_buf, arena_size)
        self._parser = (json_parser_t * 1)()
        self._input_ref = None  # Prevent GC of input buffer (zero-copy strings)

    def parse(self, data: str | bytes) -> "JsonValue":
        if isinstance(data, str):
            data = data.encode("utf-8")
        self._input_ref = data  # Keep alive for zero-copy string pointers
        lib.json_arena_reset(self._arena)
        root = ctypes.c_void_p()
        rc = lib.json_parse(self._parser, data, len(data),
                            self._arena, ctypes.byref(root))
        if rc != 0:
            raise ValueError(f"JSON parse error: {self._parser[0].error.decode()}")
        return JsonValue(root)

    def reset(self):
        lib.json_arena_reset(self._arena)


class JsonValue:
    def __init__(self, v):
        self._v = v if isinstance(v, int) else v.value

    @property
    def type(self) -> str:
        t = json_value_t.from_address(self._v).type
        return {0: "null", 1: "bool", 2: "int", 3: "float",
                4: "string", 5: "array", 6: "object"}.get(t, "unknown")

    def as_int(self, fallback: int = 0) -> int:
        return lib.json_get_int(self._v, fallback)

    def as_float(self, fallback: float = 0.0) -> float:
        return lib.json_get_float(self._v, fallback)

    def as_str(self) -> str:
        slen = ctypes.c_size_t()
        ptr = lib.json_get_str(self._v, ctypes.byref(slen))
        if not ptr:
            return ""
        return ctypes.string_at(ptr, slen.value).decode("utf-8")

    def as_bool(self, fallback: bool = False) -> bool:
        return bool(lib.json_get_bool(self._v, int(fallback)))

    def __len__(self) -> int:
        return lib.json_array_len(self._v)

    def __getitem__(self, key):
        if isinstance(key, int):
            item = lib.json_array_get(self._v, key)
            if not item:
                raise IndexError(f"Index {key} out of range")
            return JsonValue(item)
        item = lib.json_obj_get(self._v, key.encode("utf-8"))
        if not item:
            raise KeyError(key)
        return JsonValue(item)

    def __contains__(self, key: str) -> bool:
        return lib.json_obj_get(self._v, key.encode("utf-8")) is not None

    def get(self, key: str, default=None):
        item = lib.json_obj_get(self._v, key.encode("utf-8"))
        if not item:
            return default
        return JsonValue(item)

    def __repr__(self):
        return f"JsonValue(type={self.type})"


class JsonWriter:
    def __init__(self, capacity: int = 4096):
        self._buf = (ctypes.c_char * capacity)()
        self._w = (json_writer_t * 1)()
        lib.json_writer_init(self._w, self._buf, capacity)

    def null(self):
        lib.json_write_null(self._w)

    def bool(self, val: bool):
        lib.json_write_bool(self._w, int(val))

    def int(self, val: int):
        lib.json_write_int(self._w, val)

    def float(self, val: float):
        lib.json_write_float(self._w, val)

    def str(self, val: str):
        b = val.encode("utf-8")
        lib.json_write_str(self._w, b, len(b))

    def obj_start(self):
        lib.json_write_obj_start(self._w)

    def obj_key(self, key: str):
        b = key.encode("utf-8")
        lib.json_write_obj_key(self._w, b, len(b))

    def obj_end(self):
        lib.json_write_obj_end(self._w)

    def arr_start(self):
        lib.json_write_arr_start(self._w)

    def arr_end(self):
        lib.json_write_arr_end(self._w)

    def finish(self) -> bytes:
        length = lib.json_writer_finish(self._w)
        return bytes(self._buf[:length])

    def reset(self, capacity: int = 4096):
        if capacity > len(self._buf):
            self._buf = (ctypes.c_char * capacity)()
        lib.json_writer_init(self._w, self._buf, capacity)


class Stream:
    def __init__(self, stream_id: int, buf_size: int = 16384):
        self._buf = (ctypes.c_char * 1024)()  # Large enough for brpc_stream_t (112 bytes)
        self._s = ctypes.cast(self._buf, ctypes.POINTER(brpc_stream_t))
        lib.brpc_stream_init(self._s, stream_id, buf_size)

    @property
    def stream_id(self) -> int:
        return self._s.contents.stream_id

    @property
    def state(self) -> str:
        s = self._s.contents.state
        return {0: "idle", 1: "open", 2: "half_closed_local",
                3: "half_closed_remote", 4: "closed"}.get(s, "unknown")

    @property
    def available_read(self) -> int:
        return lib.brpc_stream_available_read(self._s)

    @property
    def available_write(self) -> int:
        return lib.brpc_stream_available_write(self._s)

    def write(self, data: bytes) -> int:
        buf = ctypes.create_string_buffer(data)
        return lib.brpc_stream_write(self._s, buf, len(data))

    def read(self, size: int = 65536) -> bytes:
        buf = ctypes.create_string_buffer(size)
        n = lib.brpc_stream_read(self._s, buf, size)
        if n <= 0:
            return b""
        return buf.raw[:n]

    def close(self):
        lib.brpc_stream_close(self._s)

    def destroy(self):
        pass  # Stream memory is managed by Python (stack-allocated in channel)


class Channel:
    def __init__(self, fd: int, is_server: bool = False, max_streams: int = 0):
        self._buf = (ctypes.c_char * CHANNEL_SIZE)()
        self._ch = ctypes.cast(self._buf, ctypes.c_void_p)
        lib.brpc_channel_init(self._ch, fd, int(is_server), max_streams)

    @property
    def stream_count(self) -> int:
        return ctypes.c_int.from_buffer(self._buf, STREAM_COUNT_OFFSET).value

    def open_stream(self) -> Stream:
        s = lib.brpc_channel_open_stream(self._ch)
        if not s:
            raise RuntimeError("Max streams reached")
        sid = brpc_stream_t.from_address(s).stream_id
        return Stream(sid)

    def send_data(self, stream_id: int, data: bytes, end_stream: bool = False) -> int:
        buf = ctypes.create_string_buffer(data)
        return lib.brpc_channel_send_data(self._ch, stream_id, buf, len(data), int(end_stream))

    def send_ping(self) -> int:
        return lib.brpc_channel_send_ping(self._ch)

    def close(self) -> int:
        return lib.brpc_channel_close(self._ch)

    def recv(self) -> int:
        return lib.brpc_channel_recv(self._ch)

    def pump(self) -> int:
        return lib.brpc_channel_pump(self._ch)

    def destroy(self):
        lib.brpc_channel_destroy(self._ch)


class Profiler:
    @staticmethod
    def init():
        lib.brpc_prof_init()

    @staticmethod
    def reset():
        lib.brpc_prof_reset()

    @staticmethod
    def print():
        lib.brpc_prof_print()
