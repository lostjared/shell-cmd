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

### Options

| Flag | Description |
|------|-------------|
| `-n` | Dry-run — print commands without executing |
| `-v` | Verbose — print each command before running |
| `-a` | Include hidden files and directories |
| `-d depth` | Max recursion depth (0 = current directory only) |
| `-h` | Show help |

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

Extract all `.7z` archives:

```bash
shell-cmd . "7z e %1" ".*\.7z$"
```

## How It Works

The program recursively walks the specified directory using `std::filesystem`. For each file whose path matches the given regex, it substitutes placeholders in the command template and executes it via `/bin/sh`. Hidden files and directories are skipped by default.

## shell-cmd vs `find -exec`

| Feature | `shell-cmd` | `find -exec` |
|---------|-------------|---------------|
| **Filename placeholder** | `%0` gives the filename without the path | No equivalent — requires `sh -c` + `basename` |
| **Full path placeholder** | `%1` | `{}` |
| **Extra arguments** | `%2`, `%3`, … with validation | Not supported — use shell variables |
| **Pattern matching** | ECMAScript regex on the full path | Glob (`-name`) or implementation-varying `-regex` |
| **Dry-run** | Built-in `-n` flag | No native support |
| **Verbose mode** | Built-in `-v` flag | No native support |
| **Filtering by metadata** | Not supported | Size, time, permissions, ownership, type, boolean logic |
| **Portability** | Requires C++20 build | POSIX-standard, available everywhere |

Side-by-side example — copy all `.txt` files to a backup directory, preserving filenames:

```bash
# shell-cmd
shell-cmd . "cp %1 /tmp/backup/%0" ".*\.txt$"

# find equivalent
find . -regex '.*\.txt$' -exec sh -c 'cp "$1" "/tmp/backup/$(basename "$1")"' _ {} \;
```

In short, `shell-cmd` trades `find`'s metadata filtering power for a more ergonomic command-templating experience, especially when you need the filename separate from the path or want to inject extra arguments.

## License

See [LICENSE](LICENSE).
