# A job control shell in C

An interactive Unix shell written from scratch, with no libraries beyond libc: pipes, redirection, process groups and job control, quoting and expansion, scripts, and its own line editor with history search and tab completion. It began as part of the Operating Systems and Networks course at IIIT Hyderabad and goes well past the assignment.

```
make
./shell.out
```

```
<arun@mac:~> reveal -l src | wc -l
11
<arun@mac:~> sleep 100
^Z[1] Stopped sleep 100
<arun@mac:~> bg ; activities
[1] sleep 100 &
[8123] : sleep 100 - Running
<arun@mac:~> N=$(ls src/*.c | wc -l) && echo "$N files" > count.txt 2>&1 || echo failed
```

## What it does

| Area | Details |
|---|---|
| Commands | anything on `PATH`, or a path |
| Pipes and lists | `\|`, `;`, `&`, `&&`, `\|\|`; `a && b &` puts all of it in the background |
| Redirection | `<`, `>`, `>>`, `2>`, `2>>`, `2>&1`, `>&2`, applied left to right |
| Quoting | `'...'`, `"..."`, `\`, and `#` comments |
| Expansion | `$VAR`, `${VAR}`, `$?`, `$$`, `$!`, `$0` to `$9`, `$#`, `"$@"`, `$(command)`, `~`, `*`, `?`, `[...]` |
| Settings | `A=1`, `A=1 command` for one command only, `export`, `unset` |
| Job control | `fg`, `bg`, `activities`, Ctrl-Z, Ctrl-C; each job is a process group that owns the terminal while it is in front |
| Builtins | `hop` or `cd`, `reveal`, `log`, `activities`, `ping`, `fg`, `bg`, `export`, `unset`, `exit`, `source` or `.` |
| Scripts | `./shell.out script.sh args`, `./shell.out -c 'command'`, with the right exit status |

## The line editor

There is no readline here. The terminal is put in raw mode while a line is being typed, and the shell draws everything itself.

| Keys | Action |
|---|---|
| Left, Right, Home, End, Ctrl-A, Ctrl-E, Alt-B, Alt-F | move |
| Backspace, Delete, Ctrl-W, Ctrl-K, Ctrl-U | delete |
| Up, Down, Ctrl-P, Ctrl-N | history, up to 1000 lines kept across runs |
| Ctrl-R | search the history as you type |
| Tab | complete programs, builtins, files and `$VARIABLES` |
| Ctrl-C | abandon the line |
| Ctrl-D | leave, on an empty line |

## Job control, done properly

- Every job is its own process group, so Ctrl-C reaches the whole of `a | b | c` and never the shell.
- A background job that reads the terminal is stopped by the kernel instead of stealing your typing.
- A stopped program's terminal settings are saved and restored around it. Stop a program that turned off echo and your prompt still echoes, and `fg` gives the program back what it had. A program that exits normally keeps its changes, which is why `stty` works.

## The course's grammar

`--strict` turns all of the extensions off, leaving only what the course's grammar allowed. Run that way, the shell passes 121 of the 125 cases from a replay of the course's autograder. The other four disagree with the assignment itself:

- Tests 44 and 47 expect `hop a bad b` to look up `b` from where `hop` started. The assignment applies arguments one after another.
- Tests 64 and 66 expect `pwd` to be missing from the history, where the assignment says to store it.

In the default mode, 119 of the 125 pass with the editor turned off using `--no-edit`. The two extra failures are `a ;` and `a || b`, which that grammar rejected and this one accepts on purpose.

## Tests

```
make test
```

`tests/run.py` has 338 checks and needs only Python. They run against both a normal build and one with AddressSanitizer and UBSan, and pass on macOS and Linux.

- Line-by-line behaviour goes through pipes.
- Job control and editing go through a real pseudo-terminal, so Ctrl-Z is the terminal's own signal, and a small screen model checks what a person would actually see and where the cursor is.
- Two fuzz tests: 1500 random lines of shell syntax, and over 20000 random keys at the prompt, never including Enter, so that nothing typed can run.

## What it does not do

- No `if`, `while` or `for`, so scripts are straight lines plus `&&` and `||`.
- Every variable is exported.
- A `$(...)` in a background command runs before the job starts, so `echo $(sleep 5) &` holds the prompt for 5 seconds.
- `~` means the directory the shell started in, as the course defined it, and not `$HOME`.
- Completion does not look inside an open quote. Characters as wide as two columns, like CJK, count as one when moving the cursor.
