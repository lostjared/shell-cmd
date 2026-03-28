# shell-cmd — Complete Guide

## What Is shell-cmd?

`shell-cmd` is a command-line utility written in C++20 that **recursively walks a directory tree**, finds files matching a **regex pattern**, and **executes a shell command for each match**. Think of it as a more ergonomic alternative to chaining `find` with `-exec` — you write a command template with numbered placeholders (`%0`, `%1`, `%2`, …) and `shell-cmd` fills them in and runs the command for every matched file.

---

## How the Program Works (Internals)

### High-Level Flow

```
1. Parse command-line options and positional arguments
2. Recursively walk the target directory
3. For each file whose full path matches the regex:
   a. Substitute placeholders in the command template
   b. Fork a child process and execute the command via /bin/sh
```

### Argument Parsing

`shell-cmd` uses a custom header-only argument parser (`argz.hpp`). It separates **options** (`-n`, `-v`, `-a`, `-d`, `-h`) from **positional arguments**. The positional arguments are, in order:

| Position | Meaning |
|----------|---------|
| 1st | **Path** — the root directory to search |
| 2nd | **Command template** — shell command with `%` placeholders |
| 3rd | **Regex** — ECMAScript regex matched against the full file path |
| 4th+ | **Extra arguments** — substituted into `%2`, `%3`, etc. |

If fewer than three positional arguments are given, the program prints an error and the help text.

### Directory Traversal

The function `add_directory()` walks the directory tree using `std::filesystem::directory_iterator`. Key behaviors:

- **Hidden files/directories** (names starting with `.`) are **skipped by default**. Pass `-a` to include them.
- **Depth limiting** — if `-d N` is specified, recursion stops after `N` levels (0 means only the given directory, no subdirectories).
- **Permission errors** are handled gracefully; directories that can't be opened produce an error message and exit.
- **Symlinks** are not explicitly followed — standard `directory_iterator` behavior applies.

For every regular file whose full path matches the regex, the program calls `proc_cmd()`.

### Placeholder Substitution

Inside `proc_cmd()`, the command template string is scanned and placeholders are replaced:

| Placeholder | Replaced With |
|-------------|---------------|
| `%0` | The **filename only** (no directory path), extracted via `std::filesystem::path::filename()` |
| `%1` | The **full path** to the matched file |
| `%2`, `%3`, … | The **extra arguments** passed after the regex on the command line |

If the full path for `%1` contains spaces, it is automatically wrapped in double quotes to prevent word-splitting by the shell.

### Command Execution

Commands are executed through a custom `System()` function. Rather than calling the standard library's `system()`, this implementation:

1. **Forks** a child process.
2. **Blocks `SIGCHLD`** and **ignores `SIGINT`/`SIGQUIT`** in the parent so the parent isn't accidentally killed by Ctrl+C.
3. Runs the command via `execl("/bin/sh", "sh", "-c", command, …)` in the child.
4. **Waits** for the child to finish, then restores signal masks.

This gives reliable process management and prevents interrupted batch operations from leaving the parent in a bad state.

---

## Building from Source

### Prerequisites

- A C++20-capable compiler (GCC 13+ or Clang 16+)
- CMake 3.10+ (for the CMake build)

### CMake Build (recommended)

```bash
cd shell-cmd
mkdir -p build && cd build
cmake ..
make
```

The compiled binary is at `build/shell-cmd`.

### System-Wide Install

```bash
cd build
sudo make install
```

This copies the binary to `/usr/local/bin` (or the system default `bin` directory).

### Alternative: Plain Makefile

```bash
make -f Makefile.cmd
sudo make -f Makefile.cmd install
```

---

## Usage Synopsis

```
shell-cmd [options] <path> "<command %1 [%2 %3..]>" <regex> [extra_args..]
```

### Options

| Flag | Description |
|------|-------------|
| `-n` | **Dry-run** — print each command but don't execute it |
| `-v` | **Verbose** — print each command before executing it |
| `-a` | **All files** — include hidden files and directories |
| `-d N` | **Max depth** — limit recursion (0 = current directory only) |
| `-h` | **Help** — show usage information |

---

## Concrete Real-Life Examples

### 1. Count Lines of Code in a Project

You want to see line counts for every `.py` file in your project:

```bash
shell-cmd . "wc -l %1" ".*\.py$"
```

**What happens:** Every `.py` file found recursively under `.` is passed to `wc -l`. Output looks like:

```
  42 ./src/main.py
 118 ./src/utils.py
  27 ./tests/test_main.py
```

### 2. Preview Before You Act (Dry-Run)

You want to auto-format all C/C++ source files, but first check what would run:

```bash
shell-cmd -n ./src "clang-format -i %1" ".*\.(c|cpp|h|hpp)$"
```

Output (nothing is executed):

```
clang-format -i ./src/main.cpp
clang-format -i ./src/parser.hpp
clang-format -i ./src/utils.c
```

When satisfied, remove `-n` to actually format the files:

```bash
shell-cmd ./src "clang-format -i %1" ".*\.(c|cpp|h|hpp)$"
```

### 3. Batch Resize Photos

Resize every JPEG in your photo library to 1920×1080 using ImageMagick:

```bash
shell-cmd ~/Photos "convert %1 -resize 1920x1080 /tmp/resized/%0" ".*\.jpe?g$"
```

- `%1` → `/home/you/Photos/vacation/sunset.jpg` (the source)
- `%0` → `sunset.jpg` (used to name the output file)

The regex `jpe?g` matches both `.jpg` and `.jpeg`.

### 4. Back Up Log Files to Another Directory

Copy all `.log` files to a backup folder, preserving filenames:

```bash
shell-cmd /var/log "cp %1 %2/%0" ".*\.log$" /mnt/backup/logs
```

- `%1` → full path to each log file
- `%2` → `/mnt/backup/logs` (the extra argument)
- `%0` → the filename only

### 5. Search for TODO Comments Across a Codebase

```bash
shell-cmd . "grep -Hn 'TODO' %1" ".*\.(js|ts|py|cpp)$"
```

This runs `grep` with line numbers on every source file. Example output:

```
./src/api.ts:45:    // TODO: add rate limiting
./src/db.py:112:    # TODO: handle connection timeout
```

### 6. Convert All Markdown Files to PDF

Using Pandoc to generate PDFs from your documentation:

```bash
shell-cmd ~/docs "pandoc %1 -o /tmp/pdfs/%0.pdf" ".*\.md$"
```

Each Markdown file becomes a PDF in `/tmp/pdfs/`.

### 7. Transcode WAV Audio to MP3

```bash
shell-cmd ~/recordings "ffmpeg -i %1 -b:a 192k /tmp/mp3/%0.mp3" ".*\.wav$"
```

### 8. Validate Shell Scripts Without Running Them

Check syntax of all `.sh` files with verbose output:

```bash
shell-cmd -v . "bash -n %1" ".*\.sh$"
```

`-v` prints each command as it runs, so you see:

```
bash -n ./deploy.sh
bash -n ./setup.sh
bash -n ./scripts/cleanup.sh
```

If any script has a syntax error, `bash -n` will report it.

### 9. Extract All tar.gz Archives

```bash
shell-cmd ~/Downloads "tar xzf %1 -C /tmp/extracted" ".*\.tar\.gz$"
```

### 10. Strip EXIF Metadata Before Sharing Photos

```bash
shell-cmd ./photos "exiftool -all= %1" ".*\.(jpg|png)$"
```

### 11. Work Only in the Current Directory (No Recursion)

Limit depth to 0 so only files directly in the given path are processed:

```bash
shell-cmd -d 0 . "cat %1" ".*\.txt$"
```

### 12. Include Hidden Config Files

Normally `shell-cmd` skips dotfiles. Use `-a` to include them:

```bash
shell-cmd -a ~ "cat %1" ".*\.bashrc|.*\.zshrc"
```

### 13. Combine Multiple Options

Preview commands, include hidden files, limit depth to 2 levels:

```bash
shell-cmd -n -a -d 2 ~ "wc -l %1" ".*rc$"
```

Output (dry-run, nothing executed):

```
wc -l /home/you/.bashrc
wc -l /home/you/.config/i3/config  (skipped — depth > 2 or no match)
wc -l /home/you/.zshrc
```

### 14. Compile Every C File in a Project

```bash
shell-cmd ./src "gcc -c %1 -o /tmp/%0.o" ".*\.c$"
```

- `%1` gives `gcc` the full source path
- `%0.o` names the output object file after the source filename

### 15. Sign All RPM Packages in a Repository

```bash
shell-cmd ./packages "rpm --addsign %1" ".*\.rpm$"
```

### 16. Using Multiple Extra Arguments

You can pass multiple extra arguments after the regex. Each maps to `%2`, `%3`, etc.:

```bash
shell-cmd . "cp %1 %2/%0 && echo 'copied to %3'" ".*\.conf$" /backup user@host
```

- `%2` → `/backup`
- `%3` → `user@host`

The program validates that every extra argument has a corresponding placeholder in the command template. If you pass an extra argument but the command doesn't reference its placeholder, `shell-cmd` exits with an error.

---

## Placeholder Quick Reference

| Placeholder | Value |
|-------------|-------|
| `%0` | Filename only (e.g., `report.txt`) |
| `%1` | Full path (e.g., `/home/user/docs/report.txt`) |
| `%2` | First extra argument after the regex |
| `%3` | Second extra argument after the regex |
| `%N` | Nth extra argument (no upper limit) |

---

## Tips and Best Practices

1. **Always dry-run first.** Use `-n` before running destructive commands (`rm`, `mv`, overwriting files) to verify what will execute.

2. **Quote the command template.** Since it contains `%` placeholders and often shell metacharacters, always wrap it in double quotes: `"command %1"`.

3. **Escape regex special characters.** The regex uses ECMAScript syntax. To match a literal dot in file extensions, use `\.` — e.g., `".*\.cpp$"` not `".*cpp$"` (the latter also matches `acpp`).

4. **Use `%0` for output filenames.** When copying/converting files to a new directory, `%0` gives you the original filename without the source path, which is ideal for naming outputs.

5. **Combine `-v` with normal execution** to watch progress on long-running batch jobs without doing a separate dry-run pass.

6. **Depth control is useful for large trees.** If you only want files in `src/` and its immediate children, use `-d 1`.

---

## Error Handling

| Scenario | Behavior |
|----------|----------|
| Fewer than 3 positional arguments | Prints error + help text, exits |
| Directory can't be opened | Prints error with path, exits |
| Extra argument has no matching placeholder | Prints error naming the missing `%N`, exits |
| Command fails (non-zero exit) | The next file is still processed (no abort-on-error) |
| Invalid regex | `std::regex` throws an exception, program crashes with an unhandled exception |

---

## License

`shell-cmd` is released under the **GNU GPL v3**. See the [LICENSE](LICENSE) file for details.
