#!/usr/bin/env python3
"""CLI preferences and actual terminal editing, using only the standard library."""
import errno
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time


binary = os.path.abspath(sys.argv[1])
ansi = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")


def run(env, source, *args):
    return subprocess.run([binary, "--no-history", *args], input=source, text=True,
                          capture_output=True, env=env, timeout=15)


class Terminal:
    def __init__(self, env, *args):
        self.pid, self.fd = pty.fork()
        if not self.pid:
            os.execve(binary, [binary, "--no-history", "--no-config", *args], env)
        self.resize(100)
        assert b"Cterpreter" in self.read(), "missing banner"

    def resize(self, columns):
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, columns, 0, 0))

    def read(self):
        output = b""
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.fd], [], [], 0.15 if output else 2)
            if not ready:
                break
            try:
                block = os.read(self.fd, 65536)
            except OSError as error:
                if error.errno == errno.EIO:
                    break
                raise
            if not block:
                break
            output += block
        return output

    def send(self, data):
        os.write(self.fd, data)
        return self.read()

    def command(self, text):
        return self.send(text.encode() + b"\r")

    def close(self):
        try:
            self.command(".quit")
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                pid, status = os.waitpid(self.pid, os.WNOHANG)
                if pid:
                    assert os.waitstatus_to_exitcode(status) == 0, status
                    self.pid = 0
                    break
                time.sleep(0.02)
            assert not self.pid, "CLI did not exit"
        finally:
            if self.pid:
                os.kill(self.pid, signal.SIGKILL)
                os.waitpid(self.pid, 0)
            os.close(self.fd)


with tempfile.TemporaryDirectory(prefix="cterpreter-cli-") as directory:
    env = dict(os.environ, HOME=directory, TERM="xterm-256color")
    env.pop("NO_COLOR", None)
    config = Path(directory) / ".cterpreterrc"
    result = run(env, ".config tips off\n.config highlighting off\n.config save\n.quit\n", "--no-prompt")
    assert result.returncode == 0 and not result.stderr, result
    assert "tips=off" in config.read_text() and "highlighting=off" in config.read_text()
    result = run(env, ".config tips\n.config highlighting\n.config color\n.quit\n", "--no-prompt", "--color", "never")
    assert result.stdout == "tips = off\nhighlighting = off\ncolor = never\n", result
    assert "\x1b" not in result.stdout
    result = run(env, ".config tips\n.quit\n", "--no-prompt", "--no-config")
    assert result.stdout == "tips = on\n", result
    result = run(env, ".config reset\n.config tips\n.quit\n", "--no-prompt")
    assert "tips = on" in result.stdout and "tips=off" in config.read_text()
    result = run(env, ".config tips yes\n.config missing off\n.config tips\n.quit\n", "--no-prompt")
    assert "tips = off" in result.stdout and "expects on, off, or toggle" in result.stderr
    assert "unknown setting" in result.stderr
    custom = Path(directory) / "custom.conf"
    result = run(env, ".config tips off\n.config save\n.quit\n", "--no-prompt", "--config", str(custom))
    assert result.returncode == 0 and "tips=off" in custom.read_text(), result
    custom.write_text("# preferences\n tips = off # comment\ncolor=never\n")
    result = run(env, ".config suggestions off\n.config save\n.quit\n", "--no-prompt", "--config", str(custom))
    assert result.returncode == 0 and "suggestions=off" in custom.read_text(), result
    assert "suggestions=on" in config.read_text(), "custom config changed the default file"
    custom.write_text("tips=off\nhighlighting=banana\n")
    result = run(env, "", "--config", str(custom), "-e", "42")
    assert result.returncode == 2 and "invalid setting" in result.stderr, result
    config.write_text("tips=off\nhighlighting=banana\n")
    result = run(env, ".config tips\n.quit\n", "--no-prompt")
    assert "tips = on" in result.stdout and "invalid setting" in result.stderr, result
    result = run(env, ".config save\n.quit\n", "--no-prompt", "--no-config", "--config", str(Path(directory) / "missing" / "file"))
    assert "Saved CLI" not in result.stdout and result.stderr, result
    result = run(env, "return 3;\n.config tips off\nreturn 3;\n42\n.quit\n", "--no-prompt", "--no-config")
    assert result.stderr.count("tip:") == 1 and "42\n" in result.stdout, result
    result = run(env, "", "--no-config", "-e", "return 3;")
    assert result.returncode == 1 and "tip:" not in result.stderr, result

    terminal = Terminal(env, "--color", "always")
    try:
        shown = terminal.send(b'int count = 40;')
        assert b"\x1b[36mint" in shown and b"\x1b[33m40" in shown, shown
        terminal.send(b"\r")
        shown = terminal.send(b".config high\t off\r")
        assert b"highlighting = off" in shown, shown
        shown = terminal.send(b"int other = 2;")
        assert b"\x1b[36mint" not in shown and b"\x1b[33m2" not in shown, shown
        terminal.send(b"\r")
        terminal.command(".config suggestions off")
        shown = terminal.send(b"cou\t")
        assert b"count" not in ansi.sub(b"", shown), shown
        terminal.send(b"\x15")
        terminal.command(".config suggestions on")
        shown = terminal.send(b"cou\t")
        assert b"count" in ansi.sub(b"", shown), shown
        terminal.send(b"\x15")
        terminal.command(".config tips off")
        shown = terminal.send(b'printf("(", ')
        assert b"call | int printf(" in shown, shown
        terminal.send(b"\x15")
        terminal.command(".config signatures off")
        shown = terminal.send(b"printf(")
        assert b"call |" not in shown, shown
        terminal.send(b"\x15")
        shown = terminal.send(b"int broken = ;")
        assert b"syntax |" in shown, shown
        terminal.send(b"\x15")
        terminal.command(".config diagnostics off")
        shown = terminal.send(b"int broken = ;")
        assert b"syntax |" not in shown, shown
        terminal.send(b"\x15")
        terminal.command(".config tips on")
        assert b"tip | for (int i = 0;" in terminal.send(b"for (")
        terminal.send(b"\x15")
        terminal.command(".config highlighting on")
        terminal.command("int answer(void) {")
        terminal.command("/* open comment")
        shown = terminal.send(b"still comment */ return count + other;")
        assert b"\x1b[90mstill comment */" in shown and b"\x1b[1;35mreturn" in shown, shown
        terminal.send(b"\r")
        terminal.command("}")
        assert b"\r\n42\r\n" in terminal.command("answer()")
        terminal.resize(32)
        shown = terminal.send(b"1" + b" + 1" * 30)
        # Cursor positioning stays inside the viewport, even for a 121-byte line.
        assert all(int(n) < 32 for n in re.findall(rb"\x1b\[(\d+)C", shown)), shown
        assert b"\r\n31\r\n" in terminal.send(b"\r")
        terminal.command(".clear")
        assert b"signatures = off" in terminal.command(".config signatures")
    finally:
        terminal.close()

    terminal = Terminal(dict(env, NO_COLOR="1"))
    try:
        shown = terminal.send(b"int n = 42;")
        assert not re.search(rb"\x1b\[[0-9;]*m", shown), shown
        terminal.send(b"\x15")
    finally:
        terminal.close()

print("CLI preferences, persistence, tips, highlighting, completion, and narrow terminals passed")
