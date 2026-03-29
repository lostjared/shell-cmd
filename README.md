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
| `-b` | `--glob` | Treat pattern as a glob (`*`, `?`) instead of regex |
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
| `-x REGEX` | `--exclude REGEX` | Exclude files/directories matching REGEX |
| `-e` | `--stop-on-error` | Stop on first command failure |
| `-c` | `--confirm` | Prompt for confirmation before each command |
| `-j N` | `--jobs N` | Run N commands in parallel (default: 1) |
| `-w SHELL` | `--shell SHELL` | Shell to use for execution (default: `/bin/bash`) |
| `-h` | `--help` | Show help |

## Examples

Count lines in all `.cpp` files:

```bash
shell-cmd . "wc -l %1" ".*\.cpp$"
```

Dry-run to preview what would be executed:

```bash
shell-cmd -n . "clang-format -i %1" ".*\.(cpp|hpp)$"
```

Copy matched files to a destination, using filename-only placeholder:

```bash
shell-cmd . "cp %1 /tmp/backup/%0" ".*\.txt$"
```

Limit search to current directory (no recursion):

```bash
shell-cmd -d 0 . "cat %1" ".*\.md$"
```

Use extra arguments — `%2` is replaced with the value passed after the regex:

```bash
shell-cmd . "cp %1 %2/%0" ".*\.log$" /tmp/logs
```

Include hidden files:

```bash
shell-cmd -a ~ "echo %1" ".*\.bashrc"
```

Run a single batch command with all matches using list-all:

```bash
shell-cmd -l . "ls -l %0" ".*\.log$"
```

In this mode, `%0` is substituted with a single space-separated string containing every matched path.

Extract all `.7z` archives:

```bash
shell-cmd . "7z e %1" ".*\.7z$"
```

Find large files (over 10 MB):

```bash
shell-cmd . "ls -lh %1" ".*" --size +10M
```

Delete files older than 30 days, with dry-run:

```bash
shell-cmd --dry-run /tmp "rm %1" ".*\.tmp$" --mtime +30
```

Find executable files (permission 755):

```bash
shell-cmd . "echo %1" ".*" --perm 755 --type f
```

List files owned by root:

```bash
shell-cmd /etc "echo %1" ".*\.conf$" --user root
```

List only directories matching a pattern:

```bash
shell-cmd . "echo %1" ".*src.*" --type d
```

Combine filters — large `.log` files modified recently:

```bash
shell-cmd /var/log "wc -l %1" ".*\.log$" -s +1M -m -7
```

Use glob patterns instead of regex:

```bash
shell-cmd --glob . "echo %1" "*.cpp"
```

Glob with regex-match mode:

```bash
shell-cmd --glob --regex-match . "echo %1" "*cmake"
```

Exclude `node_modules` and `.git` directories:

```bash
shell-cmd -x "node_modules|\.git" . "wc -l %1" ".*\.ts$"
```

Convert WAV to MP3, using `%b` for the output filename without extension:

```bash
shell-cmd ~/music "ffmpeg -i %1 /tmp/mp3/%b.mp3" ".*\.wav$"
```

Run commands in parallel with 4 jobs:

```bash
shell-cmd -j 4 ./images "convert %1 -resize 800x600 /tmp/thumbs/%0" ".*\.jpg$"
```

Confirm before each destructive command:

```bash
shell-cmd -c /tmp "rm %1" ".*\.bak$"
```

Stop on first error:

```bash
shell-cmd -e ./src "gcc -c %1 -o /tmp/%b.o" ".*\.c$"
```

## How It Works

The program recursively walks the specified directory using `std::filesystem`. For each file whose path matches the given regex, it substitutes placeholders in the command template and executes it via the configured shell (`/bin/bash` by default). Hidden files and directories are skipped by default.

When using `-l` or `--list-all`, `shell-cmd` does not run a command per file; it collects all matched paths, joins them with spaces, and runs the command exactly once with `%0` replaced by the full list string.

## shell-cmd vs `find -exec`

| Feature | `shell-cmd` | `find -exec` |
|---------|-------------|---------------|
| **Filename placeholder** | `%0` gives the filename without the path | No equivalent — requires `sh -c` + `basename` |
| **Full path placeholder** | `%1` | `{}` |
| **Extra arguments** | `%2`, `%3`, … with validation | Not supported — use shell variables |
| **Pattern matching** | ECMAScript regex on the full path | Glob (`-name`) or implementation-varying `-regex` |
| **Dry-run** | Built-in `-n` flag | No native support |
| **Verbose mode** | Built-in `-v` flag | No native support |
| **Filter by metadata** | Size (`-s`), time (`-m`), permissions (`-p`), owner (`-u`), group (`-g`), type (`-t`) | Size, time, permissions, ownership, type, boolean logic |
| **Exclude patterns** | Built-in `-x` with regex | Requires negation logic or `! -name` |
| **Parallel execution** | Built-in `-j N` | Requires `xargs -P` or GNU `parallel` |
| **Confirm mode** | Built-in `-c` flag | Requires `-ok` (not universally supported) |
| **Stop on error** | Built-in `-e` flag | No native support |
| **Summary stats** | Automatic (matched/run/failed counts) | No native support |
| **Portability** | Requires C++20 build | POSIX-standard, available everywhere |

Side-by-side example — copy all `.txt` files to a backup directory, preserving filenames:

```bash
# shell-cmd
shell-cmd . "cp %1 /tmp/backup/%0" ".*\.txt$"

# find equivalent
find . -regex '.*\.txt$' -exec sh -c 'cp "$1" "/tmp/backup/$(basename "$1")"' _ {} \;
```

In short, `shell-cmd` trades `find`'s boolean filter combinators for a more ergonomic command-templating experience with built-in dry-run, parallel execution, confirm mode, stop-on-error, exclude patterns, and summary statistics.

## License

See [LICENSE](LICENSE).
