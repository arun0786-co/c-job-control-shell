"""Helpers for driving the shell, through pipes or a real terminal."""
import os
import pty
import re
import select
import shutil
import signal
import subprocess
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHELL = os.environ.get("SHELL_UNDER_TEST", os.path.join(ROOT, "shell.out"))
PROMPT = re.compile(r"<[^<>\n]*@[^<>\n]*:[^<>\n]*> ")
ESCAPE = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")

failures = []
passed = 0
only = None


def check(name, good, detail=""):
    global passed
    if only and only not in name:
        return
    if good:
        passed += 1
        print("ok   " + name, flush=True)
    else:
        failures.append(name)
        print("FAIL " + name + (": " + repr(detail) if detail != "" else ""), flush=True)


def same(name, got, want):
    check(name, got == want, f"got {got!r}, want {want!r}")


class Sandbox:
    """A scratch directory that is both the shell's start directory and HOME."""

    def __init__(self):
        self.dir = os.path.realpath(tempfile.mkdtemp(prefix="shelltest"))
        self.env = dict(os.environ, HOME=self.dir, PATH=os.environ.get("PATH", ""), TERM="xterm")
        self.env.pop("SHELL_UNDER_TEST", None)

    def path(self, *parts):
        return os.path.join(self.dir, *parts)

    def write(self, name, text):
        full = self.path(name)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w") as f:
            f.write(text)
        return full

    def read(self, name):
        with open(self.path(name)) as f:
            return f.read()

    def close(self):
        shutil.rmtree(self.dir, ignore_errors=True)


def strip_prompts(text):
    return PROMPT.sub("", text)


def piped(box, *lines, args=(), timeout=20):
    """Feed lines to the shell's stdin. Returns stdout without prompts, stderr
    and the exit status."""
    data = "".join(line + "\n" for line in lines)
    p = subprocess.run([SHELL, *args], input=data, cwd=box.dir, env=box.env, text=True,
                       capture_output=True, timeout=timeout)
    out = strip_prompts(p.stdout)
    if out.endswith("logout\n"):
        out = out[:-len("logout\n")]
    return out, p.stderr, p.returncode


class Term:
    """The shell on a real terminal, so that Ctrl-C and Ctrl-Z are the
    terminal's own signals."""

    def __init__(self, box, args=(), cols=200):
        self.box = box
        pid, fd = pty.fork()
        if pid == 0:
            os.chdir(box.dir)
            os.execve(SHELL, [SHELL, *args], box.env)
        self.pid, self.fd = pid, fd
        self.resize(24, cols)
        self.out = ""
        self.mark = 0

    def resize(self, rows, cols):
        import fcntl
        import struct
        import termios
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))

    def pump(self, secs=0.05):
        end = time.time() + secs
        while True:
            left = end - time.time()
            r, _, _ = select.select([self.fd], [], [], max(left, 0))
            if r:
                try:
                    d = os.read(self.fd, 65536)
                except OSError:
                    return False
                if not d:
                    return False
                self.out += d.decode(errors="replace")
            elif left <= 0:
                return True

    def wait_for(self, what, secs=10):
        """Wait until the text (or compiled pattern) appears after the mark."""
        end = time.time() + secs
        while True:
            new = self.out[self.mark:]
            found = what.search(new) if hasattr(what, "search") else what in new
            if found:
                return True
            if time.time() > end:
                return False
            self.pump(0.05)

    def send(self, text):
        self.mark = len(self.out)
        os.write(self.fd, text.encode())

    def wait_prompt(self, secs=10):
        """Wait for a prompt at the very end of the output."""
        end = time.time() + secs
        at_end = re.compile(PROMPT.pattern + r"(\x1b\[[0-9;?]*[A-Za-z]|\r)*$")
        while True:
            if at_end.search(self.out[self.mark:]):
                return True
            if time.time() > end:
                return False
            self.pump(0.05)

    def run(self, line, secs=10):
        """Type a line, wait for the next prompt, and return what it printed."""
        self.send(line + "\n")
        self.wait_prompt(secs)
        return self.since()

    def since(self):
        text = ESCAPE.sub("", self.out[self.mark:]).replace("\r", "")
        return PROMPT.sub("", text)

    def close(self):
        try:
            os.kill(self.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            os.waitpid(self.pid, 0)
        except ChildProcessError:
            pass
        os.close(self.fd)

    def wait_exit(self, secs=5):
        end = time.time() + secs
        while time.time() < end:
            self.pump(0.05)
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                return os.waitstatus_to_exitcode(status)
        return None


def finish():
    print(f"\n{passed} passed, {len(failures)} failed" + (f": {failures}" if failures else ""))
    return 1 if failures else 0
