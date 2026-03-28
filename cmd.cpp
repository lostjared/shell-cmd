/**
 * @file cmd.cpp
 * @brief shell-cmd v1.2 — Recursively find files matching a regex and execute a shell command for each match.
 * @details Walks a directory tree using std::filesystem, applies metadata filters (size, time,
 *          permissions, ownership, type), substitutes placeholders in a command template, and
 *          executes the resulting command for every matched entry. Supports parallel execution,
 *          exclude patterns, confirm mode, and stop-on-error.
 * @see https://lostsidedead.biz
 * @license GNU GPL v3
 */

#include "argz.hpp"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <grp.h>
#include <iostream>
#include <pwd.h>
#include <regex>
#include <signal.h>
#include <span>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

/// @brief Comparison operator for size and time filters.
enum class CmpOp {
    EQ,
    LT,
    GT
};

/// @brief Holds a parsed size filter with comparison operator and byte threshold.
struct SizeFilter {
    bool active = false;  ///< Whether this filter is enabled.
    CmpOp op = CmpOp::EQ; ///< Comparison direction (equal, less-than, greater-than).
    uintmax_t bytes = 0;  ///< Size threshold in bytes.
};

/// @brief Holds a parsed modification-time filter with comparison operator and day count.
struct TimeFilter {
    bool active = false;  ///< Whether this filter is enabled.
    CmpOp op = CmpOp::EQ; ///< Comparison direction.
    int days = 0;         ///< Age threshold in days.
};

/// @brief Aggregates all runtime options parsed from the command line.
struct Options {
    bool dry_run = false;        ///< Print commands without executing.
    bool verbose = false;        ///< Print commands before executing.
    bool hidden = false;         ///< Include hidden (dot) files/directories.
    int max_depth = -1;          ///< Max recursion depth (-1 = unlimited).
    SizeFilter size_filter;      ///< Optional size filter.
    TimeFilter mtime_filter;     ///< Optional modification-time filter.
    std::string perm_filter;     ///< Octal permission string, e.g. "755".
    std::string user_filter;     ///< Owner username filter.
    std::string group_filter;    ///< Group name filter.
    char type_filter = 0;        ///< Type filter: 'f' file, 'd' directory, 'l' symlink.
    std::string exclude_pattern; ///< Regex pattern to exclude files/dirs.
    bool stop_on_error = false;  ///< Halt on first command failure.
    bool confirm = false;        ///< Prompt for confirmation before each command.
    int jobs = 1;                ///< Number of parallel jobs (1 = sequential).
};

static Options opts; ///< Global runtime options.

/// @brief Tracks execution statistics printed in the summary.
struct Stats {
    int files_matched = 0;   ///< Number of entries that matched all filters.
    int commands_run = 0;    ///< Number of commands executed (or printed in dry-run).
    int commands_failed = 0; ///< Number of commands that returned non-zero.
};

static Stats stats;                   ///< Global execution statistics.
static std::vector<pid_t> child_pids; ///< PIDs of outstanding child processes (parallel mode).
static bool stop_requested = false;   ///< Set to true when stop-on-error triggers.

SizeFilter parse_size_filter(const std::string &s);
TimeFilter parse_time_filter(const std::string &s);
bool matches_filters(const fs::directory_entry &entry);
bool proc_cmd(const std::string &cmd, std::span<const std::string> text);
void wait_for_slot();
void wait_all();
std::string replace_string(std::string orig, const std::string &with, const std::string &rep);
void add_directory(const fs::path &path, const std::string &cmd, const std::string &regex_str, std::vector<std::string> &args, int depth);
int System(const std::string &command);

/**
 * @brief Parse a size filter string into a SizeFilter.
 * @param s Filter string, e.g. "+10M", "-1K", "4096".
 *          Prefix '+' means greater-than, '-' means less-than, no prefix means exact.
 *          Suffix K/M/G multiplies by 1024/1048576/1073741824.
 * @return A populated SizeFilter with active=true.
 */
SizeFilter parse_size_filter(const std::string &s) {
    SizeFilter f;
    f.active = true;
    std::string val = s;
    if (val[0] == '+') {
        f.op = CmpOp::GT;
        val = val.substr(1);
    } else if (val[0] == '-') {
        f.op = CmpOp::LT;
        val = val.substr(1);
    } else {
        f.op = CmpOp::EQ;
    }
    uintmax_t multiplier = 1;
    char suffix = val.back();
    if (suffix == 'K' || suffix == 'k') {
        multiplier = 1024;
        val.pop_back();
    } else if (suffix == 'M' || suffix == 'm') {
        multiplier = 1024 * 1024;
        val.pop_back();
    } else if (suffix == 'G' || suffix == 'g') {
        multiplier = 1024ULL * 1024 * 1024;
        val.pop_back();
    }
    f.bytes = std::stoull(val) * multiplier;
    return f;
}

/**
 * @brief Parse a time filter string into a TimeFilter.
 * @param s Filter string, e.g. "+7", "-1", "3".
 *          Prefix '+' means older-than, '-' means newer-than, no prefix means exact.
 * @return A populated TimeFilter with active=true.
 */
TimeFilter parse_time_filter(const std::string &s) {
    TimeFilter f;
    f.active = true;
    std::string val = s;
    if (val[0] == '+') {
        f.op = CmpOp::GT;
        val = val.substr(1);
    } else if (val[0] == '-') {
        f.op = CmpOp::LT;
        val = val.substr(1);
    } else {
        f.op = CmpOp::EQ;
    }
    f.days = std::stoi(val);
    return f;
}

/**
 * @brief Test a directory entry against all active metadata filters.
 * @param entry The filesystem directory entry to check.
 * @return true if the entry passes all active filters, false otherwise.
 */
bool matches_filters(const fs::directory_entry &entry) {
    std::error_code ec;

    // Type filter
    if (opts.type_filter != 0) {
        switch (opts.type_filter) {
        case 'f':
            if (!entry.is_regular_file(ec))
                return false;
            break;
        case 'd':
            if (!entry.is_directory(ec))
                return false;
            break;
        case 'l':
            if (!entry.is_symlink(ec))
                return false;
            break;
        }
    }

    // Size filter (only meaningful for regular files)
    if (opts.size_filter.active) {
        if (!entry.is_regular_file(ec))
            return false;
        auto sz = entry.file_size(ec);
        if (ec)
            return false;
        switch (opts.size_filter.op) {
        case CmpOp::GT:
            if (sz <= opts.size_filter.bytes)
                return false;
            break;
        case CmpOp::LT:
            if (sz >= opts.size_filter.bytes)
                return false;
            break;
        case CmpOp::EQ:
            if (sz != opts.size_filter.bytes)
                return false;
            break;
        }
    }

    // Modification time filter
    if (opts.mtime_filter.active) {
        auto ftime = entry.last_write_time(ec);
        if (ec)
            return false;
        auto sctp = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
        auto now = std::chrono::system_clock::now();
        auto age = std::chrono::duration_cast<std::chrono::hours>(now - sctp).count() / 24;
        switch (opts.mtime_filter.op) {
        case CmpOp::GT:
            if (age <= opts.mtime_filter.days)
                return false;
            break;
        case CmpOp::LT:
            if (age >= opts.mtime_filter.days)
                return false;
            break;
        case CmpOp::EQ:
            if (age != opts.mtime_filter.days)
                return false;
            break;
        }
    }

    // Permission filter (octal comparison)
    if (!opts.perm_filter.empty()) {
        struct stat st;
        if (stat(entry.path().c_str(), &st) != 0)
            return false;
        auto mode = st.st_mode & 07777;
        auto target = static_cast<mode_t>(std::stoul(opts.perm_filter, nullptr, 8));
        if (mode != target)
            return false;
    }

    // User filter
    if (!opts.user_filter.empty()) {
        struct stat st;
        if (stat(entry.path().c_str(), &st) != 0)
            return false;
        struct passwd *pw = getpwuid(st.st_uid);
        if (!pw || opts.user_filter != pw->pw_name)
            return false;
    }

    // Group filter
    if (!opts.group_filter.empty()) {
        struct stat st;
        if (stat(entry.path().c_str(), &st) != 0)
            return false;
        struct group *gr = getgrgid(st.st_gid);
        if (!gr || opts.group_filter != gr->gr_name)
            return false;
    }

    return true;
}

/**
 * @brief Replace all occurrences of a substring within a string.
 * @param orig The original string.
 * @param with The substring to search for.
 * @param rep  The replacement text.
 * @return A new string with all occurrences replaced.
 */
std::string replace_string(std::string orig, const std::string &with, const std::string &rep) {
    size_t pos = 0;
    while ((pos = orig.find(with, pos)) != std::string::npos) {
        orig.replace(pos, with.length(), rep);
        pos += rep.length();
    }
    return orig;
}

/**
 * @brief Recursively walk a directory, match entries against a regex and filters, and run commands.
 * @param path       The directory to scan.
 * @param cmd        The command template containing placeholders.
 * @param regex_str  ECMAScript regex matched against each entry's full path.
 * @param args       Mutable argument vector; args[0] is overwritten with the matched path.
 * @param depth      Current recursion depth (0 at the root call).
 */
void add_directory(const fs::path &path, const std::string &cmd, const std::string &regex_str, std::vector<std::string> &args, int depth) {
    if (opts.max_depth >= 0 && depth > opts.max_depth)
        return;
    if (stop_requested)
        return;

    std::error_code ec;
    auto dir = fs::directory_iterator(path, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        std::cerr << std::format("Error: could not open directory: {}\n", path.string());
        exit(EXIT_FAILURE);
    }

    for (const auto &entry : dir) {
        if (stop_requested)
            return;
        auto filename = entry.path().filename().string();
        if (!opts.hidden && filename.starts_with('.'))
            continue;

        // Exclude pattern check
        if (!opts.exclude_pattern.empty()) {
            std::regex excl(opts.exclude_pattern, std::regex::ECMAScript);
            if (std::regex_search(filename, excl))
                continue;
        }

        if (entry.is_directory(ec)) {
            // If type filter is 'd', also match directories against regex
            if (opts.type_filter == 'd') {
                std::regex ex(regex_str);
                auto fullpath = entry.path().string();
                if (std::regex_search(fullpath, ex) && matches_filters(entry)) {
                    stats.files_matched++;
                    args[0] = fullpath;
                    if (!proc_cmd(cmd, args))
                        return;
                }
            }
            add_directory(entry.path(), cmd, regex_str, args, depth + 1);
        } else if (entry.is_symlink(ec) && opts.type_filter == 'l') {
            std::regex ex(regex_str);
            auto fullpath = entry.path().string();
            if (std::regex_search(fullpath, ex) && matches_filters(entry)) {
                stats.files_matched++;
                args[0] = fullpath;
                if (!proc_cmd(cmd, args))
                    return;
            }
        } else if (entry.is_regular_file(ec) || (entry.is_symlink(ec) && opts.type_filter == 0)) {
            std::regex ex(regex_str);
            auto fullpath = entry.path().string();
            if (std::regex_search(fullpath, ex) && matches_filters(entry)) {
                stats.files_matched++;
                args[0] = fullpath;
                if (!proc_cmd(cmd, args))
                    return;
            }
        }
    }
}

/**
 * @brief Execute a shell command using fork/exec with proper signal handling.
 * @details Blocks SIGCHLD and ignores SIGINT/SIGQUIT in the parent process
 *          to prevent interrupted batch operations. Restores all signal masks
 *          after the child exits.
 * @param command The shell command string to execute via /bin/sh -c.
 * @return The child's exit status from waitpid, or -1 on fork failure.
 */
int System(const std::string &command) {
    sigset_t bmask, omask;
    struct sigaction sa_ignore, sa_oquit, sa_origint, sa_default;
    pid_t id;
    int status, serrno;

    if (command.empty())
        return System(":") == 0;

    sigemptyset(&bmask);
    sigaddset(&bmask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &bmask, &omask);
    sa_ignore.sa_handler = SIG_IGN;
    sa_ignore.sa_flags = 0;
    sigemptyset(&sa_ignore.sa_mask);
    sigaction(SIGINT, &sa_ignore, &sa_origint);
    sigaction(SIGQUIT, &sa_ignore, &sa_oquit);

    switch ((id = fork())) {
    case -1:
        status = -1;
        break;
    case 0:
        sa_default.sa_handler = SIG_DFL;
        sa_default.sa_flags = 0;
        sigemptyset(&sa_default.sa_mask);
        if (sa_origint.sa_handler != SIG_IGN)
            sigaction(SIGINT, &sa_default, NULL);
        if (sa_oquit.sa_handler != SIG_IGN)
            sigaction(SIGQUIT, &sa_default, NULL);

        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char *>(nullptr));
        _exit(127);
        break;
    default:
        while (waitpid(id, &status, 0) == -1) {
            if (errno != EINTR) {
                status = -1;
                break;
            }
        }
        break;
    }
    serrno = errno;
    sigprocmask(SIG_SETMASK, &omask, NULL);
    sigaction(SIGINT, &sa_origint, NULL);
    sigaction(SIGQUIT, &sa_oquit, NULL);
    errno = serrno;
    return status;
}

/**
 * @brief Block until a parallel execution slot is available.
 * @details Waits for any outstanding child process to finish, updates stats,
 *          and sets stop_requested if stop-on-error is enabled and a child failed.
 */
void wait_for_slot() {
    while (static_cast<int>(child_pids.size()) >= opts.jobs) {
        int status;
        pid_t pid = waitpid(-1, &status, 0);
        if (pid > 0) {
            std::erase(child_pids, pid);
            stats.commands_run++;
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                stats.commands_failed++;
                if (opts.stop_on_error)
                    stop_requested = true;
            }
        }
    }
}

/**
 * @brief Wait for all outstanding child processes to finish.
 * @details Called at the end of execution in parallel mode to drain the process pool.
 */
void wait_all() {
    while (!child_pids.empty()) {
        int status;
        pid_t pid = waitpid(-1, &status, 0);
        if (pid > 0) {
            std::erase(child_pids, pid);
            stats.commands_run++;
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                stats.commands_failed++;
        }
    }
}

/**
 * @brief Substitute placeholders in a command template and execute the result.
 * @details Replaces %%0 (filename), %%1 (full path), %%b (stem), %%e (extension),
 *          and %%2+ (extra args). Supports confirm mode, dry-run, parallel forking,
 *          and stop-on-error.
 * @param cmd  The command template string.
 * @param text Span of strings: text[0] is the matched file path, text[1+] are extras.
 * @return true to continue processing, false to stop (stop-on-error triggered).
 */
bool proc_cmd(const std::string &cmd, std::span<const std::string> text) {
    std::string r = cmd;
    if (!text.empty()) {
        auto fpath = fs::path(text[0]);
        auto fname = fpath.filename().string();
        r = replace_string(r, "%0", fname);
        r = replace_string(r, "%b", fpath.stem().string());
        r = replace_string(r, "%e", fpath.extension().string());
    }
    for (size_t i = 0; i < text.size(); ++i) {
        auto placeholder = std::format("%{}", i + 1);
        if (i == 0 && text[i].find(' ') != std::string::npos)
            r = replace_string(r, placeholder, std::format("\"{}\"", text[i]));
        else
            r = replace_string(r, placeholder, std::string{text[i]});
    }

    if (opts.confirm) {
        std::cout << std::format("Execute: {} ? [y/N] ", r);
        std::string answer;
        std::getline(std::cin, answer);
        if (answer != "y" && answer != "Y")
            return true;
    }

    if (opts.verbose || opts.dry_run)
        std::cout << r << "\n";

    if (opts.dry_run) {
        stats.commands_run++;
        return true;
    }

    if (opts.jobs > 1) {
        wait_for_slot();
        if (stop_requested)
            return false;
        pid_t pid = fork();
        if (pid == 0) {
            execl("/bin/sh", "sh", "-c", r.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        } else if (pid > 0) {
            child_pids.push_back(pid);
        } else {
            perror("fork");
            stats.commands_failed++;
            return !opts.stop_on_error;
        }
        return true;
    }

    int ret = System(r);
    stats.commands_run++;
    if (ret != 0) {
        stats.commands_failed++;
        if (opts.stop_on_error) {
            std::cerr << std::format("Error: command failed (exit {}), stopping.\n", WEXITSTATUS(ret));
            stop_requested = true;
            return false;
        }
    }
    return true;
}

/**
 * @brief Print usage information to stdout.
 * @param prog The program name (argv[0]).
 */
void print_help(const char *prog) {
    std::cout << std::format(
        "usage: {} [options] path \"command %1 [%2 %3..]\" regex [extra_args..]\n\n"
        "Recursively find files matching regex and run command for each.\n\n"
        "placeholders:\n"
        "  %0          filename only (no path)\n"
        "  %1          full path to matched file\n"
        "  %2+         extra arguments from command line\n"
        "  %b          basename without extension\n"
        "  %e          file extension (including dot)\n\n"
        "options:\n"
        "  -n, --dry-run       dry-run, print commands without executing\n"
        "  -v, --verbose       verbose, print each command before running\n"
        "  -a, --all           include hidden files/directories\n"
        "  -d, --depth N       max recursion depth (0 = current dir only)\n"
        "  -s, --size SIZE     filter by size: +10M (>10MB), -1K (<1KB),\n"
        "                      4096 (exactly 4096 bytes). Suffixes: K, M, G\n"
        "  -m, --mtime DAYS    filter by modification time: +7 (older than 7 days),\n"
        "                      -1 (modified within last day), 3 (exactly 3 days)\n"
        "  -p, --perm MODE     filter by permissions (octal), e.g. 755\n"
        "  -u, --user USER     filter by owner username\n"
        "  -g, --group GROUP   filter by group name\n"
        "  -t, --type TYPE     filter by type: f (file), d (directory), l (symlink)\n"
        "  -x, --exclude REGEX exclude files/directories matching REGEX\n"
        "  -e, --stop-on-error stop on first command failure\n"
        "  -c, --confirm       prompt for confirmation before each command\n"
        "  -j, --jobs N        run N commands in parallel (default: 1)\n"
        "  -h, --help          show this help\n",
        prog);
}

/**
 * @brief Program entry point.
 * @details Parses command-line arguments via argz, configures options, validates
 *          positional arguments and placeholder consistency, runs directory traversal,
 *          waits for parallel children, and prints a summary.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return EXIT_SUCCESS on success, EXIT_FAILURE if any command failed or input was invalid.
 */
int main(int argc, char **argv) {
    Argz<std::string> argz(argc, argv);
    argz.addOptionSingle('n', "dry-run mode")
        .addOptionDouble('N', "dry-run", "dry-run mode")
        .addOptionSingle('v', "verbose output")
        .addOptionDouble('V', "verbose", "verbose output")
        .addOptionSingle('a', "include hidden files")
        .addOptionDouble('A', "all", "include hidden files")
        .addOptionSingleValue('d', "max depth")
        .addOptionDoubleValue('D', "depth", "max depth")
        .addOptionSingleValue('s', "size filter")
        .addOptionDoubleValue('S', "size", "size filter")
        .addOptionSingleValue('m', "modification time filter")
        .addOptionDoubleValue('M', "mtime", "modification time filter")
        .addOptionSingleValue('p', "permission filter")
        .addOptionDoubleValue('P', "perm", "permission filter")
        .addOptionSingleValue('u', "user filter")
        .addOptionDoubleValue('U', "user", "user filter")
        .addOptionSingleValue('g', "group filter")
        .addOptionDoubleValue('G', "group", "group filter")
        .addOptionSingleValue('t', "type filter")
        .addOptionDoubleValue('T', "type", "type filter")
        .addOptionSingleValue('x', "exclude pattern")
        .addOptionDoubleValue('X', "exclude", "exclude pattern")
        .addOptionSingle('e', "stop on error")
        .addOptionDouble('E', "stop-on-error", "stop on error")
        .addOptionSingle('c', "confirm mode")
        .addOptionDouble('C', "confirm", "confirm mode")
        .addOptionSingleValue('j', "parallel jobs")
        .addOptionDoubleValue('J', "jobs", "parallel jobs")
        .addOptionSingle('h', "show help")
        .addOptionDouble('H', "help", "show help");

    std::vector<std::string> positional;

    try {
        Argument<std::string> arg;
        int ret;
        while ((ret = argz.proc(arg)) != -1) {
            switch (ret) {
            case 'n':
            case 'N':
                opts.dry_run = true;
                break;
            case 'v':
            case 'V':
                opts.verbose = true;
                break;
            case 'a':
            case 'A':
                opts.hidden = true;
                break;
            case 'd':
            case 'D':
                opts.max_depth = std::stoi(arg.arg_value);
                break;
            case 's':
            case 'S':
                opts.size_filter = parse_size_filter(arg.arg_value);
                break;
            case 'm':
            case 'M':
                opts.mtime_filter = parse_time_filter(arg.arg_value);
                break;
            case 'p':
            case 'P':
                opts.perm_filter = arg.arg_value;
                break;
            case 'u':
            case 'U':
                opts.user_filter = arg.arg_value;
                break;
            case 'g':
            case 'G':
                opts.group_filter = arg.arg_value;
                break;
            case 't':
            case 'T':
                if (arg.arg_value == "f" || arg.arg_value == "d" || arg.arg_value == "l") {
                    opts.type_filter = arg.arg_value[0];
                } else {
                    std::cerr << std::format("Error: invalid type '{}'. Use f (file), d (directory), or l (symlink).\n", arg.arg_value);
                    return EXIT_FAILURE;
                }
                break;
            case 'x':
            case 'X':
                opts.exclude_pattern = arg.arg_value;
                break;
            case 'e':
            case 'E':
                opts.stop_on_error = true;
                break;
            case 'c':
            case 'C':
                opts.confirm = true;
                break;
            case 'j':
            case 'J':
                opts.jobs = std::stoi(arg.arg_value);
                if (opts.jobs < 1)
                    opts.jobs = 1;
                break;
            case 'h':
            case 'H':
                print_help(argv[0]);
                return 0;
            case '-':
                positional.push_back(arg.arg_value);
                break;
            }
        }
    } catch (const ArgException<std::string> &e) {
        std::cerr << std::format("Error: {}\n", e.text());
        return EXIT_FAILURE;
    }

    if (positional.size() < 3) {
        std::cerr << "Error: at least three positional arguments required.\n";
        print_help(argv[0]);
        return EXIT_FAILURE;
    }

    try {
        const auto &path = positional[0];
        const auto &input = positional[1];
        const auto &regex_str = positional[2];
        size_t index = 2;
        std::vector<std::string> args{"filename"};
        for (size_t i = 3; i < positional.size(); ++i) {
            if (input.find(std::format("%{}", index)) == std::string::npos) {
                std::cerr << std::format("Error: command has no placeholder %{} for extra argument \"{}\"\n", index, positional[i]);
                return EXIT_FAILURE;
            }
            args.push_back(positional[i]);
            ++index;
        }
        add_directory(path, input, regex_str, args, 0);
        if (opts.jobs > 1)
            wait_all();

        if (opts.verbose || opts.dry_run || stats.commands_failed > 0) {
            std::cerr << std::format("\nSummary: {} matched, {} run, {} failed\n",
                                    stats.files_matched, stats.commands_run, stats.commands_failed);
        }
    } catch(const std::exception &e) {
        std::cerr << "Exception: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return stats.commands_failed > 0 ? EXIT_FAILURE : 0;
}
