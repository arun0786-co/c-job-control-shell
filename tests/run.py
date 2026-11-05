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


def test_new_builtins():
    box = Sandbox()
    os.makedirs(box.path("d"))
    same("cd", piped(box, "cd d", "pwd")[0], box.path("d") + "\n")
    same("cd home", piped(box, "cd d", "cd", "pwd")[0], box.dir + "\n")
    same("export sets", piped(box, "export A=1", "printenv A")[0], "1\n")
    same("export many", piped(box, "export A=1 B=2", "printenv B")[0], "2\n")
    same("export does not split", piped(box, "X='a b'", "export A=$X", 'echo "$A"')[0], "a b\n")
    check("export lists", "XY=1\n" in piped(box, "XY=1", "export")[0])
    same("export bad name", piped(box, "export 1a=2")[1], "export: 1a=2: not a valid name\n")
    same("unset", piped(box, "A=1", "unset A", 'echo "[$A]"')[0], "[]\n")
    same("unset bad name", piped(box, "unset a-b")[1], "unset: a-b: not a valid name\n")
    same("exit stops here", piped(box, "exit 3", "echo no")[0], "")
    same("exit gives its number", piped(box, "exit 3")[2], 3)
    same("exit keeps the last status", piped(box, "false", "exit")[2], 1)
    same("exit bad number", piped(box, "exit x", "echo yes")[:2], ("yes\n", "exit: Invalid Syntax!\n"))
    same("exit in a pipe stays in the pipe", piped(box, "exit 3 | cat", "echo yes")[0], "yes\n")
    box.write("lib.sh", "LIBVAR=7\nhop d\n")
    same("source runs in this shell", piped(box, "source lib.sh", "echo $LIBVAR", "pwd")[0], "7\n" + box.path("d") + "\n")
    same(". is source", piped(box, ". lib.sh", "echo $LIBVAR")[0], "7\n")
    same("source missing", piped(box, "source nope")[2], 0)
    check("source says why", "nope:" in piped(box, "source nope")[1])
    box.write("loop.sh", "source loop.sh\n")
    check("source of itself ends", "too many nested" in piped(box, "source loop.sh")[1])
    for name in ["export", "unset", "exit", "source"]:
        same(f"strict has no {name}", piped(box, f"{name} x", args=["--strict"])[1], "Command not found!\n")
    same("strict: cd is not a builtin", piped(box, "cd d", "pwd", args=["--strict"])[0], box.dir + "\n")
    box.close()


def test_scripts():
    import subprocess
    box = Sandbox()
    def run(*argv, **kw):
        return subprocess.run([h.SHELL, *argv], cwd=box.dir, env=box.env, text=True, capture_output=True, timeout=20, **kw)
    r = run("-c", "echo a ; echo b")
    same("-c", (r.stdout, r.returncode), ("a\nb\n", 0))
    same("-c status", run("-c", "exit 4").returncode, 4)
    same("-c last status", run("-c", "false").returncode, 1)
    same("-c args", run("-c", "echo $0 $1 $#", "n", "one").stdout, "n one 1\n")
    box.write("s.sh", "#!/bin/whatever\n# comment\nX=1\necho $X\n\necho $0 $1 $2 $#\nfalse\n")
    r = run("s.sh", "p q", "r")
    same("a script", (r.stdout, r.stderr, r.returncode), ("1\ns.sh p q r 3\n".replace("p q r 3", "p q r 2"), "", 1))
    box.write("at.sh", 'printf "<%s>" "$@"\necho\nprintf "<%s>" $@\necho\nprintf "<%s>" "x$@y"\necho\n')
    same('"$@" and $@', run("at.sh", "a b", "c").stdout, "<a b><c>\n<a><b><c>\n<xa b><cy>\n")
    box.write("none.sh", 'printf "<%s>" "$@"\necho\n')
    same('"$@" with none', run("none.sh").stdout, "<>\n")
    same("no such script", run("nope.sh").returncode, 127)
    same("no prompt in a script", run("-c", "echo a").stdout, "a\n")
    same("nothing stored from a script", (run("-c", "echo a"), piped(box, "log")[0])[1], "")
    box.write("bg.sh", "sleep 0.1 &\nsleep 0.3\necho end\n")
    same("a script is quiet about jobs", run("bg.sh").stdout.splitlines()[-1], "end")
    same("usage", run("--nope").returncode, 2)
    box.write("read.sh", "cat\n")
    same("a script's command reads stdin", run("read.sh", input="typed\n").stdout, "typed\n")
    box.write("job.sh", 'sleep 0.2 &\nW=$!\necho got\nsleep 0.4\nfg\n')
    same("a background job and a foreground one", run("job.sh").stdout.split("\n")[1], "got")

    t = Term(box, args=["-c", "cat ; echo after"])
    t.pump(0.3)
    t.send("to-cat\n")
    check("a command in -c can read the terminal", t.wait_for(re.compile(r"to-cat\r?\n.*to-cat", re.S)), t.out)
    t.send("\x04")
    check("then the next one runs", t.wait_for("after"), t.out)
    t.close()
    t = Term(box, args=["-c", "sleep 100 ; echo after"])
    t.pump(0.3)
    t.send("\x03")
    same("ctrl-c ends a script", t.wait_exit(), -2)
    t.close()
    box.close()


def editing(box, **kw):
    t = Term(box, **kw)
    t.wait_prompt()
    prompt, col = t.screen()
    return t, col


def test_edit_keys():
    box = Sandbox()
    t, p = editing(box)
    def typed(keys, want_text, want_col=None, name=None):
        t.send(keys)
        text, col = t.screen()
        same(name or f"keys {keys!r}", text[p:], want_text)
        if want_col is not None:
            same((name or f"keys {keys!r}") + " cursor", col - p, want_col)
    typed("hello", "hello", 5, "typing")
    typed("\x1b[D\x1b[D", "hello", 3, "left arrow")
    typed("XY", "helXYlo", 5, "insert in the middle")
    typed("\x7f", "helXlo", 4, "backspace")
    typed("\x1b[C", "helXlo", 5, "right arrow")
    typed("\x01", "helXlo", 0, "ctrl-a")
    typed("\x05", "helXlo", 6, "ctrl-e")
    typed("\x1b[H", "helXlo", 0, "home")
    typed("\x1b[F", "helXlo", 6, "end")
    typed("\x1b[H\x1b[3~", "elXlo", 0, "delete")
    typed("\x04", "lXlo", 0, "ctrl-d deletes")
    typed("\x05 two three", "lXlo two three", None, "more")
    typed("\x17", "lXlo two ", 9, "ctrl-w")
    typed("\x1bb", "lXlo two ", 5, "alt-b")
    typed("\x1bf", "lXlo two ", 8, "alt-f")
    typed("\x1b[1;5D", "lXlo two ", 5, "ctrl-left")
    typed("\x0b", "lXlo ", 5, "ctrl-k")
    typed("\x15", "", 0, "ctrl-u")
    typed("\x1b[D", "", 0, "left at the start")
    typed("\x7f", "", 0, "backspace at the start")
    typed("\x1b[C", "", 0, "right at the end")
    typed("héllo", "héllo", 5, "utf-8 counts as one column")
    typed("\x1b[D\x1b[D\x1b[D\x1b[D", "héllo", 1, "left over utf-8")
    typed("\x1b[C\x7f", "hllo", 1, "backspace over utf-8")
    t.send("\x15")
    t.screen()
    check("the line runs", "abc" in t.run("echo abc"), t.since())
    t.send("echo a\x03")
    text, col = t.screen()
    same("ctrl-c abandons the line", text[p:], "")
    check("and shows it did", "^C" in t.out)
    t.send("\x1bq")
    text, _ = t.screen()
    same("an unknown alt key does nothing", text[p:], "")
    t.send("\x15\x0c")
    check("ctrl-l clears", t.wait_for("\x1b[2J"))
    t.close()
    t = Term(box, cols=30)
    t.wait_prompt()
    long = "echo " + "x" * 200
    t.send(long)
    t.pump(0.3)
    check("a long line does not break", t.pump(0.05))
    out = t.run("")
    check("a long line runs whole", "x" * 200 in out, out)
    t.close()
    box.close()


def test_edit_history():
    box = Sandbox()
    t, p = editing(box)
    for line in ["echo first", "echo second", "echo third"]:
        t.run(line)
    t.send("draft")
    t.screen()
    def key(k, want, name):
        t.send(k)
        same(name, t.screen()[0][p:], want)
    key("\x1b[A", "echo third", "up")
    key("\x1b[A", "echo second", "up again")
    key("\x10", "echo first", "ctrl-p")
    key("\x1b[A", "echo first", "up at the oldest")
    key("\x1b[B", "echo second", "down")
    key("\x0e", "echo third", "ctrl-n")
    key("\x1b[B", "draft", "down returns to the draft")
    key("\x1b[B", "draft", "down at the draft")
    t.send("\x15")
    t.screen()
    key("\x1b[A\x1b[A", "echo second", "up twice")
    check("an old line can run", "second" in t.run(""), t.since())
    t.send("\x12sec")
    text = t.screen()[0]
    check("ctrl-r finds it", "(search)'sec': echo second" in text, text)
    t.send("\x7f\x7f\x7f")
    text = t.screen()[0]
    check("ctrl-r backspace", "(search)'': " in text, text)
    t.send("ir")
    text = t.screen()[0]
    check("ctrl-r narrows", "(search)'ir': echo third" in text, text)
    t.send("\x12")
    text = t.screen()[0]
    check("ctrl-r again goes older", "(search)'ir': echo first" in text, text)
    t.send("\x07")
    same("ctrl-g gives up", t.screen()[0][p:], "")
    t.send("\x12thi\r")
    check("enter runs the match", t.wait_for("third"), t.since())
    t.wait_prompt()
    t.send("\x12sec\x1b[D")
    t.pump(0.2)
    same("an arrow keeps it to edit", t.screen()[0][p:], "echo second")
    t.send("\x15")
    t.close()
    t, p = editing(box)
    t.send("\x1b[A")
    same("history survives a restart", t.screen()[0][p:], "echo third")
    t.close()
    lines = [f"echo {i}" for i in range(30)]
    piped(box, "log purge", *lines)
    same("log still shows 15", piped(box, "log")[0].splitlines(), lines[15:])
    t, p = editing(box)
    t.send("\x1b[A" * 30)
    same("the editor reaches further", t.screen()[0][p:], "echo 0")
    t.close()
    box.close()


def test_edit_jobs():
    box = Sandbox()
    t, p = editing(box)
    t.send("sleep 100\n")
    time.sleep(0.3)
    t.send("\x1a")
    check("ctrl-z reaches the job", t.wait_for("Stopped sleep 100"), t.out[-200:])
    t.wait_prompt()
    t.send("\x1a")
    text = t.screen()[0]
    same("ctrl-z at the prompt does nothing", text[p:], "")
    t.send("fg\n")
    time.sleep(0.3)
    t.send("\x03")
    check("ctrl-c reaches the job", t.wait_prompt(), t.since())
    modes = t.run("stty -a")
    check("programs get a normal terminal", re.search(r"(?<![-\w])icanon\b", modes), modes)
    t.send("\x04")
    code = t.wait_exit()
    check("ctrl-d on an empty line logs out", "logout" in t.out and code == 0, code)
    t.close()
    box.close()


def test_completion():
    box = Sandbox()
    os.makedirs(box.path("src/deep"))
    box.write("src/alpha.c", ""); box.write("src/beta.c", ""); box.write("readme.md", "")
    box.write("two words.txt", ""); box.write(".secret", "")
    box.write("bin/zzmytool", "#!/bin/sh\necho ran-my-tool\n")
    os.chmod(box.path("bin/zzmytool"), 0o755)
    box.write("bin/zzother", "")
    box.env["PATH"] = box.path("bin") + ":" + box.env["PATH"]
    box.env["ZQVAR"] = "1"
    t, p = editing(box)
    def tab(text, want, name):
        t.send("\x15" + text + "\t")
        same(name, t.screen()[0][p:], want)
    tab("zzm", "zzmytool ", "a program")
    tab("zzo", "zzo", "not executable")
    tab("activ", "activities ", "a builtin")
    tab("export Z", "export Z", "unknown leaves it")
    tab("cat rea", "cat readme.md ", "a file")
    tab("cat sr", "cat src/", "a directory gets a slash")
    tab("cat src/a", "cat src/alpha.c ", "inside a directory")
    tab("cat src/", "cat src/", "several in a directory")
    check("choices are listed", "alpha.c" in t.out or "src/alpha.c" in t.out, t.out[-200:])
    tab("cat tw", "cat two\\ words.txt ", "a space is escaped")
    tab("cat two\\ w", "cat two\\ words.txt ", "after an escaped space")
    tab("cat .se", "cat .secret ", "hidden when asked")
    tab("cat ~/rea", "cat ~/readme.md ", "after ~")
    tab("echo $ZQ", "echo $ZQVAR", "a variable")
    tab("true | zzm", "true | zzmytool ", "after a pipe")
    tab("true ; activ", "true ; activities ", "after ;")
    tab("cat < rea", "cat < readme.md ", "after <")
    t.send("\x15cat rea\t\x1b[H\x1b[C\x1b[C")
    same("completion then move", t.screen()[0][p:], "cat readme.md ")
    out = t.run("\x15zzm\t")
    check("a completed command runs", "ran-my-tool" in out, out)
    t.close()
    box.close()


def test_fuzz_keys():
    """Random keys at the prompt. Enter and Ctrl-D are never sent, so nothing
    typed can run and the shell cannot exit: it only has to survive."""
    import random
    box = Sandbox()
    os.makedirs(box.path("sub dir"))
    box.write("sub dir/file one", "")
    t = Term(box, cols=40)
    t.wait_prompt()
    rng = random.Random(99)
    keys = [bytes([c]) for c in range(1, 128) if c not in (3, 4, 10, 13, 26)]
    keys += [b"\x1b[A", b"\x1b[B", b"\x1b[C", b"\x1b[D", b"\x1b[H", b"\x1b[F", b"\x1b[3~", b"\x1b[1;5C",
             b"\x1b[99;99Z", b"\x1bb", b"\x1bf", b"\t", b"\t", b"\x12", b"\x7f", b"\x7f",
             "é".encode(), "日本".encode(), b"\xff", b"\xc3", b"sub", b"$", b"~/", b"\\"]
    alive = True
    for _ in range(60):
        data = b"".join(rng.choice(keys) for _ in range(120))
        try:
            os.write(t.fd, data)
        except OSError:
            alive = False
            break
        if not t.pump(0.1):
            alive = False
            break
    check("20000-odd random keys", alive and "Sanitizer" not in t.out and "runtime error" not in t.out, t.out[-600:])
    t.send("\x07\x15")
    t.pump(0.3)
    out = t.run("echo still-alive")
    check("still works after them", "still-alive" in out, out)
    t.close()
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
    same("strict: quotes are plain", piped(box, 'echo "a  b"', **strict)[0], '"a b"\n')
    same("strict: $ is plain", piped(box, "echo $HOME", **strict)[0], "$HOME\n")
    same("strict: * is plain", piped(box, "echo *", **strict)[0], "*\n")
    same("strict: # is plain", piped(box, "echo #x", **strict)[0], "#x\n")
    same("strict: = is plain", piped(box, "A=1", **strict)[1], "Command not found!\n")
    box.close()


def test_quoting():
    box = Sandbox()
    box.write("args.py", "import sys\nfor a in sys.argv[1:]:\n    print('[' + a + ']')\n")
    def args(line):
        return piped(box, "python3 args.py " + line)[0]
    same("double quotes keep spaces", args('"a  b" c'), "[a  b]\n[c]\n")
    same("single quotes keep spaces", args("'a  b' c"), "[a  b]\n[c]\n")
    same("backslash keeps a space", args("a\\ b c"), "[a b]\n[c]\n")
    same("quotes join", args('a"b"\'c\'d'), "[abcd]\n")
    same("empty quotes are an argument", args('"" x \'\''), "[]\n[x]\n[]\n")
    same("operators in quotes are plain", args("'a|b;c&d>e'"), "[a|b;c&d>e]\n")
    same("backslash in double quotes", args(r'"a\"b\\c\$d\e"'), '[a"b\\c$d\\e]\n')
    same("single quotes hide all", args("'$HOME \\ \"'"), '[$HOME \\ "]\n')
    for bad in ['echo "a', "echo 'a", "echo $(a", 'echo "$(a"']:
        same(f"open quote {bad!r}", piped(box, bad)[1], "Invalid Syntax!\n")
    same("comment", piped(box, "echo a # b ; echo c")[0], "a\n")
    same("a # in a word is plain", piped(box, "echo a#b")[0], "a#b\n")
    same("a quoted # is plain", piped(box, "echo '#b'")[0], "#b\n")
    same("a comment alone", piped(box, "# nothing", "echo x")[0], "x\n")
    box.close()


def test_expansion():
    box = Sandbox()
    box.write("args.py", "import sys\nfor a in sys.argv[1:]:\n    print('[' + a + ']')\n")
    box.write("x1.c", ""); box.write("x2.c", ""); box.write("y.h", ""); box.write(".hide.c", "")
    def args(*lines):
        return piped(box, *lines[:-1], "python3 args.py " + lines[-1])[0]
    same("variable", args("A=hello", "$A"), "[hello]\n")
    same("braces", args("A=hello", "${A}x"), "[hellox]\n")
    same("unset is nothing", args("$NOPE"), "")
    same("unset in quotes is empty", args('"$NOPE"'), "[]\n")
    same("unquoted splits", args("A='a  b'", "$A"), "[a]\n[b]\n")
    same("quoted does not split", args("A='a  b'", '"$A"'), "[a  b]\n")
    same("split joins with what is beside it", args("A='a b'", "x${A}y"), "[xa]\n[by]\n")
    same("assignment does not split", args("A='a b'", "B=$A", '"$B"'), "[a b]\n")
    same("a lone $", args("$ a$"), "[$]\n[a$]\n")
    same("$?", piped(box, "false", "echo $?", "true", "echo $?")[0], "1\n0\n")
    same("$? after not found", piped(box, "no_such_x", "echo $?")[0], "127\n")
    same("$? after syntax", piped(box, "| a", "echo $?")[0], "2\n")
    same("$? is read when it runs", piped(box, "false ; echo $?")[0], "1\n")
    out = piped(box, "echo $$ $(echo $$)")[0].split()
    check("$$ is the same in a substitution", len(out) == 2 and out[0] == out[1] and out[0].isdigit(), out)
    same("a setting succeeds", piped(box, "false", "A=1", "echo $?")[0], "0\n")
    same("a setting can read the old $?", piped(box, "false", "A=$?", "echo $A")[0], "1\n")
    same("a substitution alone gives its status", piped(box, "$(false)", "echo $?")[0], "1\n")
    out = piped(box, "sleep 0 &", "echo $!")[0]
    check("$! is the last job", re.search(r"\[1\] (\d+)\n\1\n", out), out)
    same("substitution", args("$(echo hi)"), "[hi]\n")
    same("substitution splits", args("$(echo a b)"), "[a]\n[b]\n")
    same("quoted substitution does not", args('"$(echo a b)"'), "[a b]\n")
    same("trailing newlines go", args('"$(printf a; echo; echo)"'), "[a]\n")
    same("nested", args('$(echo $(echo deep))'), "[deep]\n")
    same("with a pipe", args("$(echo hi | tr a-z A-Z)"), "[HI]\n")
    same("with parens in quotes", args("$(echo ')')"), "[)]\n")
    same("its status", piped(box, "X=$(false)", "echo $?")[0], "1\n")
    same("it sees this shell's vars", args("A=7", "$(echo $A)"), "[7]\n")
    same("settings in it stay in it", args("$(A=8)", '"$A"'), "[]\n")
    same("* matches", args("*.c"), "[x1.c]\n[x2.c]\n")
    same("? matches", args("x?.c"), "[x1.c]\n[x2.c]\n")
    same("[] matches", args("x[2].c"), "[x2.c]\n")
    same("no match stays", args("*.zzz"), "[*.zzz]\n")
    same("quoted * stays", args('"*.c"'), "[*.c]\n")
    same("escaped * stays", args("\\*.c"), "[*.c]\n")
    same("no glob on quoted part", args('x"?".c'), "[x?.c]\n")
    same("variable globs", args("A='*.h'", "$A"), "[y.h]\n")
    same("~", args("~"), f"[{box.dir}]\n")
    same("~/x", args("~/x"), f"[{box.dir}/x]\n")
    same("~x stays", args("~x"), "[~x]\n")
    same("quoted ~ stays", args("'~'"), "[~]\n")
    same("setting for one command", piped(box, "A=1", "A=2 printenv A", "echo $A")[0], "2\n1\n")
    same("settings reach a pipeline", piped(box, "A=2 printenv A | cat")[0], "2\n")
    same("an empty command is fine", piped(box, "$NOPE", "echo $?")[0], "0\n")
    piped(box, "F=made.txt", "echo x > $F")
    same("redirect to a variable", box.read("made.txt"), "x\n")
    piped(box, "F='a file'", 'echo x > "$F"')
    same("redirect to a name with a space", box.read("a file"), "x\n")
    piped(box, "$NOPE > empty.txt")
    same("redirect with no command", box.read("empty.txt"), "")
    same("job text is as typed", re.findall(r": (.*) - Running", piped(box, 'sleep 30 "$NOPE" &', "activities")[0])[0:1], ['sleep 30 "$NOPE"'])
    same("many fields", piped(box, "A=$(seq 1 3000)", "echo $A | wc -w")[0].strip(), "3000")
    for odd in ['echo "${a"', 'echo "${a"b"}"', "echo ${a", "echo '${a'", 'echo "$(echo "x")"', 'echo \\', 'echo "\\']:
        out, err, code = piped(box, odd, "echo still here")
        check(f"survives {odd!r}", code == 0 and "still here" in out and "Sanitizer" not in err, (out, err))
    box.close()


def test_fuzz_words():
    """Random lines must neither crash the shell nor hang it. Commands are only
    ever a or b, which do not exist, and the cwd is a scratch directory."""
    import random
    box = Sandbox()
    rng = random.Random(1234)
    pieces = list("ab ") + ['"', "'", "\\", "$", "(", ")", "{", "}", "*", "?", "[", "]", "#", ";", "|", "&",
                            "<", ">", "~", "=", "$(", "${", "&&", "||", ">>", "$?", "$$"]
    lines = ["".join(rng.choice(pieces) for _ in range(rng.randint(1, 14))) for _ in range(1500)]
    for start in range(0, len(lines), 50):
        part = lines[start:start + 50]
        out, err, code = piped(box, *part, "echo end-of-batch", timeout=60)
        if code != 0 or "end-of-batch" not in out or "Sanitizer" in err or "runtime error" in err:
            check("random lines", False, (part, code, err[-500:]))
            break
    else:
        check("1500 random lines", True)
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
    box.write("both.sh", "echo out\necho err >&2\n")
    out, err, _ = piped(box, "sh both.sh 2> e")
    same("2> takes stderr", (out, err, box.read("e")), ("out\n", "", "err\n"))
    piped(box, "sh both.sh 2>> e")
    same("2>> appends", box.read("e"), "err\nerr\n")
    out, err, _ = piped(box, "sh both.sh > all 2>&1")
    same("2>&1 after > goes to the file", (out, err, box.read("all")), ("", "", "out\nerr\n"))
    out, err, _ = piped(box, "sh both.sh 2>&1 > only")
    same("2>&1 before > stays on stdout", (out, err, box.read("only")), ("err\n", "", "out\n"))
    out, err, _ = piped(box, "sh both.sh 2>&1 | tr a-z A-Z")
    same("2>&1 in a pipe", (sorted(out.split()), err), (["ERR", "OUT"], ""))
    out, err, _ = piped(box, "echo x >&2")
    same(">&2", (out, err), ("", "x\n"))
    same("a builtin's stderr", (piped(box, "hop nowhere 2> be")[1], box.read("be")), ("", "No such directory!\n"))
    same("stderr comes back after a builtin", piped(box, "hop nowhere 2> be", "hop nowhere")[1], "No such directory!\n")
    same("a lone 2 is an argument", (piped(box, "echo 2 > two"), box.read("two"))[1], "2\n")
    same("12> is an argument", (piped(box, "echo 12> tw"), box.read("tw"))[1], "12\n")
    same("strict has no 2>", (piped(box, "echo 2> s", args=["--strict"]), box.read("s"))[1], "2\n")
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
