# shell-cmd

Recursively find files matching a regex pattern and execute a shell command for each match.

## Build

Requires C++20 (GCC 13+, Clang 16+).

```bash
mkdir -p build && cd build
cmake ..
make
```

Or install system-wide:

```bash
sudo make install
```

## Usage

```
shell-cmd [options] path "command %1 [%2 %3..]" regex [extra_args..]
```

### Placeholders

| Placeholder | Description |
|-------------|-------------|
| `%0` | Filename only (no path) |
| `%1` | Full path to matched file |
| `%2+` | Extra arguments from command line |
| `%b` | Basename without extension (e.g., `report` from `report.txt`) |
| `%e` | File extension including dot (e.g., `.txt`) |

### Options

| Short | Long | Description |
|-------|------|-------------|
| `-z` | `--regex-match` | Use `regex_match` (entire path must match) instead of the default `regex_search` (substring match) |
| `-b` | `--glob` | Treat the search pattern as a glob (`*`, `?`, `[...]`) instead of regex |
| `-i` | `--glob-exclude` | Treat the exclude pattern (`-x`) as a glob instead of regex |
| `-n` | `--dry-run` | Dry-run — print commands without executing |
| `-v` | `--verbose` | Verbose — print each command before running |
| `-a` | `--all` | Include hidden files and directories |
| `-l` | `--list-all` | Run one command with `%0` set to a space-delimited list of all matched paths |
| `-d N` | `--depth N` | Max recursion depth (0 = current directory only) |
| `-s SIZE` | `--size SIZE` | Filter by size: `+10M` (>10 MB), `-1K` (<1 KB), `4096` (exact). Suffixes: K, M, G |
| `-m DAYS` | `--mtime DAYS` | Filter by modification time: `+7` (older than 7 days), `-1` (within last day) |
| `-p MODE` | `--perm MODE` | Filter by permissions (octal), e.g. `755` |
| `-u USER` | `--user USER` | Filter by owner username |
| `-g GROUP` | `--group GROUP` | Filter by group name |
| `-t TYPE` | `--type TYPE` | Filter by type: `f` (file), `d` (directory), `l` (symlink) |
| `-x REGEX` | `--exclude REGEX` | Exclude files/directories matching REGEX (or a glob when combined with `-i`) |
| `-e` | `--stop-on-error` | Stop on first command failure |
| `-c` | `--confirm` | Prompt for confirmation before each command |
| `-j N` | `--jobs N` | Run N commands in parallel (default: 1) |
| `-w SHELL` | `--shell SHELL` | Shell to use for execution (default: `/bin/bash`) |
| `-h` | `--help` | Show help |

---

## Pattern Matching Modes

`shell-cmd` supports three independent switches that control how the search pattern and exclude pattern are interpreted. They can be combined freely.

### Default: Regex Search

By default, the third positional argument is an **ECMAScript regex** tested as a **substring search** against each file's full path. If the pattern appears anywhere in the path, the file matches.

```bash
# Matches any path containing ".cpp" — e.g. ./src/main.cpp, ./lib/foo.cpp
shell-cmd . "echo %1" "\.cpp"

# Anchor with $ to match only paths ending in .cpp
shell-cmd . "echo %1" "\.cpp$"

# Match .c, .cpp, .h, and .hpp files
shell-cmd . "echo %1" "\.(c|cpp|h|hpp)$"
```

Because this is a substring search, you do **not** need `.*` at the start of the pattern — `\.cpp$` is enough to match all paths ending in `.cpp`.

### `--regex-match` / `-z`: Full-Path Matching

With `-z`, the regex must match the **entire path** (equivalent to wrapping the pattern in `^...$`). This is useful when you want precise control:

```bash
# Only matches paths that are entirely ".*\.rs$" — same effect as default with anchors
shell-cmd -z . "echo %1" ".*\.rs$"

# Match files whose full path starts with ./src/ and ends with .cpp
shell-cmd -z . "echo %1" "\./src/.*\.cpp"
```

### `--glob` / `-b`: Glob Mode

With `-b`, write familiar shell wildcard patterns instead of regex. Glob metacharacters:

| Glob | Meaning | Regex equivalent |
|------|---------|-----------------|
| `*` | Match any number of characters | `.*` |
| `?` | Match exactly one character | `.` |
| `[abc]` | Match one of the listed characters | `[abc]` |
| `[!abc]` or `[^abc]` | Match any character not listed | `[^abc]` |

All other regex-special characters (`.`, `+`, `|`, `(`, `)`, etc.) are automatically escaped, so you never need backslashes.

The glob pattern is anchored — it must match the **entire** path (internally converted to `^...$`).

```bash
# Match all .cpp files
shell-cmd --glob . "echo %1" "*.cpp"

# Match all .c and .h files (character class)
shell-cmd --glob . "echo %1" "*.[ch]"

# Match .cpp and .hpp files
shell-cmd --glob . "echo %1" "*.[ch]pp"

# Match files starting with "test" and ending with .py
shell-cmd --glob . "echo %1" "*test*.py"
```

### Combining `--glob` with `--regex-match`

When both `-b` and `-z` are active, the glob is converted to regex and then full-path matching is applied. This is useful for matching the complete path with glob syntax:

```bash
shell-cmd --glob --regex-match . "echo %1" "*cmake*"
```

---

## Exclude Patterns

The `-x` / `--exclude` option skips files and directories whose **filename** (not full path) matches the given pattern. By default, the exclude pattern is a **regex** (substring search):

```bash
# Exclude any file/directory whose name contains "build", "CMakeFiles", or "third_party"
shell-cmd -x "build|CMakeFiles|third_party" . "echo %1" "\.cpp$"

# Exclude .git and node_modules
shell-cmd -x "node_modules|\.git" . "wc -l %1" "\.ts$"
```

### `--glob-exclude` / `-i`: Glob Exclude

Add `-i` to treat the `-x` pattern as a **glob** instead of regex. The glob is converted to an anchored regex internally, so it must match the entire filename:

```bash
# Exclude files/dirs whose name matches the glob "build*"
shell-cmd --glob -x "build*" --glob-exclude . "echo %1" "*.cpp"

# Exclude object files
shell-cmd --glob -x "*.o" -i . "echo %1" "*.c"
```

### Mixing Regex Exclude with Glob Search

The `-x` pattern and the search pattern are **independent** — you can use `--glob` for the search pattern while keeping `-x` as a regex (the default), or vice versa:

```bash
# Glob search pattern, regex exclude pattern (no -i needed)
shell-cmd --glob -x "build|CMakeFiles|third_party" . "clang-format -i %1" "*.[ch]pp"

# Regex search pattern, glob exclude pattern (use -i)
shell-cmd -x "build*" -i . "echo %1" "\.cpp$"
```

---

## Examples

### Basic Usage

Count lines in all `.cpp` files:

```bash
shell-cmd . "wc -l %1" "\.cpp$"
```

Dry-run to preview what would be executed:

```bash
shell-cmd -n . "clang-format -i %1" "\.(cpp|hpp)$"
```

Copy matched files to a destination, using filename-only placeholder:

```bash
shell-cmd . "cp %1 /tmp/backup/%0" "\.txt$"
```

### Depth and Hidden Files

Limit search to current directory (no recursion):

```bash
shell-cmd -d 0 . "cat %1" "\.md$"
```

Include hidden files:

```bash
shell-cmd -a ~ "echo %1" "\.bashrc"
```

### Extra Arguments

Use extra arguments — `%2` is replaced with the value passed after the regex:

```bash
shell-cmd . "cp %1 %2/%0" "\.log$" /tmp/logs
```

Multiple extra arguments:

```bash
shell-cmd . "cp %1 %2/%0 && echo 'copied to %3'" "\.conf$" /backup user@host
```

### Basename and Extension Placeholders

Convert WAV to MP3, using `%b` for the output filename without extension:

```bash
shell-cmd ~/music "ffmpeg -i %1 /tmp/mp3/%b.mp3" "\.wav$"
```

Organize files by extension:

```bash
shell-cmd -n . "mkdir -p /tmp/by-ext/%e && cp %1 /tmp/by-ext/%e/%0" ".*"
```

### List-All Mode

Run a single batch command with all matches:

```bash
shell-cmd -l . "wc -l %0" "\.cpp$"
```

In this mode, `%0` is substituted with a single space-separated string containing every matched path.

### Metadata Filters

Find large files (over 10 MB):

```bash
shell-cmd . "ls -lh %1" ".*" --size +10M
```

Delete files older than 30 days, with dry-run:

```bash
shell-cmd --dry-run /tmp "rm %1" "\.tmp$" --mtime +30
```

Find executable files (permission 755):

```bash
shell-cmd . "echo %1" ".*" --perm 755 --type f
```

List files owned by root:

```bash
shell-cmd /etc "echo %1" "\.conf$" --user root
```

List only directories matching a pattern:

```bash
shell-cmd . "echo %1" "src" --type d
```

Combine filters — large `.log` files modified recently:

```bash
shell-cmd /var/log "wc -l %1" "\.log$" -s +1M -m -7
```

### Glob Mode Examples

Match all C/C++ source and header files:

```bash
shell-cmd --glob . "echo %1" "*.[ch]pp"
```

Format C/C++ files, excluding build directories:

```bash
shell-cmd --glob -x "build|CMakeFiles" . "clang-format -i %1" "*.[ch]pp"
```

Format C/C++ files, excluding with a glob exclude pattern:

```bash
shell-cmd --glob -x "build*" --glob-exclude . "clang-format -i %1" "*.[ch]pp"
```

Match files with single-character extensions:

```bash
shell-cmd --glob . "echo %1" "*.?"
```

### Parallel Execution

Run commands in parallel with 4 jobs:

```bash
shell-cmd -j 4 ./images "convert %1 -resize 800x600 /tmp/thumbs/%0" ".*\.jpg$"
```

### Safety Options

Confirm before each destructive command:

```bash
shell-cmd -c /tmp "rm %1" "\.bak$"
```

Stop on first error:

```bash
shell-cmd -e ./src "gcc -c %1 -o /tmp/%b.o" "\.c$"
```

---

## How It Works

The program recursively walks the specified directory using `std::filesystem`. For each entry:

1. Hidden files (names starting with `.`) are skipped unless `-a` is set.
2. The **exclude pattern** (`-x`) is tested against the entry's filename. If it matches, the entry (and its subtree, if a directory) is skipped.
3. The **search regex** is tested against the entry's full path.
4. All active **metadata filters** (size, mtime, permissions, owner, group, type) are applied.
5. If everything passes, placeholders in the command template are substituted and the command is executed via `fork`/`exec` through the configured shell.

When using `--glob`, the search pattern and/or exclude pattern (with `-i`) are converted to anchored regex (`^...$`) with proper escaping before matching begins. When using `--regex-match`, the search regex is wrapped in `^(?:...)$` for full-path matching.

When using `-l` / `--list-all`, `shell-cmd` does not run a command per file; it collects all matched paths, joins them with spaces, and runs the command exactly once with `%0` replaced by the full list string.

## shell-cmd vs `find -exec`

| Feature | `shell-cmd` | `find -exec` |
|---------|-------------|---------------|
| **Filename placeholder** | `%0` gives the filename without the path | No equivalent — requires `sh -c` + `basename` |
| **Full path placeholder** | `%1` | `{}` |
| **Extra arguments** | `%2`, `%3`, … with validation | Not supported — use shell variables |
| **Pattern matching** | ECMAScript regex (substring or full-path), glob mode | Glob (`-name`) or implementation-varying `-regex` |
| **Exclude patterns** | Built-in `-x` with regex or glob (`-i`) | Requires negation logic or `! -name` |
| **Dry-run** | Built-in `-n` flag | No native support |
| **Verbose mode** | Built-in `-v` flag | No native support |
| **Filter by metadata** | Size (`-s`), time (`-m`), permissions (`-p`), owner (`-u`), group (`-g`), type (`-t`) | Size, time, permissions, ownership, type, boolean logic |
| **Parallel execution** | Built-in `-j N` | Requires `xargs -P` or GNU `parallel` |
| **Confirm mode** | Built-in `-c` flag | Requires `-ok` (not universally supported) |
| **Stop on error** | Built-in `-e` flag | No native support |
| **Summary stats** | Automatic (matched/run/failed counts) | No native support |
| **Portability** | Requires C++20 build | POSIX-standard, available everywhere |

Side-by-side example — copy all `.txt` files to a backup directory, preserving filenames:

```bash
# shell-cmd
shell-cmd . "cp %1 /tmp/backup/%0" "\.txt$"

# find equivalent
find . -regex '.*\.txt$' -exec sh -c 'cp "$1" "/tmp/backup/$(basename "$1")"' _ {} \;
```

In short, `shell-cmd` trades `find`'s boolean filter combinators for a more ergonomic command-templating experience with built-in dry-run, parallel execution, confirm mode, stop-on-error, exclude patterns (regex or glob), and summary statistics.

## License

See [LICENSE](LICENSE).
