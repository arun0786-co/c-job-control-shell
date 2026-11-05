#!/usr/bin/env python3
"""Tests for the shell. usage: tests/run.py [text a test name must contain]"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness as h  # noqa: E402
from harness import Sandbox, Term, check, piped, same  # noqa: E402


def test_commands():
    box = Sandbox()
    same("run a program", piped(box, "echo hello")[0], "hello\n")
    same("a path as the name", piped(box, "/bin/echo hi")[0], "hi\n")
    out, err, _ = piped(box, "no_such_program_x")
    same("unknown command", err, "Command not found!\n")
    same("blank lines do nothing", piped(box, "", "   ", "echo a")[0], "a\n")
    same("many arguments", piped(box, "echo " + " ".join(["x"] * 300) + " | wc -w")[0].strip(), "300")
    same("sequence", piped(box, "echo a ; echo b")[0], "a\nb\n")
    box.close()


def test_syntax():
    box = Sandbox()
    for bad in ["| ls", "ls |", "ls < ", "ls > > f", "ls < > f", "; ls", "& ls", "ls && ", "|| ls",
                "ls && && ls", "ls || | ls", "ls ; ; ls", "ls & & ls"]:
        same(f"rejects {bad!r}", piped(box, bad)[1], "Invalid Syntax!\n")
    box.close()


def test_strict():
    box = Sandbox()
    strict = {"args": ["--strict"]}
    for bad in ["ls ; ", "echo a && echo b", "echo a || echo b"]:
        same(f"strict rejects {bad!r}", piped(box, bad, **strict)[1], "Invalid Syntax!\n")
    same("strict allows a final &", piped(box, "true &", **strict)[1], "")
    box.close()


def test_and_or():
    box = Sandbox()
    same("&& after success", piped(box, "true && echo yes")[0], "yes\n")
    same("&& after failure", piped(box, "false && echo yes")[0], "")
    same("|| after failure", piped(box, "false || echo yes")[0], "yes\n")
    same("|| after success", piped(box, "true || echo yes")[0], "")
    same("a skipped group keeps the status", piped(box, "false && echo a || echo b")[0], "b\n")
    same("and then or", piped(box, "true && echo a || echo b")[0], "a\n")
    same("; starts again", piped(box, "false && echo a ; echo b")[0], "b\n")
    same("the pipe's last command decides", piped(box, "false | true && echo yes")[0], "yes\n")
    same("not found counts as failure", piped(box, "no_such_x || echo no")[0], "no\n")
    same("builtins give a status", piped(box, "hop nowhere || echo no")[0], "no\n")
    same("a trailing ; is fine", piped(box, "echo a ;")[0], "a\n")
    os.makedirs(box.path("d"))
    same("&& with a builtin in this shell", piped(box, "hop d && pwd")[0], box.path("d") + "\n")
    out = piped(box, "true && sleep 0.2 &", "activities", "sleep 0.5", "echo end")[0]
    check("and-or in the background is one job", re.search(r"^\[1\] \d+$", out, re.M), out)
    check("it has the whole text", "true && sleep 0.2 - Running" in out, out)
    check("and it finishes", "true && sleep 0.2 with pid" in out and "exited normally" in out, out)
    same("no wait for it", piped(box, "sleep 5 && echo late &", "echo first", timeout=3)[0].split("\n")[1], "first")
    t = Term(box)
    t.wait_prompt()
    t.run("sleep 50 && echo after &")
    out = t.run("activities")
    check("background and-or listed", "sleep 50 && echo after - Running" in out, out)
    pid = re.search(r"\[(\d+)\] : sleep 50", out).group(1)
    t.run(f"ping {pid} 9")
    time.sleep(0.2)
    out = t.run("true")
    check("killing it ends all of it", "exited abnormally" in out and "after" not in out.replace("echo after", ""), out)
    t.close()
    box.close()


def test_redirection():
    box = Sandbox()
    piped(box, "echo one > f")
    same("> writes", box.read("f"), "one\n")
    piped(box, "echo two >> f")
    same(">> appends", box.read("f"), "one\ntwo\n")
    piped(box, "echo three > f")
    same("> truncates", box.read("f"), "three\n")
    same("< reads", piped(box, "cat < f")[0], "three\n")
    same("last output wins", (piped(box, "echo x > a > b"), box.read("a"), box.read("b"))[1:], ("", "x\n"))
    out, err, _ = piped(box, "cat < missing")
    same("missing input", err, "No such file or directory\n")
    piped(box, "echo y > g", "cat < missing < g > out")
    check("a bad input stops the command", not os.path.exists(box.path("out")) or box.read("out") == "")
    os.makedirs(box.path("d"), exist_ok=True)
    same("cannot write", piped(box, "echo x > d")[1], "Unable to create file for writing\n")
    same("redirect a builtin", (piped(box, "reveal > list"), box.read("list").split()[0])[1], "a")
    box.close()


def test_pipes():
    box = Sandbox()
    same("pipe", piped(box, "echo hello | tr a-z A-Z")[0], "HELLO\n")
    same("long pipe", piped(box, "echo c b a | tr a-c A-C | cut -c1")[0], "C\n")
    same("builtin in a pipe", piped(box, "hop . | cat ; echo ok")[0], "ok\n")
    same("pipe beats redirect", (piped(box, "echo x > p | wc -l")[0].strip(), box.read("p")), ("0", "x\n"))
    same("reader sees EOF", piped(box, "echo a | cat | cat | cat")[0], "a\n")
    box.close()


def test_builtins():
    box = Sandbox()
    os.makedirs(box.path("a/b"))
    box.write("a/f", "")
    box.write(".hidden", "")
    same("hop", piped(box, "hop a", "pwd")[0], box.path("a") + "\n")
    same("hop home", piped(box, "hop a", "hop", "pwd")[0], box.dir + "\n")
    same("hop ~", piped(box, "hop a/b", "hop ~/a", "pwd")[0], box.path("a") + "\n")
    same("hop -", piped(box, "hop a", "hop -", "pwd")[0], box.dir + "\n")
    same("hop - with nothing before", piped(box, "hop -", "pwd")[0], box.dir + "\n")
    out, err, _ = piped(box, "hop nowhere a", "pwd")
    same("hop continues past a bad one", (err, out), ("No such directory!\n", box.path("a") + "\n"))
    same("hop in sequence", piped(box, "hop a b .. -", "pwd")[0], box.path("a/b") + "\n")
    same("reveal", piped(box, "reveal a")[0], "b f\n")
    same("reveal -l", piped(box, "reveal -l a")[0], "b\nf\n")
    check("reveal -a", ".hidden" in piped(box, "reveal -a")[0])
    check("reveal without -a", ".hidden" not in piped(box, "reveal")[0])
    same("reveal bad flag", piped(box, "reveal -z")[1], "reveal: Invalid Syntax!\n")
    same("reveal two dirs", piped(box, "reveal a a")[1], "reveal: Invalid Syntax!\n")
    same("reveal missing", piped(box, "reveal nope")[1], "No such directory!\n")
    box.close()


def test_log():
    box = Sandbox()
    same("log stores", piped(box, "echo a", "echo b", "log")[0].splitlines()[-2:], ["echo a", "echo b"])
    same("no repeat", piped(box, "log purge", "echo a", "echo a", "log")[0], "a\na\necho a\n")
    same("log purge", piped(box, "log purge", "log")[0], "")
    same("log not stored", piped(box, "log purge", "log", "log")[0], "")
    same("execute", piped(box, "log purge", "echo first", "echo second", "log execute 2")[0],
         "first\nsecond\nfirst\n")
    same("execute out of range", piped(box, "log execute 99")[1], "log: Invalid Syntax!\n")
    same("survives a restart", (piped(box, "log purge", "echo kept"), piped(box, "log")[0])[1], "echo kept\n")
    lines = [f"echo {i}" for i in range(20)]
    same("keeps 15", piped(box, "log purge", *lines, "log")[0].splitlines()[-15:], lines[5:])
    same("a line with log is not stored", piped(box, "log purge", "echo a ; log", "log")[0], "a\n")
    box.close()


def test_jobs_piped():
    box = Sandbox()
    out = piped(box, "sleep 0.1 &", "sleep 0.4", "echo done")[0]
    check("background prints [n] pid", re.search(r"^\[1\] \d+$", out, re.M), out)
    check("background end is reported", re.search(r"^sleep 0\.1 with pid \d+ exited normally$", out, re.M), out)
    out = piped(box, "false &", "sleep 0.3", "echo x")[0]
    check("abnormal exit reported", "false with pid" in out and "exited abnormally" in out, out)
    out = piped(box, "sleep 30 &", "activities")[0]
    check("activities lists it", re.search(r"^\[\d+\] : sleep 30 - Running$", out, re.M), out)

    out = piped(box, "sleep 30 &", "activities | cat", "activities")[0]
    runs = re.findall(r"^\[\d+\] : sleep 30 - Running$", out, re.M)
    same("activities in a pipe shows live jobs", len(runs), 2)
    check("a pipe does not report a live job as finished", "exited" not in out, out)

    out, err, _ = piped(box, "sleep 30 &", "fg | cat", "activities")
    check("fg refuses in a pipe", "no job control" in err and "sleep 30 - Running" in out, (out, err))

    out, err, _ = piped(box, "sleep 30 &", "ping 999999 9")
    same("ping missing", err, "No such process found\n")
    same("ping bad", piped(box, "ping 1 abc")[1], "Invalid syntax!\n")
    out = piped(box, "sleep 30 &", "sleep 0.1", "activities")[0]
    pid = re.search(r"\[1\] (\d+)", out).group(1)
    out = piped(box, f"ping {os.getpid()} 32")[0]
    check("ping reduces mod 32", "Sent signal 32" in out, out)
    box.close()


def test_job_control():
    box = Sandbox()
    t = Term(box)
    check("prompt appears", t.wait_prompt())
    out = t.run("hop")
    t.send("sleep 100\n")
    time.sleep(0.3)
    t.send("\x1a")
    check("ctrl-z stops the job", t.wait_for("Stopped sleep 100"), t.since())
    check("back at the prompt", t.wait_prompt())
    check("activities shows it stopped", "sleep 100 - Stopped" in t.run("activities"), t.since())
    check("bg resumes it", "[1] sleep 100 &" in t.run("bg 1"), t.since())
    check("activities shows it running", "sleep 100 - Running" in t.run("activities"), t.since())
    t.send("fg 1\n")
    check("fg names the job", t.wait_for("sleep 100"), t.since())
    time.sleep(0.2)
    t.send("\x03")
    check("ctrl-c ends the foreground job", t.wait_prompt(), t.since())
    check("job has gone", "sleep 100" not in t.run("activities"), t.since())
    same("fg with no job", t.run("fg").strip().splitlines()[-1], "No such job")
    same("bg with no job", t.run("bg 7").strip().splitlines()[-1], "No such job")

    t.send("sleep 100 | cat\n")
    time.sleep(0.3)
    t.send("\x1a")
    check("ctrl-z stops a whole pipeline", t.wait_for("Stopped sleep 100 | cat"), t.since())
    t.wait_prompt()
    t.send("fg\n")
    time.sleep(0.3)
    t.send("\x03")
    check("ctrl-c ends a whole pipeline", t.wait_prompt(), t.since())

    t.send("\x03")
    time.sleep(0.2)
    check("ctrl-c at the prompt leaves the shell running", "alive" in t.run("echo alive"), t.since())

    t.run("sleep 100 &")
    t.send("\x04")
    code = t.wait_exit()
    check("ctrl-d logs out", "logout" in t.out and code == 0, (code, t.out[-100:]))
    t.close()

    t = Term(box)
    t.wait_prompt()
    t.run("cat &")
    time.sleep(0.3)
    out = t.run("activities")
    check("a background reader is stopped", "cat - Stopped" in out, out)
    t.close()
    box.close()


def test_terminal_modes():
    box = Sandbox()
    box.write("noecho.py", """
import os, signal, sys, termios
m = termios.tcgetattr(0)
m[3] &= ~termios.ECHO
termios.tcsetattr(0, termios.TCSANOW, m)
print("echo is off", flush=True)
os.kill(os.getpid(), signal.SIGSTOP)
print("echo on" if termios.tcgetattr(0)[3] & termios.ECHO else "still off", flush=True)
""")
    box.write("diesilent.py", """
import os, signal, termios
m = termios.tcgetattr(0)
m[3] &= ~termios.ECHO
termios.tcsetattr(0, termios.TCSANOW, m)
os.kill(os.getpid(), signal.SIGKILL)
""")
    t = Term(box)
    t.wait_prompt()
    t.send("python3 noecho.py\n")
    check("a stopped job reports", t.wait_for("Stopped python3 noecho.py"), t.since())
    t.wait_prompt()
    modes = t.run("stty -a")
    check("the shell gets its modes back", re.search(r"(?<![-\w])echo\b", modes), modes)
    check("the job gets its modes back on fg", "still off" in t.run("fg"), t.since())
    check("a job that exits keeps its change", re.search(r"-echo\b", t.run("stty -a")), t.since())
    t.run("stty echo")
    t.send("python3 diesilent.py\n")
    t.wait_prompt()
    modes = t.run("stty -a")
    check("a killed job's modes are undone", re.search(r"(?<![-\w])echo\b", modes), modes)
    t.close()
    box.close()


def main():
    if len(sys.argv) > 1:
        h.only = sys.argv[1]
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            fn()
    return h.finish()


if __name__ == "__main__":
    sys.exit(main())
