# shell-cmd

A fast, practical command-line utility for recursively finding files, filtering them, and running a shell command for each match.

`shell-cmd` is designed to feel simpler and easier to remember than `find -exec` or `xargs`, while still being powerful enough for real day-to-day work. It supports regex matching, glob matching, exclude patterns, metadata filters, dry runs, confirmations, parallel jobs, and command placeholders.

## Highlights

- Recursive file discovery with `std::filesystem`
- Regex search mode and regex full-match mode
- Glob pattern mode for shell-style matching
- Regex or glob excludes
- Dry-run mode for safe previews
- Verbose output
- Hidden file support
- Max depth control
- Size, mtime, permissions, owner, group, and type filters
- Per-file execution or one-shot list-all execution
- Parallel jobs
- Confirmation mode
- Stop-on-error mode
- Simple placeholders like `%0`, `%1`, `%b`, and `%e`
- Configurable shell for command execution

## Why use it?

If you ever forget the exact syntax for `find ... -exec` or how to wire things through `xargs`, `shell-cmd` gives you a cleaner mental model:

1. walk a directory tree
2. keep the files you want
3. substitute placeholders into a command template
4. run the command

That makes common tasks much easier to read and reuse.

## Build

Requires a C++20 compiler. The repository README currently documents GCC 13+ or Clang 16+ as the expected baseline.

### Build with CMake

```bash
mkdir -p build
cd build
cmake ..
make
```

### Install system-wide

```bash
sudo make install
```

The repository includes `CMakeLists.txt`, `Makefile.cmd`, `README.md`, `GUIDE.md`, and `EXAMPLES.md`.

## Usage

```bash
shell-cmd [options] path "command %1 [%2 %3..]" pattern [extra_args..]
```

### Positional arguments

- `path` — root directory to search
- `command` — shell command template to execute
- `pattern` — the main match pattern
- `extra_args..` — optional extra values available through `%2`, `%3`, and so on

## Matching modes

`shell-cmd` supports three main ways to match files.

### 1. Regex search mode (default)

The main pattern is treated as a regex and searched within the path.

```bash
shell-cmd . "echo %1" ".*\\.cpp$"
```

### 2. Regex full-match mode

Use `--regex-match` to require the regex to match the entire path. The built-in help describes this as using `regex_match` instead of search.

```bash
shell-cmd . "echo %1" --regex-match ".*\.(cpp|hpp)"
```

### 3. Glob mode

Use `--glob` to treat the main pattern as a glob. The program converts glob syntax to anchored regex internally, including support for `*`, `?`, and character classes like `[ch]`.

```bash
shell-cmd . "echo %1" --glob "*.cmake"
shell-cmd . "echo %1" --glob "*.[ch]pp"
```

## Exclude patterns

Use `-x` or `--exclude` to skip paths.

By default, excludes use regex semantics. The built-in help describes `-x` as excluding files or directories matching a regex, and `--glob-exclude` switches the exclude pattern to glob mode.

### Regex exclude

```bash
shell-cmd . "echo %1" --regex-match ".*\.(cpp|hpp)" -x "build|CMakeFiles|third_party" --dry-run
```

### Glob exclude

```bash
shell-cmd . "echo %1" --glob "*.[ch]pp" --glob-exclude -x "*bu*" --dry-run
```

## Placeholders

The built-in help and repository README define these placeholders:

| Placeholder | Meaning |
|---|---|
| `%0` | filename only in per-match mode |
| `%1` | full path to matched file |
| `%2+` | extra arguments from the command line |
| `%b` | basename without extension |
| `%e` | file extension including the dot |

When `-l` or `--list-all` is used, `%0` changes meaning and expands to all matched paths joined by spaces.

## Options

The following options are documented in the current repository README and built-in help. `--regex-match`, `--glob`, and `--glob-exclude` are documented in the program help and source.

| Short | Long | Description |
|---|---|---|
| `-z` | `--regex-match` | use full regex match instead of regex search |
| `-b` | `--glob` | treat the main pattern as a glob |
| `-n` | `--dry-run` | print commands without executing them |
| `-v` | `--verbose` | print each command before running it |
| `-a` | `--all` | include hidden files and directories |
| `-l` | `--list-all` | collect all matches and invoke the command once |
| `-d N` | `--depth N` | maximum recursion depth, `0` means current directory only |
| `-s SIZE` | `--size SIZE` | filter by file size |
| `-m DAYS` | `--mtime DAYS` | filter by modification time |
| `-p MODE` | `--perm MODE` | filter by octal permissions, such as `755` |
| `-u USER` | `--user USER` | filter by owner username |
| `-g GROUP` | `--group GROUP` | filter by group name |
| `-t TYPE` | `--type TYPE` | filter by type: `f`, `d`, or `l` |
| `-x PATTERN` | `--exclude PATTERN` | exclude matching files or directories |
| `-i` | `--glob-exclude` | treat the exclude pattern as a glob |
| `-e` | `--stop-on-error` | stop after the first command failure |
| `-c` | `--confirm` | ask before each command |
| `-j N` | `--jobs N` | run commands in parallel |
| `-w SHELL` | `--shell SHELL` | shell used for execution, default `/bin/bash` |
| `-h` | `--help` | show help |

## Filter syntax

### Size filter

Examples documented in the repo:

- `+10M` — larger than 10 MB
- `-1K` — smaller than 1 KB
- `4096` — exactly 4096 bytes

Suffixes supported in the README/help are `K`, `M`, and `G`.

### Modification time filter

Examples documented in the repo/help:

- `+7` — older than 7 days
- `-1` — modified within the last day
- `3` — exactly 3 days, according to the built-in help text

### Type filter

- `f` — regular file
- `d` — directory
- `l` — symlink

## Examples

### Preview formatting C++ files

```bash
shell-cmd . "clang-format -i %1" --glob "*.[ch]pp" -x "build|CMakeFiles|third_party" --dry-run
```

### Preview formatting C++ files with full regex matching

```bash
shell-cmd . "clang-format -i %1" --regex-match ".*\.(cpp|hpp|cc|hh)" -x "build|CMakeFiles|third_party" --dry-run
```

### Count lines in all `.cpp` files

```bash
shell-cmd . "wc -l %1" ".*\.cpp$"
```

### Copy matched files to a backup directory

```bash
shell-cmd . "cp %1 /tmp/backup/%0" ".*\.txt$"
```

### Use basename without extension

```bash
shell-cmd ~/music "ffmpeg -i %1 /tmp/mp3/%b.mp3" ".*\.wav$"
```

### Limit search depth

```bash
shell-cmd -d 0 . "cat %1" ".*\.md$"
```

### Include hidden files

```bash
shell-cmd -a ~ "echo %1" ".*\.bashrc"
```

### Run one command for every matched path as a single list

```bash
shell-cmd -l . "ls -l %0" ".*\.log$"
```

### Find large files

```bash
shell-cmd . "ls -lh %1" ".*" --size +10M
```

### Delete old temp files, but preview first

```bash
shell-cmd --dry-run /tmp "rm %1" ".*\.tmp$" --mtime +30
```

### Filter by permissions

```bash
shell-cmd . "echo %1" ".*" --perm 755 --type f
```

### Filter by owner

```bash
shell-cmd /etc "echo %1" ".*\.conf$" --user root
```

### Filter only directories

```bash
shell-cmd . "echo %1" ".*src.*" --type d
```

### Combine filters

```bash
shell-cmd /var/log "wc -l %1" ".*\.log$" -s +1M -m -7
```

### Parallel image processing

```bash
shell-cmd -j 4 ./images "convert %1 -resize 800x600 /tmp/thumbs/%0" ".*\.jpg$"
```

### Confirm destructive commands

```bash
shell-cmd -c /tmp "rm %1" ".*\.bak$"
```

### Stop on first error

```bash
shell-cmd -e ./src "gcc -c %1 -o /tmp/%b.o" ".*\.c$"
```

### Pass extra arguments

```bash
shell-cmd . "cp %1 %2/%0" ".*\.log$" /tmp/logs
```

## How it works

The current README explains that `shell-cmd` recursively walks the target directory with `std::filesystem`, checks each path against the main pattern and metadata filters, substitutes placeholders in the command template, and executes the result via the configured shell, which defaults to `/bin/bash`. Hidden files and directories are skipped unless enabled. In list-all mode, it gathers all matched paths and invokes the command once.

The source also shows that commands are executed through the configured shell and that parallel mode uses `fork()`/`execl()` when `--jobs` is greater than 1.

## Notes on quoting

Because commands are executed through a shell, quoting still matters.

A few practical tips:

- Wrap the command template in quotes.
- Quote regex patterns that contain shell-sensitive characters.
- Use `--dry-run` first when testing a new command.
- Prefer `%1` when the full relative path matters.
- Use `%0` when you want the filename only.

## shell-cmd vs find -exec

The repository README compares `shell-cmd` with `find -exec`: `shell-cmd` adds built-in placeholders, dry-run, verbose output, exclude support, parallel jobs, confirmation mode, stop-on-error, and summary statistics, while `find` remains more universal and more expressive for complex boolean filtering.

A simple side-by-side example:

```bash
# shell-cmd
shell-cmd . "cp %1 /tmp/backup/%0" ".*\.txt$"

# find equivalent
find . -regex '.*\.txt$' -exec sh -c 'cp "$1" "/tmp/backup/$(basename "$1")"' _ {} \;
```

## Project files

The repository currently contains at least these top-level files: `CMakeLists.txt`, `Doxyfile`, `EXAMPLES.md`, `GUIDE.md`, `LICENSE`, `Makefile.cmd`, `README.md`, `argz.hpp`, and `cmd.cpp`.

## Website

Project website: `lostsidedead.biz/shell-cmd` is linked from the GitHub repository page. 
## License

GPL-3.0
