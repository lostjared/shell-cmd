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

## License

See [LICENSE](LICENSE).
