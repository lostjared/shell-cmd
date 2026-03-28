/*

 shell-cmd v1.1
 https://lostsidedead.biz
 GNU GPL v3

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

enum class CmpOp { EQ, LT, GT };

struct SizeFilter {
    bool active = false;
    CmpOp op = CmpOp::EQ;
    uintmax_t bytes = 0;
};

struct TimeFilter {
    bool active = false;
    CmpOp op = CmpOp::EQ;
    int days = 0;
};

struct Options {
    bool dry_run = false;
    bool verbose = false;
    bool hidden = false;
    int max_depth = -1;
    SizeFilter size_filter;
    TimeFilter mtime_filter;
    std::string perm_filter;     // octal string, e.g. "755"
    std::string user_filter;     // username
    std::string group_filter;    // group name
    char type_filter = 0;        // 'f' = file, 'd' = directory, 'l' = symlink
};

static Options opts;

SizeFilter parse_size_filter(const std::string &s);
TimeFilter parse_time_filter(const std::string &s);
bool matches_filters(const fs::directory_entry &entry);
void proc_cmd(const std::string &cmd, std::span<const std::string> text);
std::string replace_string(std::string orig, const std::string &with, const std::string &rep);
void add_directory(const fs::path &path, const std::string &cmd, const std::string &regex_str, std::vector<std::string> &args, int depth);
int System(const std::string &command);

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

bool matches_filters(const fs::directory_entry &entry) {
    std::error_code ec;

    // Type filter
    if (opts.type_filter != 0) {
        switch (opts.type_filter) {
        case 'f':
            if (!entry.is_regular_file(ec)) return false;
            break;
        case 'd':
            if (!entry.is_directory(ec)) return false;
            break;
        case 'l':
            if (!entry.is_symlink(ec)) return false;
            break;
        }
    }

    // Size filter (only meaningful for regular files)
    if (opts.size_filter.active) {
        if (!entry.is_regular_file(ec)) return false;
        auto sz = entry.file_size(ec);
        if (ec) return false;
        switch (opts.size_filter.op) {
        case CmpOp::GT: if (sz <= opts.size_filter.bytes) return false; break;
        case CmpOp::LT: if (sz >= opts.size_filter.bytes) return false; break;
        case CmpOp::EQ: if (sz != opts.size_filter.bytes) return false; break;
        }
    }

    // Modification time filter
    if (opts.mtime_filter.active) {
        auto ftime = entry.last_write_time(ec);
        if (ec) return false;
        auto sctp = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
        auto now = std::chrono::system_clock::now();
        auto age = std::chrono::duration_cast<std::chrono::hours>(now - sctp).count() / 24;
        switch (opts.mtime_filter.op) {
        case CmpOp::GT: if (age <= opts.mtime_filter.days) return false; break;
        case CmpOp::LT: if (age >= opts.mtime_filter.days) return false; break;
        case CmpOp::EQ: if (age != opts.mtime_filter.days) return false; break;
        }
    }

    // Permission filter (octal comparison)
    if (!opts.perm_filter.empty()) {
        struct stat st;
        if (stat(entry.path().c_str(), &st) != 0) return false;
        auto mode = st.st_mode & 07777;
        auto target = static_cast<mode_t>(std::stoul(opts.perm_filter, nullptr, 8));
        if (mode != target) return false;
    }

    // User filter
    if (!opts.user_filter.empty()) {
        struct stat st;
        if (stat(entry.path().c_str(), &st) != 0) return false;
        struct passwd *pw = getpwuid(st.st_uid);
        if (!pw || opts.user_filter != pw->pw_name) return false;
    }

    // Group filter
    if (!opts.group_filter.empty()) {
        struct stat st;
        if (stat(entry.path().c_str(), &st) != 0) return false;
        struct group *gr = getgrgid(st.st_gid);
        if (!gr || opts.group_filter != gr->gr_name) return false;
    }

    return true;
}

std::string replace_string(std::string orig, const std::string &with, const std::string &rep) {
    size_t pos = 0;
    while ((pos = orig.find(with, pos)) != std::string::npos) {
        orig.replace(pos, with.length(), rep);
        pos += rep.length();
    }
    return orig;
}

void add_directory(const fs::path &path, const std::string &cmd, const std::string &regex_str, std::vector<std::string> &args, int depth) {
    if (opts.max_depth >= 0 && depth > opts.max_depth)
        return;

    std::error_code ec;
    auto dir = fs::directory_iterator(path, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        std::cerr << std::format("Error: could not open directory: {}\n", path.string());
        exit(EXIT_FAILURE);
    }

    for (const auto &entry : dir) {
        auto filename = entry.path().filename().string();
        if (!opts.hidden && filename.starts_with('.'))
            continue;

        if (entry.is_directory(ec)) {
            // If type filter is 'd', also match directories against regex
            if (opts.type_filter == 'd') {
                std::regex ex(regex_str);
                auto fullpath = entry.path().string();
                if (std::regex_search(fullpath, ex) && matches_filters(entry)) {
                    args[0] = fullpath;
                    proc_cmd(cmd, args);
                }
            }
            add_directory(entry.path(), cmd, regex_str, args, depth + 1);
        } else if (entry.is_symlink(ec) && opts.type_filter == 'l') {
            std::regex ex(regex_str);
            auto fullpath = entry.path().string();
            if (std::regex_search(fullpath, ex) && matches_filters(entry)) {
                args[0] = fullpath;
                proc_cmd(cmd, args);
            }
        } else if (entry.is_regular_file(ec) || (entry.is_symlink(ec) && opts.type_filter == 0)) {
            std::regex ex(regex_str);
            auto fullpath = entry.path().string();
            if (std::regex_search(fullpath, ex) && matches_filters(entry)) {
                args[0] = fullpath;
                proc_cmd(cmd, args);
            }
        }
    }
}

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

void proc_cmd(const std::string &cmd, std::span<const std::string> text) {
    std::string r = cmd;
    if (!text.empty()) {
        auto fname = fs::path(text[0]).filename().string();
        r = replace_string(r, "%0", fname);
    }
    for (size_t i = 0; i < text.size(); ++i) {
        auto placeholder = std::format("%{}", i + 1);
        if (i == 0 && text[i].find(' ') != std::string::npos)
            r = replace_string(r, placeholder, std::format("\"{}\"", text[i]));
        else
            r = replace_string(r, placeholder, std::string{text[i]});
    }
    if (opts.verbose || opts.dry_run)
        std::cout << r << "\n";
    if (!opts.dry_run)
        System(r);
}

void print_help(const char *prog) {
    std::cout << std::format(
        "usage: {} [options] path \"command %1 [%2 %3..]\" regex [extra_args..]\n\n"
        "Recursively find files matching regex and run command for each.\n\n"
        "placeholders:\n"
        "  %0          filename only (no path)\n"
        "  %1          full path to matched file\n"
        "  %2+         extra arguments from command line\n\n"
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
        "  -h, --help          show this help\n",
        prog);
}

int main(int argc, char **argv) {
    Argz<std::string> argz(argc, argv);
    argz.addOptionSingle('n', "dry-run mode")
        .addOptionDouble('n', "dry-run", "dry-run mode")
        .addOptionSingle('v', "verbose output")
        .addOptionDouble('v', "verbose", "verbose output")
        .addOptionSingle('a', "include hidden files")
        .addOptionDouble('a', "all", "include hidden files")
        .addOptionSingleValue('d', "max depth")
        .addOptionDoubleValue('d', "depth", "max depth")
        .addOptionSingleValue('s', "size filter")
        .addOptionDoubleValue('s', "size", "size filter")
        .addOptionSingleValue('m', "modification time filter")
        .addOptionDoubleValue('m', "mtime", "modification time filter")
        .addOptionSingleValue('p', "permission filter")
        .addOptionDoubleValue('p', "perm", "permission filter")
        .addOptionSingleValue('u', "user filter")
        .addOptionDoubleValue('u', "user", "user filter")
        .addOptionSingleValue('g', "group filter")
        .addOptionDoubleValue('g', "group", "group filter")
        .addOptionSingleValue('t', "type filter")
        .addOptionDoubleValue('t', "type", "type filter")
        .addOptionSingle('h', "show help")
        .addOptionDouble('h', "help", "show help");

    std::vector<std::string> positional;

    try {
        Argument<std::string> arg;
        int ret;
        while ((ret = argz.proc(arg)) != -1) {
            switch (ret) {
            case 'n':
                opts.dry_run = true;
                break;
            case 'v':
                opts.verbose = true;
                break;
            case 'a':
                opts.hidden = true;
                break;
            case 'd':
                opts.max_depth = std::stoi(arg.arg_value);
                break;
            case 's':
                opts.size_filter = parse_size_filter(arg.arg_value);
                break;
            case 'm':
                opts.mtime_filter = parse_time_filter(arg.arg_value);
                break;
            case 'p':
                opts.perm_filter = arg.arg_value;
                break;
            case 'u':
                opts.user_filter = arg.arg_value;
                break;
            case 'g':
                opts.group_filter = arg.arg_value;
                break;
            case 't':
                if (arg.arg_value == "f" || arg.arg_value == "d" || arg.arg_value == "l") {
                    opts.type_filter = arg.arg_value[0];
                } else {
                    std::cerr << std::format("Error: invalid type '{}'. Use f (file), d (directory), or l (symlink).\n", arg.arg_value);
                    return EXIT_FAILURE;
                }
                break;
            case 'h':
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
    return 0;
}
