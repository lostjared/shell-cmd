/**
 * @file cmd.cpp
 * @brief shell-cmd v1.3 — Recursively find files matching a regex and execute a shell command for each match.
 * @details Walks a directory tree using std::filesystem, applies metadata filters (size, time,
 *          permissions, ownership, type), substitutes placeholders in a command template, and
 *          executes the resulting command for every matched entry. Supports parallel execution,
 *          exclude patterns (regex or glob via @c -i / @c --glob-exclude), confirm mode,
 *          stop-on-error, list-all mode (@c -l / @c --list-all) which collects all matched
 *          paths and runs the command once with @c %%0 expanded to the full list, and expression
 *          filters (@c -f / @c --expr) which allow combining @c glob(), @c regex(), and
 *          @c regex_match() predicates with boolean operators @c and, @c or, and @c not.
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

/**
 * @brief Check whether to use color output on the given file descriptor.
 * @details Respects the NO_COLOR environment variable convention (https://no-color.org/).
 * @param fd File descriptor to check (1 = stdout, 2 = stderr).
 * @return true if the fd is a terminal and NO_COLOR is not set.
 */
static bool use_color(int fd) {
    if (std::getenv("NO_COLOR") != nullptr)
        return false;
    return isatty(fd) != 0;
}

/**
 * @brief Print a colored error message to stderr.
 * @details Prefixes the message with "Error: " (bold red when color is enabled).
 * @param msg The error message to print.
 */
static void print_error(const std::string &msg) {
    if (use_color(2))
        std::cerr << std::format("\x1b[1;31mError:\x1b[0m {}\n", msg);
    else
        std::cerr << std::format("Error: {}\n", msg);
}

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

enum class RegExMode {
    REGEX_SEARCH=1,
    REGEX_MATCH
};

/// @brief Aggregates all runtime options parsed from the command line.
struct Options {
    bool dry_run = false;            ///< Print commands without executing.
    bool verbose = false;            ///< Print commands before executing.
    bool hidden = false;             ///< Include hidden (dot) files/directories.
    int max_depth = -1;              ///< Max recursion depth (-1 = unlimited).
    SizeFilter size_filter;          ///< Optional size filter.
    TimeFilter mtime_filter;         ///< Optional modification-time filter.
    std::string perm_filter;         ///< Octal permission string, e.g. "755".
    std::string user_filter;         ///< Owner username filter.
    std::string group_filter;        ///< Group name filter.
    char type_filter = 0;            ///< Type filter: 'f' file, 'd' directory, 'l' symlink.
    std::string exclude_pattern;     ///< Regex (or glob, see glob_exclude) pattern to exclude files/dirs.
    bool stop_on_error = false;      ///< Halt on first command failure.
    bool confirm = false;            ///< Prompt for confirmation before each command.
    int jobs = 1;                    ///< Number of parallel jobs (1 = sequential).
    std::string shell = "/bin/bash"; ///< Shell to use for command execution.
    std::string shell_name = "bash"; ///< Shell argv[0] name.
    bool collect_all = false;        ///< If true (via -l/--list-all), collect all matched file paths and run one command with a combined argument list.
    RegExMode mode = RegExMode::REGEX_SEARCH; ///< RegEx mode
    bool glob = false;               ///< If true, treat search pattern as a glob instead of regex.
    bool glob_exclude = false;       ///< If true (via -i/--glob-exclude), treat exclude pattern as a glob instead of regex.
    std::string expr_str;            ///< Expression filter string from --expr.
};

static Options opts; ///< Global runtime options.

/// @brief Tracks execution statistics printed in the summary.
struct Stats {
    int files_matched = 0;   ///< Number of entries that matched all filters.
    int commands_run = 0;    ///< Number of commands executed (or printed in dry-run).
    int commands_failed = 0; ///< Number of commands that returned non-zero.
};

static Stats stats;                           ///< Global execution statistics.
static std::vector<pid_t> child_pids;         ///< PIDs of outstanding child processes (parallel mode).
static bool stop_requested = false;           ///< Set to true when stop-on-error triggers.
static volatile sig_atomic_t interrupted = 0; ///< Set to 1 by SIGINT handler (Ctrl+C).

/**
 * @brief Signal handler for SIGINT (Ctrl+C).
 * @details Sets the interrupted flag so the main loop exits cleanly.
 */
static void sigint_handler(int /*sig*/) {
    interrupted = 1;
}

std::string glob_to_regex(const std::string &glob);
SizeFilter parse_size_filter(const std::string &s);
TimeFilter parse_time_filter(const std::string &s);
bool matches_filters(const fs::directory_entry &entry);
bool proc_cmd(const std::string &cmd, std::span<const std::string> text, std::string file_string = "");
void wait_for_slot();
void wait_all();
std::string replace_string(std::string orig, const std::string &with, const std::string &rep);
void fill_list(const fs::path &path, const std::string &cmd, const std::string &regex_str, std::vector<std::string> &args, std::vector<std::string> &files, int depth);
std::string join(std::vector<std::string> &v);
void add_directory(const fs::path &path, const std::string &cmd, const std::string &regex_str, std::vector<std::string> &args, int depth);
int System(const std::string &command);

/**
 * @brief Convert a glob pattern to an equivalent regex string.
 * @details Escapes regex-special characters and translates glob wildcards:
 *          '*' becomes '.*', '?' becomes '.', character class brackets '[...]'
 *          are passed through with '!' or '^' mapped to '^' for negation,
 *          and all other special characters are escaped with a backslash.
 *          The result is anchored with '^' and '$'.
 *
 *          Used by @c --glob to convert the search pattern to regex, and by
 *          @c --glob-exclude (@c -i) to convert the exclude pattern to regex.
 * @param glob The glob pattern string, e.g. "*.cpp", "test?", "[!a-z]*".
 * @return The equivalent anchored regex string, e.g. "^.*\.cpp$".
 */
std::string glob_to_regex(const std::string &glob) {
    std::string result;
    result += '^';

    bool in_class = false;

    for (size_t i = 0; i < glob.size(); ++i) {
        char c = glob[i];

        if (in_class) {
            if (c == ']') {
                in_class = false;
                result += ']';
            } else if (c == '\\') {
                result += "\\\\";
            } else {
                result += c;
            }
            continue;
        }

        switch (c) {
        case '*':
            result += ".*";
            break;
        case '?':
            result += '.';
            break;
        case '[':
            in_class = true;
            result += '[';
            if (i + 1 < glob.size() && (glob[i + 1] == '!' || glob[i + 1] == '^')) {
                result += '^';
                ++i;
            }
            break;
        case '.':
        case '\\':
        case '+':
        case '^':
        case '$':
        case '|':
        case '(':
        case ')':
        case '{':
        case '}':
            result += '\\';
            result += c;
            break;
        default:
            result += c;
            break;
        }
    }

    if (in_class)
        result += '\\';

    result += '$';
    return result;
}

// --- Expression filter (--expr) ------------------------------------------------

/// @brief Node types for the expression filter AST.
enum class ExprType { GLOB, REGEX_SEARCH, REGEX_MATCH, AND, OR, NOT };

/// @brief AST node for expression-based file matching.
struct ExprNode {
    ExprType type;
    std::regex compiled;               ///< Pre-compiled regex (leaf nodes only).
    std::unique_ptr<ExprNode> left;    ///< Left child (AND/OR) or sole child (NOT).
    std::unique_ptr<ExprNode> right;   ///< Right child (AND/OR only).

    /// @brief Evaluate this expression node against a file path.
    bool evaluate(const std::string &path) const {
        switch (type) {
        case ExprType::GLOB:
            return std::regex_search(path, compiled);
        case ExprType::REGEX_SEARCH:
            return std::regex_search(path, compiled);
        case ExprType::REGEX_MATCH:
            return std::regex_match(path, compiled);
        case ExprType::AND:
            return left->evaluate(path) && right->evaluate(path);
        case ExprType::OR:
            return left->evaluate(path) || right->evaluate(path);
        case ExprType::NOT:
            return !left->evaluate(path);
        }
        return false;
    }
};

/// @brief Token produced by the expression tokenizer.
struct ExprToken {
    enum Type { IDENT, STRING, LPAREN, RPAREN, END } type;
    std::string value;
};

/// @brief Tokenizer for expression filter strings.
class ExprTokenizer {
    const std::string &src;
    size_t pos = 0;
    void skip_ws() {
        while (pos < src.size() && std::isspace(static_cast<unsigned char>(src[pos])))
            ++pos;
    }
public:
    explicit ExprTokenizer(const std::string &s) : src(s) {}
    ExprToken next() {
        skip_ws();
        if (pos >= src.size())
            return {ExprToken::END, ""};
        char c = src[pos];
        if (c == '(') { ++pos; return {ExprToken::LPAREN, "("}; }
        if (c == ')') { ++pos; return {ExprToken::RPAREN, ")"}; }
        if (c == '"' || c == '\'') {
            char q = c;
            ++pos;
            std::string val;
            while (pos < src.size() && src[pos] != q) {
                if (src[pos] == '\\' && pos + 1 < src.size()) {
                    ++pos;
                    val += src[pos];
                } else {
                    val += src[pos];
                }
                ++pos;
            }
            if (pos < src.size())
                ++pos;
            return {ExprToken::STRING, val};
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::string val;
            while (pos < src.size() &&
                   (std::isalnum(static_cast<unsigned char>(src[pos])) || src[pos] == '_'))
                val += src[pos++];
            return {ExprToken::IDENT, val};
        }
        throw std::runtime_error(
            std::format("unexpected character '{}' in expression at position {}", c, pos));
    }
};

/// @brief Recursive-descent parser for expression filter strings.
///
/// Grammar:
///   expr     := or_expr
///   or_expr  := and_expr ("or" and_expr)*
///   and_expr := not_expr ("and" not_expr)*
///   not_expr := "not" not_expr | primary
///   primary  := function "(" STRING ")" | "(" expr ")"
///   function := "glob" | "regex" | "regex_search" | "regex_match"
class ExprParser {
    ExprTokenizer tok;
    ExprToken cur;
    void advance() { cur = tok.next(); }
    void expect(ExprToken::Type t, const std::string &desc) {
        if (cur.type != t)
            throw std::runtime_error(std::format(
                "expected {} in expression, got '{}'", desc,
                cur.value.empty() ? "end" : cur.value));
        advance();
    }
    std::unique_ptr<ExprNode> parse_primary() {
        if (cur.type == ExprToken::LPAREN) {
            advance();
            auto node = parse_or();
            expect(ExprToken::RPAREN, "')'");
            return node;
        }
        if (cur.type != ExprToken::IDENT)
            throw std::runtime_error(std::format(
                "unexpected token '{}' in expression",
                cur.value.empty() ? "end" : cur.value));
        std::string name = cur.value;
        ExprType ft;
        if (name == "glob")
            ft = ExprType::GLOB;
        else if (name == "regex" || name == "regex_search")
            ft = ExprType::REGEX_SEARCH;
        else if (name == "regex_match")
            ft = ExprType::REGEX_MATCH;
        else
            throw std::runtime_error(
                std::format("unknown function '{}' in expression", name));
        advance();
        expect(ExprToken::LPAREN, "'(' after function name");
        if (cur.type != ExprToken::STRING)
            throw std::runtime_error(
                "expected quoted string as function argument");
        std::string pattern = cur.value;
        advance();
        expect(ExprToken::RPAREN, "')'");
        auto node = std::make_unique<ExprNode>();
        node->type = ft;
        node->compiled = std::regex(
            ft == ExprType::GLOB ? glob_to_regex(pattern) : pattern,
            std::regex::ECMAScript);
        return node;
    }
    std::unique_ptr<ExprNode> parse_not() {
        if (cur.type == ExprToken::IDENT && cur.value == "not") {
            advance();
            auto child = parse_not();
            auto node = std::make_unique<ExprNode>();
            node->type = ExprType::NOT;
            node->left = std::move(child);
            return node;
        }
        return parse_primary();
    }
    std::unique_ptr<ExprNode> parse_and() {
        auto left = parse_not();
        while (cur.type == ExprToken::IDENT && cur.value == "and") {
            advance();
            auto right = parse_not();
            auto node = std::make_unique<ExprNode>();
            node->type = ExprType::AND;
            node->left = std::move(left);
            node->right = std::move(right);
            left = std::move(node);
        }
        return left;
    }
    std::unique_ptr<ExprNode> parse_or() {
        auto left = parse_and();
        while (cur.type == ExprToken::IDENT && cur.value == "or") {
            advance();
            auto right = parse_and();
            auto node = std::make_unique<ExprNode>();
            node->type = ExprType::OR;
            node->left = std::move(left);
            node->right = std::move(right);
            left = std::move(node);
        }
        return left;
    }
public:
    explicit ExprParser(const std::string &s) : tok(s) {}
    std::unique_ptr<ExprNode> parse() {
        advance();
        auto root = parse_or();
        if (cur.type != ExprToken::END)
            throw std::runtime_error(
                "unexpected content after expression");
        return root;
    }
};

static std::unique_ptr<ExprNode> expr_root; ///< Parsed expression tree (set when --expr is used).

/// @brief Check whether a path matches the active search pattern or expression.
static bool entry_matches_path(const std::string &fullpath, const std::string &regex_str) {
    if (expr_root)
        return expr_root->evaluate(fullpath);
    std::regex ex(regex_str, std::regex::ECMAScript);
    if (opts.mode == RegExMode::REGEX_SEARCH)
        return std::regex_search(fullpath, ex);
    return std::regex_match(fullpath, ex);
}

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
 * @brief Recursively collect all file paths matching a regex and metadata filters.
 * @details Used by the @c -l / @c --list-all mode. Walks the directory tree in the
 *          same way as add_directory(), but instead of executing a command for each
 *          match it appends the full path to @p files. After the traversal the caller
 *          joins the collected paths and passes them to proc_cmd() as the @c file_string
 *          argument so that @c %%0 expands to the entire list.
 * @param path      The directory to scan.
 * @param cmd       The command template (unused during collection; kept for signature symmetry).
 * @param regex_str ECMAScript regex matched against each entry's full path.
 * @param args      Mutable argument vector forwarded from main().
 * @param files     [out] Vector that accumulates the full paths of all matched entries.
 * @param depth     Current recursion depth (0 at the root call).
 */
void fill_list(const fs::path &path, const std::string &cmd, const std::string &regex_str, std::vector<std::string> &args, std::vector<std::string> &files, int depth) {
    if (opts.max_depth >= 0 && depth > opts.max_depth)
        return;
    if (stop_requested || interrupted)
        return;

    std::error_code ec;
    auto dir = fs::directory_iterator(path, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        print_error(std::format("could not open directory: {}", path.string()));
        exit(EXIT_FAILURE);
    }

    for (const auto &entry : dir) {
        if (stop_requested || interrupted)
            return;
        auto filename = entry.path().filename().string();
        if (!opts.hidden && filename.starts_with('.'))
            continue;

        // Exclude pattern check
        if (!opts.exclude_pattern.empty()) {
            std::regex excl(opts.exclude_pattern, std::regex::ECMAScript);
            if(opts.mode == RegExMode::REGEX_SEARCH) {
                if (std::regex_search(filename, excl)) 
                    continue;
            } else if(opts.mode == RegExMode::REGEX_MATCH) {
                if(std::regex_match(filename,excl))
                    continue;
            }
        }

        if (entry.is_directory(ec)) {
            if (opts.type_filter == 'd') {
                auto fullpath = entry.path().string();
                if (entry_matches_path(fullpath, regex_str) && matches_filters(entry)) {
                    stats.files_matched++;
                    return;
                }
            }
            fill_list(entry.path(), cmd, regex_str, args, files, depth + 1);
        } else if (entry.is_symlink(ec) && opts.type_filter == 'l') {
            auto fullpath = entry.path().string();
            if (entry_matches_path(fullpath, regex_str) && matches_filters(entry)) {
                stats.files_matched++;
                files.push_back(fullpath);
            }
        } else if (entry.is_regular_file(ec) || (entry.is_symlink(ec) && opts.type_filter == 0)) {
            auto fullpath = entry.path().string();
            if (entry_matches_path(fullpath, regex_str) && matches_filters(entry)) {
                stats.files_matched++;
                files.push_back(fullpath);
            }
        }
    }
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
    if (stop_requested || interrupted)
        return;

    std::error_code ec;
    auto dir = fs::directory_iterator(path, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        print_error(std::format("could not open directory: {}", path.string()));
        exit(EXIT_FAILURE);
    }

    for (const auto &entry : dir) {
        if (stop_requested || interrupted)
            return;
        auto filename = entry.path().filename().string();
        if (!opts.hidden && filename.starts_with('.'))
            continue;

        // Exclude pattern check
        if (!opts.exclude_pattern.empty()) {
            std::regex excl(opts.exclude_pattern, std::regex::ECMAScript);
            if(opts.mode == RegExMode::REGEX_SEARCH) {
                if (std::regex_search(filename, excl))
                    continue;
            } else if(opts.mode == RegExMode::REGEX_MATCH) {
                if (std::regex_match(filename, excl))
                    continue;
            }
        }

        if (entry.is_directory(ec)) {
            if (opts.type_filter == 'd') {
                auto fullpath = entry.path().string();
                if (entry_matches_path(fullpath, regex_str) && matches_filters(entry)) {
                    stats.files_matched++;
                    args[0] = fullpath;
                    if (!proc_cmd(cmd, args))
                        return;
                }
            }
            add_directory(entry.path(), cmd, regex_str, args, depth + 1);
        } else if (entry.is_symlink(ec) && opts.type_filter == 'l') {
            auto fullpath = entry.path().string();
            if (entry_matches_path(fullpath, regex_str) && matches_filters(entry)) {
                stats.files_matched++;
                args[0] = fullpath;
                if (!proc_cmd(cmd, args))
                    return;
            }
        } else if (entry.is_regular_file(ec) || (entry.is_symlink(ec) && opts.type_filter == 0)) {
            auto fullpath = entry.path().string();
            if (entry_matches_path(fullpath, regex_str) && matches_filters(entry)) {
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
 * @param command The shell command string to execute via the configured shell.
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
    // Use our sigint_handler instead of SIG_IGN so Ctrl+C is recorded
    struct sigaction sa_int;
    sa_int.sa_handler = sigint_handler;
    sa_int.sa_flags = 0;
    sigemptyset(&sa_int.sa_mask);
    sigaction(SIGINT, &sa_int, &sa_origint);
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

        execl(opts.shell.c_str(), opts.shell_name.c_str(), "-c", command.c_str(), static_cast<char *>(nullptr));
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
    // If the child was killed by SIGINT (or the shell caught it and exited 130),
    // flag the parent to exit cleanly
    if ((WIFSIGNALED(status) && WTERMSIG(status) == SIGINT) ||
        (WIFEXITED(status) && WEXITSTATUS(status) == 130))
        interrupted = 1;
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
 * @brief Join a vector of strings into a single space-delimited string.
 * @details Used by the @c --list-all mode to combine all matched file paths into
 *          one string that replaces @c %%0 in the command template.
 * @param v The vector of strings to join.
 * @return A string with each element separated by a space (trailing space included).
 */
std::string join(std::vector<std::string> &v) {
    std::string temp;
    for (auto &i : v) {
        temp += i + " ";
    }
    return temp;
}

/**
 * @brief Substitute placeholders in a command template and execute the result.
 * @details
 *   - Default mode (one invocation per match): %0=basename, %1=full path, %2+ extra args.
 *   - --list-all mode (-l): collects all matching file paths into a single space-delimited
 *     string and passes this as file_string. In this mode %0 is replaced with the full
 *     list of matches, not individual filenames.
 *   - Supports confirm mode, dry-run, parallel forking, and stop-on-error.
 * @param cmd  The command template string.
 * @param text Span of strings: text[0] is the matched file path, text[1+] are extras.
 * @param file_string When --list-all is set, contains all matched paths joined by spaces.
 * @return true to continue processing, false to stop (stop-on-error triggered).
 */
bool proc_cmd(const std::string &cmd, std::span<const std::string> text, std::string file_string) {
    std::string r = cmd;
    if (!text.empty()) {
        if (file_string.empty()) {
            auto fpath = fs::path(text[0]);
            auto fname = fpath.filename().string();
            r = replace_string(r, "%0", fname);
            r = replace_string(r, "%b", fpath.stem().string());
            r = replace_string(r, "%e", fpath.extension().string());
        }
    }
    if (file_string.empty() && !text.empty()) {
        for (size_t i = 0; i < text.size(); ++i) {
            auto placeholder = std::format("%{}", i + 1);
            if (i == 0 && text[i].find(' ') != std::string::npos)
                r = replace_string(r, placeholder, std::format("\"{}\"", text[i]));
            else
                r = replace_string(r, placeholder, std::string{text[i]});
        }
    } else {

        r = replace_string(r, "%0", file_string);
        for (size_t i = 0; i < text.size(); ++i) {
            std::string placeholder = std::format("%{}", i + 1);
            if (text[i].find(' ') != std::string::npos) {
                r = replace_string(r, placeholder, std::format("\"{}\"", text[i]));
            } else
                r = replace_string(r, placeholder, std::string{text[i]});
        }
    }

    if (opts.confirm) {
        if (use_color(1))
            std::cout << std::format("\x1b[1;33mExecute:\x1b[0m {} \x1b[1m[y/N]\x1b[0m ", r);
        else
            std::cout << std::format("Execute: {} ? [y/N] ", r);
        std::string answer;
        std::getline(std::cin, answer);
        if (answer != "y" && answer != "Y")
            return true;
    }

    if (opts.verbose || opts.dry_run) {
        if (use_color(1))
            std::cout << std::format("\x1b[36m{}\x1b[0m\n", r);
        else
            std::cout << r << "\n";
    }

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
            execl(opts.shell.c_str(), opts.shell_name.c_str(), "-c", r.c_str(), static_cast<char *>(nullptr));
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
    if (interrupted)
        return false;
    if (ret != 0) {
        stats.commands_failed++;
        if (opts.stop_on_error) {
            print_error(std::format("command failed (exit {}), stopping.", WEXITSTATUS(ret)));
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
    bool co = use_color(1);
    std::string b  = co ? "\x1b[1m" : "";
    std::string bw = co ? "\x1b[1;37m" : "";
    std::string by = co ? "\x1b[1;33m" : "";
    std::string g  = co ? "\x1b[32m" : "";
    std::string r  = co ? "\x1b[0m" : "";
    std::cout
        << b << "usage:" << r << " " << bw << prog << r << " [options] path \"command %1 [%2 %3..]\" regex [extra_args..]\n\n"
        << bw << "Recursively find files matching regex and run command for each." << r << "\n\n"
        << by << "placeholders:" << r << "\n"
        << "  " << g << "%0" << r << "          filename only (no path, per-match mode)\n"
        << "  " << g << "%1" << r << "          full path to matched file\n"
        << "  " << g << "%2+" << r << "         extra arguments from command line\n"
        << "  " << g << "%b" << r << "          basename without extension\n"
        << "  " << g << "%e" << r << "          file extension (including dot)\n\n"
        << "  (with -l/--list-all) %0 expands to all matched paths joined by spaces\n\n"
        << by << "options:" << r << "\n"
        << "  " << g << "-z, --regex-match" << r << "   Use regex_match instead of search\n"
        << "  " << g << "-b, --glob" << r << "          Treat pattern as a glob (*, ?) instead of regex\n"
        << "  " << g << "-n, --dry-run" << r << "       dry-run, print commands without executing\n"
        << "  " << g << "-v, --verbose" << r << "       verbose, print each command before running\n"
        << "  " << g << "-a, --all" << r << "           include hidden files/directories\n"
        << "  " << g << "-l, --list-all" << r << "      collect all matches and invoke command once with %0=all-matches\n"
        << "  " << g << "-d, --depth N" << r << "       max recursion depth (0 = current dir only)\n"
        << "  " << g << "-s, --size SIZE" << r << "     filter by size: +10M (>10MB), -1K (<1KB),\n"
        << "                      4096 (exactly 4096 bytes). Suffixes: K, M, G\n"
        << "  " << g << "-m, --mtime DAYS" << r << "    filter by modification time: +7 (older than 7 days),\n"
        << "                      -1 (modified within last day), 3 (exactly 3 days)\n"
        << "  " << g << "-p, --perm MODE" << r << "     filter by permissions (octal), e.g. 755\n"
        << "  " << g << "-u, --user USER" << r << "     filter by owner username\n"
        << "  " << g << "-g, --group GROUP" << r << "   filter by group name\n"
        << "  " << g << "-t, --type TYPE" << r << "     filter by type: f (file), d (directory), l (symlink)\n"
        << "  " << g << "-x, --exclude REGEX" << r << " exclude files/directories matching REGEX\n"
        << "  " << g << "-i, --glob-exclude" << r << "  treat exclude pattern as a glob instead of regex\n"
        << "  " << g << "-f, --expr EXPR" << r << "     filter expression: glob(), regex(), regex_match(),\n"
        << "                      combined with and/or/not and parentheses\n"
        << "  " << g << "-e, --stop-on-error" << r << " stop on first command failure\n"
        << "  " << g << "-c, --confirm" << r << "       prompt for confirmation before each command\n"
        << "  " << g << "-j, --jobs N" << r << "        run N commands in parallel (default: 1)\n"
        << "  " << g << "-w, --shell SHELL" << r << "   shell to use for execution (default: /bin/bash)\n"
        << "  " << g << "-h, --help" << r << "          show this help\n";
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
    // Install SIGINT handler for clean Ctrl+C exit
    struct sigaction sa_int;
    sa_int.sa_handler = sigint_handler;
    sa_int.sa_flags = 0;
    sigemptyset(&sa_int.sa_mask);
    sigaction(SIGINT, &sa_int, nullptr);

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
        .addOptionSingleValue('w', "shell path")
        .addOptionDoubleValue('W', "shell", "shell path")
        .addOptionSingle('l', "list all matches")
        .addOptionDouble('L', "list-all", "list all matches")
        .addOptionSingle('h', "show help")
        .addOptionSingle('z', "regex match mode")
        .addOptionDouble('Z', "regex-match", "Regex mode match")
        .addOptionSingle('b', "glob mode")
        .addOptionDouble('B', "glob", "glob mode")
        .addOptionSingle('i', "glob exclude mode")
        .addOptionDouble('I', "glob-exclude", "glob exclude mode")
        .addOptionSingleValue('f', "filter expression")
        .addOptionDoubleValue('F', "expr", "filter expression")
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
                    print_error(std::format("invalid type '{}'. Use f (file), d (directory), or l (symlink).", arg.arg_value));
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
            case 'w':
            case 'W': {
                opts.shell = arg.arg_value;
                auto slash = opts.shell.rfind('/');
                opts.shell_name = (slash != std::string::npos) ? opts.shell.substr(slash + 1) : opts.shell;
                break;
            }
            case 'l':
            case 'L':
                opts.collect_all = true;
                break;
            case 'h':
            case 'H':
                print_help(argv[0]);
                return 0;
            case 'Z':
            case 'z':
                opts.mode = RegExMode::REGEX_MATCH;
                break;
            case 'b':
            case 'B':
                opts.glob = true;
                break;
            case 'i':
            case 'I':
                opts.glob_exclude = true;
                break;
            case 'f':
            case 'F':
                opts.expr_str = arg.arg_value;
                break;
            case '-':
                positional.push_back(arg.arg_value);
                break;
            }
        }
    } catch (const ArgException<std::string> &e) {
        print_error(e.text());
        return EXIT_FAILURE;
    }

    size_t min_pos = opts.expr_str.empty() ? 3 : 2;
    if (positional.size() < min_pos) {
        print_error(opts.expr_str.empty()
            ? "at least three positional arguments required (or use --expr)."
            : "at least two positional arguments required with --expr.");
        print_help(argv[0]);
        return EXIT_FAILURE;
    }

    try {
        const auto &path = positional[0];
        const auto &input = positional[1];
        std::string regex_str;
        if (!opts.expr_str.empty()) {
            expr_root = ExprParser(opts.expr_str).parse();
        } else {
            regex_str = opts.glob ? glob_to_regex(positional[2]) : positional[2];
        }
        if (opts.glob_exclude && !opts.exclude_pattern.empty())
            opts.exclude_pattern = glob_to_regex(opts.exclude_pattern);
        size_t index = (opts.collect_all) ? 1 : 2;
        std::vector<std::string> args;
        if (!opts.collect_all)
            args.push_back("filename");
        size_t extra_start = opts.expr_str.empty() ? 3 : 2;
        for (size_t i = extra_start; i < positional.size(); ++i) {
            if (input.find(std::format("%{}", index)) == std::string::npos) {
                print_error(std::format("command has no placeholder %{} for extra argument \"{}\"", index, positional[i]));
                return EXIT_FAILURE;
            }
            args.push_back(positional[i]);
            ++index;
        }
        if (opts.collect_all) {
            std::vector<std::string> files;
            fill_list(path, input, regex_str, args, files, 0);
            std::string all_files = join(files);
            if (proc_cmd(input, args, all_files)) {
                if (opts.verbose) {
                    std::cout << std::format("Success command file list: {} .\n", all_files);
                }
                return EXIT_SUCCESS;
            } else {
                std::cout << "List all command failed.\n";
                return EXIT_FAILURE;
            }
            return EXIT_SUCCESS;
        }

        add_directory(path, input, regex_str, args, 0);
        if (opts.jobs > 1)
            wait_all();

        if (interrupted) {
            // Kill outstanding child processes
            for (pid_t pid : child_pids)
                kill(pid, SIGTERM);
            for (pid_t pid : child_pids)
                waitpid(pid, nullptr, 0);
            child_pids.clear();
            std::cerr << "\nInterrupted.\n";
            if (opts.verbose || opts.dry_run || stats.commands_failed > 0 || stats.commands_run > 0) {
                if (use_color(2)) {
                    std::cerr << std::format("\x1b[1mSummary:\x1b[0m \x1b[1;32m{}\x1b[0m matched, \x1b[1;33m{}\x1b[0m run, {}{}\x1b[0m failed\n",
                                             stats.files_matched, stats.commands_run,
                                             stats.commands_failed > 0 ? "\x1b[1;31m" : "\x1b[1;32m",
                                             stats.commands_failed);
                } else {
                    std::cerr << std::format("Summary: {} matched, {} run, {} failed\n",
                                             stats.files_matched, stats.commands_run, stats.commands_failed);
                }
            }
            return 130;
        }

        if (opts.verbose || opts.dry_run || stats.commands_failed > 0) {
            if (use_color(2)) {
                std::cerr << std::format("\n\x1b[1mSummary:\x1b[0m \x1b[1;32m{}\x1b[0m matched, \x1b[1;33m{}\x1b[0m run, {}{}\x1b[0m failed\n",
                                         stats.files_matched, stats.commands_run,
                                         stats.commands_failed > 0 ? "\x1b[1;31m" : "\x1b[1;32m",
                                         stats.commands_failed);
            } else {
                std::cerr << std::format("\nSummary: {} matched, {} run, {} failed\n",
                                         stats.files_matched, stats.commands_run, stats.commands_failed);
            }
        }
    } catch (const std::exception &e) {
        std::cerr << "Exception: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return stats.commands_failed > 0 ? EXIT_FAILURE : 0;
}
