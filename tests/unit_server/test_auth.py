"""Test token format and port file parsing."""
import re
import json
from pathlib import Path


class TestTokenFormat:
    def test_hex_pattern(self):
        pattern = re.compile(r'^[0-9a-f]{64}$')
        token = "abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234"
        assert pattern.match(token)

    def test_invalid_token_rejected(self):
        pattern = re.compile(r'^[0-9a-f]{64}$')
        assert not pattern.match("not-hex")
        assert not pattern.match("abc")
        assert not pattern.match("g" * 64)


class TestPortFileParsing:
    def test_parse_port_file(self, tmp_path):
        pf = tmp_path / "port.txt"
        pf.write_text("12345\nabcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234\n")
        lines = pf.read_text().strip().split("\n")
        port = int(lines[0].strip())
        token = lines[1].strip()
        assert port == 12345
        assert len(token) == 64

    def test_parse_with_whitespace(self, tmp_path):
        pf = tmp_path / "port.txt"
        pf.write_text("  45678  \n  abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234  \n")
        lines = pf.read_text().strip().split("\n")
        port = int(lines[0].strip())
        token = lines[1].strip()
        assert port == 45678
        assert len(token) == 64
