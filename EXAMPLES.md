# shell-cmd Examples

## Batch Compile C Files

Compile every `.c` file found recursively and place the object files in `/tmp`:

```bash
shell-cmd . "gcc -c %1 -o /tmp/%0.o" ".*\.c$"
```

- `%1` expands to the full path so `gcc` can find the source
- `%0` expands to just the filename, used here to name the output `.o`

---

## Resize All JPEG Images

Resize every JPEG in your photo library to 800x600 using ImageMagick:

```bash
shell-cmd ~/Photos "convert %1 -resize 800x600 /tmp/resized/%0" ".*\.jpe?g$"
```

The regex `jpe?g` matches both `.jpg` and `.jpeg` extensions.

---

## Strip Image Metadata

Remove all EXIF/metadata from images before sharing:

```bash
shell-cmd . "exiftool -all= %1" ".*\.(jpg|png)$"
```

---

## Convert Markdown to PDF

Convert every Markdown file under `~/docs` to PDF with Pandoc:

```bash
shell-cmd ~/docs "pandoc %1 -o /tmp/%0.pdf" ".*\.md$"
```

---

## Search for TODOs in Python Files

Use `-n` (dry-run) to preview the commands first:

```bash
shell-cmd -n . "grep -Hn 'TODO' %1" ".*\.py$"
```

When satisfied, drop `-n` to execute:

```bash
shell-cmd . "grep -Hn 'TODO' %1" ".*\.py$"
```

---

## Extract Archives

Extract every `.tar.gz` archive in your downloads folder:

```bash
shell-cmd ~/Downloads "tar xzf %1 -C /tmp/extracted" ".*\.tar\.gz$"
```

Extract every `.7z` archive:

```bash
shell-cmd . "7z e %1" ".*\.7z$"
```

---

## Format C++ Source Code

Run `clang-format` on all C++ files, limited to the top-level `src/` directory only (`-d 0`):

```bash
shell-cmd -d 0 ./src "clang-format -i %1" ".*\.(cpp|hpp)$"
```

---

## Copy Files with Extra Arguments

Extra positional arguments after the regex become `%2`, `%3`, etc. Copy all `.mp3` files to a backup location:

```bash
shell-cmd ~/Music "cp %1 %2/%0" ".*\.mp3$" /mnt/backup/music
```

Here `/mnt/backup/music` is substituted for `%2`.

---

## Transcode Audio

Convert all `.wav` files to `.mp3` with ffmpeg:

```bash
shell-cmd ~/recordings "ffmpeg -i %1 -b:a 192k /tmp/mp3/%0.mp3" ".*\.wav$"
```

---

## Check Shell Script Syntax

Validate all shell scripts without executing them, with verbose output (`-v`):

```bash
shell-cmd -v . "bash -n %1" ".*\.sh$"
```

---

## Sign RPM Packages

Sign every `.rpm` package in a repository tree:

```bash
shell-cmd ./packages "rpm --addsign %1" ".*\.rpm$"
```

---

## Include Hidden Files

By default hidden files (starting with `.`) are skipped. Use `-a` to include them:

```bash
shell-cmd -a ~ "echo %1" ".*\.bashrc|.*\.zshrc"
```

---

## Combine Options

Preview (`-n`), include hidden files (`-a`), and limit depth to 2 levels (`-d 2`):

```bash
shell-cmd -n -a -d 2 ~ "wc -l %1" ".*\.bashrc|.*\.zshrc"
```

---

## Quick Reference

| Placeholder | Meaning |
|-------------|---------|
| `%0` | Filename only (no path) |
| `%1` | Full path to matched file |
| `%2+` | Extra arguments from command line |

| Flag | Effect |
|------|--------|
| `-n` | Dry-run — print without executing |
| `-v` | Verbose — print before executing |
| `-a` | Include hidden files/directories |
| `-d N` | Max recursion depth (0 = current dir only) |
| `-w SHELL` | Shell to use for execution (default: `/bin/bash`) |
| `-h` | Show help |
