# Shell Command Executor

This utility allows for the execution of shell commands on files matching a specified regex pattern within a directory or its subdirectories. It's particularly useful for batch processing files, such as extracting archives or performing file transformations. The tool was developed as a practice project to explore file system manipulation and process creation in C++.

## Getting Started

### Prerequisites

Ensure you have a C++ compiler and the necessary build tools installed on your system to compile the program. The utility uses POSIX APIs, making it suitable for UNIX-like operating systems including Linux and macOS.

### Compilation

To compile the `shell-cmd` utility, navigate to the directory containing the source code and run the following command:

```bash
mkdir build && cd build
cmake ..
make -j4
sudo make install
```

This command compiles the `shell-cmd.cpp` file and outputs an executable named `shell-cmd`.

## Usage

After compiling the utility, you can use it by specifying a directory path, a command template, and a regex pattern to match filenames. The `%f` placeholder in the command template will be replaced with the full path of each file matching the pattern.

```bash
shell-cmd <path> "command %f" <regex_search_pattern>
```

### Examples

1. To display the content of all `.txt` files in the current directory and its subdirectories:

    ```bash
    shell-cmd . "cat %f" .txt
    ```

2. To extract all `.7z` archives in the current directory and its subdirectories:

    ```bash
    shell-cmd . "7z e %f" .7z
    ```

## How It Works

The program starts by validating the input arguments to ensure the correct format. It then recursively searches the specified directory and its subdirectories for files matching the given regex pattern. For each matching file, the program replaces the `%f` placeholder in the provided command with the file's path and executes the command.

## Limitations

- The utility is designed for UNIX-like operating systems and may not work as expected on non-POSIX systems.
- Error handling is minimal, primarily focusing on demonstrating the concept.

## Contributing

This project is a practice exercise, and contributions or suggestions for improvements are welcome.
