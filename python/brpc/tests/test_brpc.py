"""Unit tests for brpc Python bindings."""
import pytest
import socket
import sys
import os

# Ensure the shared library is findable
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", ".."))

from brpc import JsonParser, JsonWriter, Stream, Channel, Profiler


class TestJsonParser:
    def setup_method(self):
        self.parser = JsonParser()

    def test_parse_null(self):
        v = self.parser.parse("null")
        assert v.type == "null"

    def test_parse_bool(self):
        v = self.parser.parse("true")
        assert v.type == "bool"
        assert v.as_bool() is True

    def test_parse_int(self):
        v = self.parser.parse("42")
        assert v.type == "int"
        assert v.as_int() == 42

    def test_parse_negative_int(self):
        v = self.parser.parse("-123")
        assert v.as_int() == -123

    def test_parse_float(self):
        v = self.parser.parse("3.14")
        assert v.type == "float"
        assert abs(v.as_float() - 3.14) < 0.001

    def test_parse_string(self):
        v = self.parser.parse('"hello"')
        assert v.type == "string"
        assert v.as_str() == "hello"

    def test_parse_escaped_string(self):
        v = self.parser.parse(r'"a\nb"')
        assert v.as_str() == "a\nb"

    def test_parse_empty_array(self):
        v = self.parser.parse("[]")
        assert v.type == "array"
        assert len(v) == 0

    def test_parse_array(self):
        v = self.parser.parse("[1,2,3]")
        assert len(v) == 3
        assert v[0].as_int() == 1
        assert v[2].as_int() == 3

    def test_parse_empty_object(self):
        v = self.parser.parse("{}")
        assert v.type == "object"
        assert len(v) == 0

    def test_parse_object(self):
        v = self.parser.parse('{"name":"test","value":42}')
        assert v["name"].as_str() == "test"
        assert v["value"].as_int() == 42
        assert "name" in v
        assert "missing" not in v

    def test_parse_nested(self):
        v = self.parser.parse('{"a":{"b":1}}')
        assert v["a"]["b"].as_int() == 1

    def test_parse_error(self):
        with pytest.raises(ValueError, match="parse error"):
            self.parser.parse("invalid")

    def test_parse_unicode(self):
        v = self.parser.parse(r'"\u0041"')
        assert v.as_str() == "A"


class TestJsonWriter:
    def test_write_null(self):
        w = JsonWriter()
        w.null()
        assert w.finish() == b"null"

    def test_write_bool(self):
        w = JsonWriter()
        w.bool(True)
        assert w.finish() == b"true"

    def test_write_int(self):
        w = JsonWriter()
        w.int(42)
        assert w.finish() == b"42"

    def test_write_string(self):
        w = JsonWriter()
        w.str("hello")
        assert w.finish() == b'"hello"'

    def test_write_object(self):
        w = JsonWriter()
        w.obj_start()
        w.obj_key("key")
        w.int(123)
        w.obj_end()
        assert w.finish() == b'{"key":123}'

    def test_write_array(self):
        w = JsonWriter()
        w.arr_start()
        w.int(1)
        w.int(2)
        w.int(3)
        w.arr_end()
        assert w.finish() == b"[1,2,3]"

    def test_roundtrip(self):
        parser = JsonParser()
        writer = JsonWriter()

        original = '{"method":"getUser","id":1,"params":{"userId":42}}'
        v = parser.parse(original)

        writer.obj_start()
        writer.obj_key("method")
        writer.str(v["method"].as_str())
        writer.obj_key("id")
        writer.int(v["id"].as_int())
        writer.obj_end()

        result = writer.finish()
        v2 = parser.parse(result)
        assert v2["method"].as_str() == "getUser"
        assert v2["id"].as_int() == 1


class TestStream:
    def test_write_read(self):
        s = Stream(1, 256)
        data = b"hello world"
        n = s.write(data)
        assert n == len(data)
        assert s.available_write == 256 - len(data)
        s.destroy()

    def test_ring_buffer_wrap(self):
        s = Stream(1, 16)
        for _ in range(20):
            s.write(b"1234567")
        assert s.available_write >= 0
        s.destroy()

    def test_close_transitions(self):
        s = Stream(1, 256)
        s.close()
        assert s.state == "half_closed_local"
        s.destroy()


class TestChannel:
    def setup_method(self):
        self.s1, self.s2 = socket.socketpair()
        self.client = Channel(self.s1.fileno(), is_server=False)
        self.server = Channel(self.s2.fileno(), is_server=True)

    def teardown_method(self):
        self.client.destroy()
        self.server.destroy()
        self.s1.close()
        self.s2.close()

    def test_open_stream(self):
        s = self.client.open_stream()
        assert s.stream_id == 1
        assert self.client.stream_count == 1

    def test_send_data(self):
        s = self.client.open_stream()
        rc = self.client.send_data(s.stream_id, b"test", end_stream=True)
        assert rc == 0

    def test_send_ping(self):
        rc = self.client.send_ping()
        assert rc == 0

    def test_roundtrip(self):
        cs = self.client.open_stream()
        payload = b'{"method":"ping","id":1}'
        self.client.send_data(cs.stream_id, payload, end_stream=True)

        rc = self.server.recv()
        assert rc == 0

    def test_close(self):
        rc = self.client.close()
        assert rc == 0


class TestProfiler:
    def test_init(self):
        Profiler.init()
        Profiler.reset()

    def test_print(self):
        Profiler.init()
        Profiler.reset()
        Profiler.print()
