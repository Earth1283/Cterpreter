#!/usr/bin/env python3
"""Cterpreter interpreting main.c: a Cterpreter that runs inside a Cterpreter."""
import errno
import fcntl
import os
import pty
import re
import select
import struct
import subprocess
import sys
import tempfile
import termios
import time

binary = os.path.abspath(sys.argv[1])
root = os.path.abspath(sys.argv[2])
ansi = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")
quiet = ["--no-config", "--no-history"]


def run(source, *args, limit="4000000000"):
    return subprocess.run([binary, *quiet, "--max-steps", limit, *args], input=source, text=True,
                          capture_output=True, cwd=root, timeout=120)


result = run(".load main.c\n1 + 2\n", "--no-prompt")
assert result.returncode == 0 and result.stdout == "3\n" and not result.stderr, result

result = run("1 + 2\nint v = 6;\nv * 7\n.depth\n", "main.c", "--no-prompt", *quiet)
assert result.returncode == 0 and result.stdout == "3\n42\nInterpreter nesting level 1\n", result

result = run("", "main.c", *quiet, "-e", "printf(\"%d\\n\", 6 * 7); exit(3);")
assert result.returncode == 3 and result.stdout == "42\n", result

result = run("", "main.c", *quiet, "--verbose", "-e", "1")
assert "[PASS] 2 + 2 = 4" in result.stderr and result.stdout == "1\n", result

result = run("", "main.c", *quiet, "examples/basics/hello.c", "Ada")
assert result.returncode == 0 and result.stdout == "Hello, Ada!\n", result

result = run("", "main.c", "--bogus")
assert result.returncode == 2 and "Usage: Cterpreter" in result.stderr, result

with tempfile.TemporaryDirectory(prefix="cterpreter-self-") as directory:
    path = os.path.join(directory, "rc")
    result = run(".config tips off\n.config save\n.config tips\n", "main.c", "--no-prompt", "--no-history",
                 "--config", path, "--color", "never")
    assert result.returncode == 0 and "Saved CLI preferences to " + path in result.stdout, result
    assert "tips=off" in open(path).read(), result
    result = run("", "main.c", "--config", path, "-e", "1")
    assert result.returncode == 0 and result.stdout == "1\n", result

copy = os.path.join(root, ".self_host_main.c")
with open(os.path.join(root, "main.c")) as source:
    text = source.read().replace("int main(int argc", "int cterpreter_main(int argc")
assert "cterpreter_main" in text
with open(copy, "w") as target:
    target.write(text)
declare = 'char *inner[] = {"x", "--no-prompt", "--no-config", "--no-history", "--max-steps", "500000000", 0};'
script = "".join(f".load .self_host_main.c\n{declare}\ncterpreter_main(6, inner);\n" for _ in range(3))
script += ".depth\n1 + 1\n" + ".quit\n.depth\n" * 3
result = run(script, "--no-prompt")
assert result.returncode == 0 and not result.stderr, result
assert result.stdout == "Interpreter nesting level 3\n2\n" + "".join(
    f"Interpreter nesting level {level}\n" for level in (2, 1, 0)), result

script = "".join(f".load .self_host_main.c\n{declare}\ncterpreter_main(6, inner);\n" for _ in range(9)) + "1 + 1\n"
result = run(script, "--no-prompt")
assert "nested interpreter limit exceeded" in result.stderr and result.stdout.endswith("2\n"), result




class Terminal:
    def __init__(self, *args):
        self.pid, self.fd = pty.fork()
        if not self.pid:
            os.chdir(root)
            env = dict(os.environ, TERM="xterm", HOME=tempfile.gettempdir())
            env.pop("NO_COLOR", None)
            os.execve(binary, [binary, *quiet, *args], env)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
        assert b"Cterpreter" in self.read()

    def read(self, quiet_for=0.4):
        output = b""
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.fd], [], [], quiet_for if output else 2)
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
        return ansi.sub(b"", output)

    def send(self, data):
        os.write(self.fd, data)
        return self.read()

    def command(self, text):
        return self.send(text.encode() + b"\r")

    def close(self):
        try:
            os.kill(self.pid, 9)
            os.waitpid(self.pid, 0)
        finally:
            os.close(self.fd)


terminal = Terminal()
try:
    assert b"C, interpreted. Mistakes welcome." in terminal.command(".load main.c")
    assert b"\r\n3\r\n" in terminal.command("1 + 2")
    terminal.command(".quit")
finally:
    terminal.close()

terminal = Terminal("--max-steps", "99999999999")
try:
    terminal.command(".load .self_host_main.c")
    terminal.command('char *inner[] = {"x", "--max-steps", "9999999999", "--no-config", "--no-history", 0};')
    shown = terminal.command("cterpreter_main(5, inner);")
    assert b"C, interpreted. Mistakes welcome." in shown, shown
    assert b"\r\n3\r\n" in terminal.command("1 + 2"), "the inner prompt did not evaluate"
    shown = terminal.send(b"int total = 4; total * 5")
    assert b"call |" not in shown, shown
    assert b"\r\n20\r\n" in terminal.send(b"\r"), "the inner terminal did not accept a line"
    shown = terminal.send(b"printf(")
    assert b"call | int printf(" in shown, shown
    terminal.send(b"\x15")
    terminal.command("while (1) {}")
    time.sleep(0.3)
    shown = terminal.send(b"\x03")
    assert b"execution interrupted" in shown, shown
    assert b"\r\n2\r\n" in terminal.command("1 + 1"), "the inner session did not survive Ctrl+C"
    shown = terminal.send(b"abc\x03")
    assert b"execution interrupted" not in shown, shown
    assert b"\r\n4\r\n" in terminal.command("2 + 2"), "the inner session did not survive Ctrl+C at the prompt"
    terminal.command(".quit")
    assert b"\r\n6\r\n" in terminal.command("2 * 3"), "the outer session did not resume"
finally:
    terminal.close()
    os.unlink(copy)

print("Cterpreter interpreting its own front end passed")
