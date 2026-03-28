/*

 shell-cmd v1.0
 https://lostsidedead.biz
 GNU GPL v3
   
*/

#include<iostream>
#include<string>
#include<vector>
#include<unistd.h>
#include<sys/wait.h>
#include<signal.h>
#include<cstdlib>
#include<regex>
#include<filesystem>
#include<format>
#include<span>
#include"argz.hpp"

namespace fs = std::filesystem;

struct Options {
    bool dry_run = false;
    bool verbose = false;
    bool hidden = false;
    int max_depth = -1;
};

static Options opts;

void proc_cmd(const std::string &cmd, std::span<const std::string> text);
std::string replace_string(std::string orig, const std::string &with, const std::string &rep);
void add_directory(const fs::path &path, const std::string &cmd, const std::string &regex_str, std::vector<std::string> &args, int depth);
int System(const std::string &command);

std::string replace_string(std::string orig, const std::string &with, const std::string &rep) {
    size_t pos = 0;
    while((pos = orig.find(with, pos)) != std::string::npos) {
        orig.replace(pos, with.length(), rep);
        pos += rep.length();
    }
    return orig;
}

void add_directory(const fs::path &path, const std::string &cmd, const std::string &regex_str, std::vector<std::string> &args, int depth) {
    if(opts.max_depth >= 0 && depth > opts.max_depth) return;

    std::error_code ec;
    auto dir = fs::directory_iterator(path, fs::directory_options::skip_permission_denied, ec);
    if(ec) {
        std::cerr << std::format("Error: could not open directory: {}\n", path.string());
        exit(EXIT_FAILURE);
    }

    for(const auto &entry : dir) {
        auto filename = entry.path().filename().string();
        if(!opts.hidden && filename.starts_with('.')) continue;

        if(entry.is_directory(ec)) {
            add_directory(entry.path(), cmd, regex_str, args, depth + 1);
        } else if(entry.is_regular_file(ec)) {
            std::regex ex(regex_str);
            auto fullpath = entry.path().string();
            if(std::regex_search(fullpath, ex)) {
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
    
    if(command.empty()) return System(":") == 0;
    
    sigemptyset(&bmask);
    sigaddset(&bmask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &bmask, &omask);
    sa_ignore.sa_handler = SIG_IGN;
    sa_ignore.sa_flags = 0;
    sigemptyset(&sa_ignore.sa_mask);
    sigaction(SIGINT, &sa_ignore, &sa_origint);
    sigaction(SIGQUIT, &sa_ignore, &sa_oquit);
    
    switch((id = fork())) {
        case -1:
            status = -1;
            break;
        case 0:
            sa_default.sa_handler = SIG_DFL;
            sa_default.sa_flags = 0;
            sigemptyset(&sa_default.sa_mask);
            if(sa_origint.sa_handler != SIG_IGN)
                sigaction(SIGINT, &sa_default, NULL);
            if(sa_oquit.sa_handler != SIG_IGN)
                sigaction(SIGQUIT, &sa_default, NULL);
            
            execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
            _exit(127);
            break;
        default:
            while(waitpid(id, &status, 0) == -1) {
                if(errno != EINTR) {
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
    if(!text.empty()) {
        auto fname = fs::path(text[0]).filename().string();
        r = replace_string(r, "%0", fname);
    }
    for(size_t i = 0; i < text.size(); ++i) {
        auto placeholder = std::format("%{}", i + 1);
        if(i == 0 && text[i].find(' ') != std::string::npos)
            r = replace_string(r, placeholder, std::format("\"{}\"", text[i]));
        else
            r = replace_string(r, placeholder, std::string{text[i]});
    }
    if(opts.verbose || opts.dry_run)
        std::cout << r << "\n";
    if(!opts.dry_run)
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
        "  -n          dry-run, print commands without executing\n"
        "  -v          verbose, print each command before running\n"
        "  -a          include hidden files/directories\n"
        "  -d depth    max recursion depth (0 = current dir only)\n"
        "  -h          show this help\n", prog);
}

int main(int argc, char **argv) {
    Argz<std::string> argz(argc, argv);
    argz.addOptionSingle('n', "dry-run mode")
        .addOptionSingle('v', "verbose output")
        .addOptionSingle('a', "include hidden files")
        .addOptionSingleValue('d', "max depth")
        .addOptionSingle('h', "show help");

    std::vector<std::string> positional;

    try {
        Argument<std::string> arg;
        int ret;
        while((ret = argz.proc(arg)) != -1) {
            switch(ret) {
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
                case 'h':
                    print_help(argv[0]);
                    return 0;
                case '-':
                    positional.push_back(arg.arg_value);
                    break;
            }
        }
    } catch(const ArgException<std::string> &e) {
        std::cerr << std::format("Error: {}\n", e.text());
        return EXIT_FAILURE;
    }

    if(positional.size() < 3) {
        std::cerr << "Error: at least three positional arguments required.\n";
        print_help(argv[0]);
        return EXIT_FAILURE;
    }

    const auto &path = positional[0];
    const auto &input = positional[1];
    const auto &regex_str = positional[2];

    size_t index = 2;
    std::vector<std::string> args{"filename"};
    for(size_t i = 3; i < positional.size(); ++i) {
        if(input.find(std::format("%{}", index)) == std::string::npos) {
            std::cerr << std::format("Error: command has no placeholder %{} for extra argument \"{}\"\n", index, positional[i]);
            return EXIT_FAILURE;
        }
        args.push_back(positional[i]);
        ++index;
    }

    add_directory(path, input, regex_str, args, 0);
    return 0;
}
