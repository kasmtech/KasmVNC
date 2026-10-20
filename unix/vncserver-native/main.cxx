// SPDX-License-Identifier: GPL-2.0-or-later
// Native port of unix/vncserver and KasmVNC modules. Original launcher:
// Copyright 1999 AT&T; 2002-2005 RealVNC; 2002-2003 Constantin Kaplinsky;
// 2005-2006 Sun Microsystems; 2009-2010 D. R. Commander.
// Native session launcher. User xstartup is deliberately a shell program;
// configuration and helper arguments are always passed directly to exec.
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <grp.h>
#include <iostream>
#include <limits>
#include <netdb.h>
#include <pwd.h>
#include <regex>
#include <stdexcept>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include "config.h"

namespace fs = std::filesystem;
namespace launcher {
    namespace {
        class Fd {
            int fd_;

        public:
            explicit Fd(int fd = -1) :
                fd_(fd) {}
            ~Fd() {
                if (fd_ >= 0)
                    close(fd_);
            }
            Fd(const Fd &) = delete;
            Fd &operator=(const Fd &) = delete;
            int get() const {
                return fd_;
            }
        };
        std::string env(const char *key) {
            const char *value = std::getenv(key);
            return value ? value : "";
        }
        std::string trim(std::string value) {
            auto first = value.find_first_not_of(" \r\n\t");
            if (first == std::string::npos)
                return {};
            return value.substr(first, value.find_last_not_of(" \r\n\t") - first + 1);
        }
        std::string readFile(const fs::path &path) {
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return {};
            std::string result;
            std::array<char, 8192> buffer;
            while (file.read(buffer.data(), buffer.size()) || file.gcount()) {
                result.append(buffer.data(), static_cast<std::size_t>(file.gcount()));
                if (result.size() > 16 * 1024 * 1024)
                    throw std::runtime_error("File too large: " + path.string());
            }
            return result;
        }
        void writeAll(int fd, const std::string &data) {
            std::size_t at = 0;
            while (at < data.size()) {
                auto count = write(fd, data.data() + at, data.size() - at);
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                    throw std::runtime_error("Cannot write file: " + std::string(std::strerror(errno)));
                at += static_cast<std::size_t>(count);
            }
        }
        void writeFile(
            const fs::path &path, const std::string &data, mode_t mode, bool exclusive = false, bool setMode = true) {
            Fd fd(open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | (exclusive ? O_EXCL : 0), mode));
            if (fd.get() < 0)
                throw std::runtime_error("Cannot write " + path.string() + ": " + std::strerror(errno));
            struct stat status {};
            if (fstat(fd.get(), &status) < 0 || !S_ISREG(status.st_mode) || status.st_uid != geteuid() ||
                status.st_nlink != 1)
                throw std::runtime_error("Unsafe file: " + path.string());
            if (ftruncate(fd.get(), 0) < 0)
                throw std::runtime_error("Cannot truncate " + path.string());
            writeAll(fd.get(), data);
            if (setMode && fchmod(fd.get(), mode) < 0)
                throw std::runtime_error("Cannot set permissions on " + path.string());
        }
        int number(const std::string &text, int max = std::numeric_limits<int>::max()) {
            int value = 0;
            auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size() || value < 0 || value > max)
                throw std::runtime_error("Invalid number: " + text);
            return value;
        }
        std::string findCommand(const std::string &name) {
            if (name.find('/') != std::string::npos)
                return access(name.c_str(), X_OK) == 0 ? name : "";
            std::string path = env("PATH");
            std::size_t at = 0;
            do {
                auto end = path.find(':', at);
                auto dir = path.substr(at, end - at);
                if (dir.empty())
                    dir = ".";
                auto file = dir + "/" + name;
                if (access(file.c_str(), X_OK) == 0)
                    return file;
                if (end == std::string::npos)
                    break;
                at = end + 1;
            } while (true);
            return {};
        }
        int waitChild(pid_t pid) {
            int status;
            while (waitpid(pid, &status, 0) < 0) {
                if (errno != EINTR)
                    throw std::runtime_error("Cannot wait for child");
            }
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        }
        // Own the pipe endpoints in the parent; the child only performs async-signal-safe
        // descriptor operations and exec after fork. Build argv before forking.
        pid_t spawn(const std::vector<std::string> &args, int input = -1, int output = -1, bool quiet = false) {
            if (args.empty())
                throw std::runtime_error("Empty child command");
            std::vector<char *> argv;
            for (const auto &arg: args)
                argv.push_back(const_cast<char *>(arg.c_str()));
            argv.push_back(nullptr);
            std::cout.flush();
            std::cerr.flush();
            pid_t pid = fork();
            if (pid < 0)
                throw std::runtime_error("Cannot fork: " + std::string(std::strerror(errno)));
            if (pid == 0) {
                std::signal(SIGPIPE, SIG_DFL);
                if (input >= 0 && dup2(input, STDIN_FILENO) < 0)
                    _exit(126);
                if (output >= 0) {
                    if (dup2(output, STDOUT_FILENO) < 0 || dup2(output, STDERR_FILENO) < 0)
                        _exit(126);
                } else if (quiet) {
                    int null = open("/dev/null", O_WRONLY);
                    if (null < 0 || dup2(null, 1) < 0 || dup2(null, 2) < 0)
                        _exit(126);
                    close(null);
                }
                execvp(argv[0], argv.data());
                _exit(errno == ENOENT ? 127 : 126);
            }
            return pid;
        }
        int run(const std::vector<std::string> &args, bool quiet = false) {
            return waitChild(spawn(args, -1, -1, quiet));
        }
        std::string quote(const std::string &arg) {
            std::string out = "'";
            for (char c: arg)
                out += c == '\'' ? "'\"'\"'" : std::string(1, c);
            return out + "'";
        }
        std::string render(const std::vector<std::string> &args, const std::string &log, std::size_t passthroughCount) {
            std::string result = args.front();
            for (std::size_t i = 1; i < args.size(); ++i)
                result += " " + ((!(i >= 2 && i < 2 + passthroughCount) && args[i].starts_with('-') &&
                                     args[i].find_first_not_of(
                                         "-abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") ==
                                         std::string::npos)
                                        ? args[i]
                                        : quote(args[i]));
            return result + " >> " + quote(log) + " 2>&1";
        }
        bool alive(pid_t pid) {
            if (pid <= 1)
                return false;
            int status;
            auto reaped = waitpid(pid, &status, WNOHANG);
            if (reaped == pid)
                return false;
            if (kill(pid, 0) != 0)
                return errno == EPERM;
            // Zombies cannot serve a display and may await a slow container init reaper.
            auto stat = readFile("/proc/" + std::to_string(pid) + "/stat");
            auto close = stat.rfind(')');
            return close == std::string::npos || stat.substr(close + 1, 3) != " Z ";
        }
        pid_t readPid(const fs::path &file) {
            Fd fd(open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
            if (fd.get() < 0) {
                if (errno == ENOENT)
                    return 0;
                throw std::runtime_error("Cannot read PID file: " + file.string());
            }
            struct stat status {};
            if (fstat(fd.get(), &status) < 0 || !S_ISREG(status.st_mode) || status.st_uid != geteuid() ||
                status.st_nlink != 1 || status.st_size > 32)
                throw std::runtime_error("Unsafe PID file: " + file.string());
            std::array<char, 33> buffer{};
            ssize_t count;
            do {
                count = read(fd.get(), buffer.data(), buffer.size());
            } while (count < 0 && errno == EINTR);
            if (count < 0 || count > 32)
                throw std::runtime_error("Cannot read PID file: " + file.string());
            auto text = trim(std::string(buffer.data(), static_cast<std::size_t>(count)));
            if (text.empty())
                return 0;
            int pid = number(text);
            if (pid <= 1)
                throw std::runtime_error("Unsafe PID: " + text);
            return pid;
        }
        std::string prompt(const std::string &message) {
            std::cout << message << std::flush;
            std::string value;
            if (!std::getline(std::cin, value))
                throw std::runtime_error("End of input while prompting");
            return trim(value);
        }
        std::vector<std::string> split(const std::string &text, char separator) {
            std::vector<std::string> out;
            std::size_t at = 0;
            for (;;) {
                auto end = text.find(separator, at);
                out.push_back(text.substr(at, end - at));
                if (end == std::string::npos)
                    break;
                at = end + 1;
            }
            return out;
        }
        const char *defaultStartup = R"STARTUP(#!/bin/sh

unset SESSION_MANAGER
unset DBUS_SESSION_BUS_ADDRESS
OS=`uname -s`
if [ $OS = 'Linux' ]; then
  case "$WINDOWMANAGER" in
    *gnome*)
      if [ -e /etc/SuSE-release ]; then
        PATH=$PATH:/opt/gnome/bin
        export PATH
      fi
      ;;
  esac
fi
if [ -x /etc/X11/xinit/xinitrc ]; then
  exec /etc/X11/xinit/xinitrc
fi
if [ -f /etc/X11/xinit/xinitrc ]; then
  exec sh /etc/X11/xinit/xinitrc
fi
[ -r $HOME/.Xresources ] && xrdb $HOME/.Xresources
xsetroot -solid grey
xterm -geometry 80x24+10+10 -ls -title "$VNCDESKTOP Desktop" &
twm
)STARTUP";
        class Session {
            std::string program, home, host, user, bin, selectDe, fontPath, safeFontPath, authority, desktop, log,
                pidFile, startup, passwd;
            int display = 1;
            bool dry = false, debug = false, prompting = false;
            std::map<std::string, std::string> cli;
            std::vector<std::string> pass;
            Config config;
            std::map<std::string, std::string> users;
            fs::path directory;
            std::string tool(const std::string &name) const {
                return bin + name;
            }
            bool has(const std::string &key) const {
                return cli.contains(key);
            }
            std::string value(const std::string &key) const {
                return join(config.get(key).values(), ",");
            }
            bool topic(const std::string &name) const {
                auto it = cli.find("-test-output-topic");
                return it == cli.end() || it->second == "all" || it->second == name;
            }
            void fonts() {
                if (fs::is_directory("/etc/X11/fontpath.d"))
                    fontPath = "catalogue:/etc/X11/fontpath.d";
                std::vector<std::string> paths{"/usr/share/X11/fonts", "/usr/share/fonts", "/usr/share/fonts/X11/"};
                for (const auto &base: {"/usr/lib/X11", "/usr/X11", "/usr/X11R6"})
                    if (!fs::is_symlink(base))
                        paths.push_back(
                            std::string(base) + (std::string(base) == "/usr/lib/X11" ? "/fonts" : "/lib/X11/fonts"));
                paths.push_back("/usr/share/fonts/default");
                std::vector<std::string> found;
                for (const auto &path: paths)
                    for (const auto &type: {"misc", "75dpi", "100dpi", "Speedo", "Type1"}) {
                        auto dir = path + "/" + type;
                        if (fs::is_regular_file(dir + "/fonts.dir") && !fs::is_symlink(dir))
                            found.push_back(dir);
                    }
                safeFontPath = join(found, ",");
                if (fontPath.empty())
                    fontPath = safeFontPath;
            }
            bool freeDisplay(int n) {
                for (int base: {6000, 5900}) {
                    Fd fd(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
                    if (fd.get() < 0)
                        throw std::runtime_error("socket failed");
                    int one = 1;
                    setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
                    sockaddr_in addr{};
                    addr.sin_family = AF_INET;
                    addr.sin_port = htons(static_cast<unsigned short>(base + n));
                    if (bind(fd.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
                        return false;
                }
                for (auto path: {"/tmp/.X" + std::to_string(n) + "-lock", "/tmp/.X11-unix/X" + std::to_string(n)})
                    if (fs::exists(path)) {
                        std::cerr << "\nWarning: " << host << ":" << n << " is taken because of " << path
                                  << "\nRemove this file if there is no X server " << host << ":" << n << "\n";
                        return false;
                    }
                return true;
            }
            void usage() const {
                std::cerr << "\nusage: " << program
                          << " [:<number>] [-desktop <desktop-name>] [-depth <depth>]\n"
                             "                 [-geometry <width>x<height>] [-pixelformat rgbNNN|bgrNNN]\n"
                             "                 [-fp <font-path>] [-fg] [-autokill] [-noxstartup]\n"
                             "                 [-xstartup <file>] <Xvnc-options>...\n\n"
                             "       "
                          << program << " -kill <X-display>\n\n       " << program << " -list\n\n";
                throw Exit{255};
            }
            void parse(int argc, char **argv) {
                const std::map<std::string, int> options{
                    {"-geometry",          1},
                    {"-kill",              1},
                    {"-help",              0},
                    {"-h",                 0},
                    {"--help",             0},
                    {"-fp",                1},
                    {"-list",              0},
                    {"-fg",                0},
                    {"-autokill",          0},
                    {"-noxstartup",        0},
                    {"-xstartup",          1},
                    {"-select-de",         2},
                    {"-interface",         1},
                    {"-debug",             0},
                    {"-websocketPort",     1},
                    {"-dry-run",           0},
                    {"-config",            1},
                    {"-test-output-topic", 1},
                    {"-prompt",            1}
                };
                for (int i = 1; i < argc; ++i) {
                    std::string arg = argv[i];
                    auto found = options.find(arg);
                    if (found == options.end()) {
                        pass.push_back(arg);
                        continue;
                    }
                    std::string val = "1";
                    if (found->second == 1) {
                        if (++i == argc)
                            usage();
                        val = argv[i];
                    } else if (found->second == 2 && i + 1 < argc && argv[i + 1][0] != '-')
                        val = argv[++i];
                    cli[arg] = val;
                }
                dry = has("-dry-run");
                debug = has("-debug");
                if (has("-help") || has("-h") || has("--help"))
                    usage();
                if (has("-kill")) {
                    stop(cli.at("-kill"));
                    throw Exit{0};
                }
                if (has("-list")) {
                    list();
                    throw Exit{1};
                }
                if (has("-geometry") && !std::regex_match(cli.at("-geometry"), std::regex("[0-9]+x[0-9]+")))
                    throw std::runtime_error(program + ": geometry " + cli.at("-geometry") + " is invalid");
                if (has("-geometry")) {
                    auto dims = split(cli.at("-geometry"), 'x');
                    if (number(dims[0]) < 1 || number(dims[1]) < 1)
                        throw std::runtime_error("Invalid geometry");
                }
                if (has("-fp"))
                    fontPath = cli.at("-fp");
                if (has("-xstartup"))
                    startup = cli.at("-xstartup");
                if (debug)
                    cli["-Log"] = "*:stderr:100";
                if (!pass.empty() && std::regex_match(pass[0], std::regex(":[0-9]+"))) {
                    display = number(pass[0].substr(1), 59535);
                    pass.erase(pass.begin());
                    if (!freeDisplay(display))
                        throw std::runtime_error("A VNC server is already running as :" + std::to_string(display));
                } else {
                    for (display = 1; display <= 99 && !freeDisplay(display); ++display) {
                    }
                    if (display == 100)
                        throw std::runtime_error(program + ": no free display number on " + host + ".");
                }
                if (!pass.empty() && !pass[0].starts_with('-') && !pass[0].starts_with('+'))
                    usage();
            }
            void list() {
                std::cout << "\nKasmVNC server sessions:\n\nX DISPLAY #\tPROCESS ID\n";
                if (!fs::is_directory(directory))
                    return;
                for (const auto &item: fs::directory_iterator(directory)) {
                    auto name = item.path().filename().string();
                    auto prefix = host + ":";
                    if (!name.starts_with(prefix) || !name.ends_with(".pid"))
                        continue;
                    auto n = name.substr(prefix.size(), name.size() - prefix.size() - 4);
                    if (n.empty() || n.find_first_not_of("0123456789") != std::string::npos)
                        continue;
                    auto pid = readPid(item.path());
                    if (alive(pid))
                        std::cout << ":" << n << "\t\t" << pid << "\n";
                    else
                        fs::remove(item.path());
                }
            }
            void stop(std::string target) {
                target = std::regex_replace(target, std::regex("(:[0-9]+)\\.[0-9]+$"), "$1");
                if (target.starts_with(host + ":"))
                    target = target.substr(host.size());
                if (!std::regex_match(target, std::regex(":[0-9]+")))
                    throw std::runtime_error(
                        "\nCan't tell if " + target + " is on " + host + "\nUse -kill :<number> instead\n");
                number(target.substr(1), 59535);
                auto file = directory / (host + target + ".pid");
                if (access(file.c_str(), R_OK) != 0) {
                    int status = errno;
                    std::cerr << "\nCan't find file " << file.string()
                              << "\nYou'll have to kill the Xvnc process manually\n\n";
                    throw Exit{status};
                }
                auto pid = readPid(file);
                std::cerr << "Killing Xvnc process ID " << pid << "\n";
                if (alive(pid)) {
                    // PID reuse must never let a stale per-user file kill an unrelated process.
                    auto cmdline = readFile("/proc/" + std::to_string(pid) + "/cmdline");
                    auto executable = fs::path(cmdline.substr(0, cmdline.find('\0'))).filename().string();
                    if (executable != "Xvnc" && executable != "Xkasmvnc")
                        throw std::runtime_error("PID file does not identify an Xvnc process");
                    if (kill(pid, SIGTERM) != 0 && errno != ESRCH)
                        throw std::runtime_error("Cannot signal Xvnc");
                    for (int i = 0; i < 20 && alive(pid); ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    if (alive(pid)) {
                        std::cout << "Xvnc seems to be deadlocked.  Kill the process manually and then re-run\n    "
                                  << program << " -kill " << target << "\nto clean up the socket files.\n";
                        return;
                    }
                } else {
                    std::cerr << "Xvnc process ID " << pid << " already killed\n";
                    for (auto path: {"/tmp/.X11-unix/X" + target.substr(1), "/tmp/.X" + target.substr(1) + "-lock"}) {
                        struct stat st {};
                        if (lstat(path.c_str(), &st) == 0 && st.st_uid == geteuid()) {
                            std::cout << "Xvnc did not appear to shut down cleanly. Removing " << path << "\n";
                            fs::remove(path);
                        }
                    }
                }
                fs::remove(file);
            }
            void xauthority() {
                std::array<unsigned char, 16> random{};
                std::size_t at = 0;
                while (at < random.size()) {
                    auto n = getrandom(random.data() + at, random.size() - at, 0);
                    if (n < 0 && errno == EINTR)
                        continue;
                    if (n <= 0)
                        throw std::runtime_error("Cannot obtain secure Xauthority cookie");
                    at += static_cast<std::size_t>(n);
                }
                constexpr char hex[] = "0123456789abcdef";
                std::string cookie;
                for (auto byte: random) {
                    cookie += hex[byte >> 4];
                    cookie += hex[byte & 15];
                }
                int pipefd[2];
                if (pipe2(pipefd, O_CLOEXEC) < 0)
                    throw std::runtime_error("Cannot create xauth pipe");
                Fd reader(pipefd[0]);
                auto child = spawn({"xauth", "-f", authority, "source", "-"}, reader.get());
                {
                    Fd writer(pipefd[1]);
                    writeAll(writer.get(),
                        "add " + host + ":" + std::to_string(display) + " . " + cookie + "\nadd " + host +
                            "/unix:" + std::to_string(display) + " . " + cookie + "\n");
                }
                if (waitChild(child) != 0)
                    throw std::runtime_error("xauth failed");
            }
            void loadUsers() {
                users.clear();
                std::ifstream input(passwd);
                std::string line;
                while (std::getline(input, line)) {
                    auto fields = split(line, ':');
                    if (!fields.empty())
                        users[fields[0]] = fields.size() > 2 ? fields[2] : "";
                }
            }
            void userSetup() {
                if (dry)
                    return;
                loadUsers();
                if (!prompting) {
                    if (users.empty()) {
                        std::cerr << "No users configured and prompting is prohitibed, exiting.\nUse vncpasswd(1) to "
                                     "add a user or enable command_line.prompt in config.\n";
                        throw Exit{1};
                    }
                    return;
                }
                for (const auto &[name, permissions]: users)
                    if (permissions.find('w') != std::string::npos)
                        return;
                std::vector<std::string> names;
                for (const auto &[name, permissions]: users)
                    names.push_back(name);
                int choice = 0;
                for (;;) {
                    std::cout << "\nIn order to control your desktop, you need a KasmVNC user with write\npermissions. "
                                 "Select what action to take:\n\n";
                    int i = 1;
                    for (const auto &name: names)
                        std::cout << "[" << i++ << "] Provide user '" << name << "' with write access\n";
                    std::cout << "[" << i << "] Create a new user with write access\n[" << (i + 1)
                              << "] Start KasmVNC without a user with write access\n\n";
                    auto text = prompt("Provide selection number: ");
                    try {
                        choice = number(text);
                        if (choice >= 1 && choice <= i + 1)
                            break;
                    } catch (const std::exception &) {
                    }
                    std::cout << "Invalid choice: '" << text << "'\n";
                }
                if (choice <= static_cast<int>(names.size())) {
                    const auto &name = names[choice - 1];
                    if (run({tool("kasmvncpasswd"), "-u", name, "-w" + users[name], "-n", passwd}) != 0)
                        throw std::runtime_error("Failed to update user permissions");
                    std::cout << "Added write permissions for user '" << name << "'\n";
                } else if (choice == static_cast<int>(names.size()) + 1) {
                    std::cout << "\nLet's create a user.\n\n";
                    std::string name;
                    for (;;) {
                        name = prompt(
                            users.contains(user) ? "Enter username: " : "Enter username (default: " + user + "): ");
                        if (name.empty() && !users.contains(user))
                            name = user;
                        if (name.empty())
                            continue;
                        if (!users.contains(name))
                            break;
                        std::cout << "User already exists: \"" << name << "\"\n";
                    }
                    if (run({tool("kasmvncpasswd"), "-u", name, "-w", passwd}) != 0)
                        throw std::runtime_error("Failed to setup user \"" + name + "\"");
                    std::cout << "Created user \"" << name << "\"\n";
                }
                loadUsers();
            }
            void desktopSetup() {
                bool named = has("-select-de") && cli.at("-select-de") != "1";
                if (!named) {
                    if (dry)
                        return;
                    if (!prompting) {
                        if (has("-select-de") || !fs::exists(directory / ".de-was-selected"))
                            std::cerr << "\nWarning: the Desktop Environment to run wasn't selected, but prompting to "
                                         "select\na Desktop Environment was disabled.\n";
                        return;
                    }
                    if (!has("-select-de") && fs::exists(directory / ".de-was-selected"))
                        return;
                    fs::remove(directory / ".de-was-selected");
                }
                std::vector<std::string> args{selectDe};
                if (has("-select-de")) {
                    args.push_back("--select-de");
                    if (named)
                        args.push_back(cli.at("-select-de"));
                }
                if (run(args) != 0)
                    throw std::runtime_error("Failed to execute " + selectDe);
            }
            void certificates() {
                if (dry)
                    return;
                std::set<std::string> checked;
                bool error = false;
                for (const auto &key: {"network.ssl.pem_certificate", "network.ssl.pem_key"}) {
                    if (!config.get(key).present())
                        continue;
                    auto path = value(key);
                    if (!checked.insert(path).second || access(path.c_str(), R_OK) == 0)
                        continue;
                    error = true;
                    if (!fs::is_regular_file(path))
                        std::cerr << path << ": certificate file doesn't exist or isn't a file\n";
                    else {
                        struct stat st {};
                        stat(path.c_str(), &st);
                        auto group = getgrgid(st.st_gid);
                        std::string name = group ? group->gr_name : std::to_string(st.st_gid);
                        std::cerr << path
                                  << ": certificate isn't readable.\nMake the certificate readable by adding your user "
                                     "to group \""
                                  << name << "\":\n  '"
                                  << (findCommand("rpm").empty() ? "addgroup $USER " + name
                                                                 : "usermod -a -G " + name + " $USER")
                                  << "'\n";
                    }
                }
                if (error)
                    throw Exit{1};
            }
            std::vector<std::string> command(Context &ctx) {
                std::vector<std::string> result{tool("Xvnc"), ":" + std::to_string(display)};
                std::set<std::string> supplied;
                for (const auto &arg: pass) {
                    result.push_back(arg);
                    if (arg.starts_with('-'))
                        supplied.insert(arg);
                }
                for (const auto &key: {"-fp", "-interface", "-websocketPort", "-Log"})
                    if (has(key) && !cli.at(key).empty() && cli.at(key) != "0") {
                        result.push_back(key);
                        result.push_back(cli.at(key));
                        supplied.insert(key);
                    }
                auto generated = arguments(config, ctx, supplied);
                result.insert(result.end(), generated.begin(), generated.end());
                for (const auto &[key, val]: std::map<std::string, std::string>{
                         {"-rfbauth", (directory / "passwd").string()},
                         {"-rfbport", "5901"                         },
                         {"-rfbwait", "30000"                        }
                })
                    if (!supplied.contains(key)) {
                        result.push_back(key);
                        result.push_back(val);
                    }
                return result;
            }
            pid_t start(const std::vector<std::string> &args) {
                Fd output(open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0666));
                if (output.get() < 0)
                    throw std::runtime_error("Cannot open desktop log");
                Fd nullInput(open("/dev/null", O_RDONLY | O_CLOEXEC));
                if (nullInput.get() < 0)
                    throw std::runtime_error("Cannot open /dev/null");
                auto child = spawn(args, nullInput.get(), output.get());
                try {
                    writeFile(pidFile, std::to_string(child) + "\n", 0666, false, false);
                } catch (...) {
                    kill(child, SIGTERM);
                    waitChild(child);
                    throw;
                }
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                do {
                    if (run({"xdpyinfo", "-display", ":" + std::to_string(display)}, true) == 0)
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                } while (std::chrono::steady_clock::now() < deadline);
                return child;
            }
            void apps() {
                if (has("-noxstartup"))
                    return;
                if (!fs::exists(startup)) {
                    std::cerr << "Creating default startup script " << startup << "\n";
                    writeFile(startup, defaultStartup, 0755, true);
                }
                std::cerr << "Starting applications specified in " << startup << "\n";
                auto displayEnv = (fs::exists("/tmp/.X11-unix/X" + std::to_string(display)) ||
                                      fs::exists("/usr/spool/sockets/X11/" + std::to_string(display)))
                                      ? ":" + std::to_string(display)
                                      : host + ":" + std::to_string(display);
                setenv("DISPLAY", displayEnv.c_str(), 1);
                setenv("VNCDESKTOP", desktop.c_str(), 1);
                // The script itself is intentionally interpreted, but its filename is an
                // argv entry, never concatenated into shell source.
                auto execute = [&]() {
                    Fd output(open(log.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW));
                    if (output.get() < 0)
                        throw std::runtime_error("Cannot open startup log");
                    writeAll(output.get(), "Running " + startup + "\n");
                    waitChild(spawn({"/bin/sh", "-c", "exec \"$1\"", "vncserver-xstartup", startup}, -1, output.get()));
                };
                if (has("-fg")) {
                    execute();
                    if (alive(readPid(pidFile)))
                        stop(":" + std::to_string(display));
                    throw Exit{0};
                } else {
                    auto child = fork();
                    if (child < 0)
                        throw std::runtime_error("Cannot fork startup supervisor");
                    if (child == 0) {
                        try {
                            Fd output(open(log.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW));
                            Fd input(open("/dev/null", O_RDONLY | O_CLOEXEC));
                            if (output.get() < 0 || input.get() < 0 || dup2(output.get(), 1) < 0 ||
                                dup2(output.get(), 2) < 0 || dup2(input.get(), 0) < 0)
                                _exit(1);
                            execute();
                            if (has("-autokill"))
                                stop(":" + std::to_string(display));
                            _exit(0);
                        } catch (...) {
                            _exit(1);
                        }
                    }
                }
            }
            void browserUrl() {
                std::string interface = has("-interface") ? cli.at("-interface") : value("network.interface");
                std::string port = has("-websocketPort") ? cli.at("-websocketPort") : value("network.websocket_port");
                if (port == "auto")
                    port = std::to_string(8443 + display);
                std::vector<std::string> hosts;
                if (interface == "0.0.0.0") {
                    addrinfo hints{};
                    hints.ai_family = AF_UNSPEC;
                    hints.ai_socktype = SOCK_STREAM;
                    addrinfo *answer = nullptr;
                    if (getaddrinfo(host.c_str(), nullptr, &hints, &answer) == 0) {
                        std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> owner(answer, freeaddrinfo);
                        for (auto item = answer; item; item = item->ai_next) {
                            std::array<char, NI_MAXHOST> name{};
                            if (getnameinfo(item->ai_addr,
                                    item->ai_addrlen,
                                    name.data(),
                                    name.size(),
                                    nullptr,
                                    0,
                                    NI_NUMERICHOST) == 0 &&
                                std::find(hosts.begin(), hosts.end(), name.data()) == hosts.end())
                                hosts.emplace_back(name.data());
                        }
                    }
                } else if (!interface.empty())
                    hosts.push_back(interface);
                std::cerr << "\nPaste this url in your browser:\n";
                for (const auto &name: hosts)
                    std::cerr << "https://" << name << ":" << port << "\n";
            }

        public:
            Session(int argc, char **argv) {
                program = fs::path(argv[0]).filename().string();
                home = env("HOME");
                if (!std::getenv("HOME"))
                    throw std::runtime_error(program + ": The HOME environment variable is not set.");
                directory = fs::path(home) / ".vnc";
                authority = std::getenv("XAUTHORITY") ? env("XAUTHORITY") : home + "/.Xauthority";
                startup = (directory / "xstartup").string();
                passwd = home + "/.kasmpasswd";
                utsname machine{};
                if (uname(&machine) != 0)
                    throw std::runtime_error("Cannot get hostname");
                host = machine.nodename;
                auto identity = getpwuid(geteuid());
                if (!identity)
                    throw std::runtime_error("Cannot resolve current user");
                user = identity->pw_name;
                std::string invoked = findCommand(argv[0]);
                if (invoked.empty())
                    invoked = argv[0];
                invoked = fs::absolute(invoked).lexically_normal().string();
                auto slash = invoked.rfind('/');
                bin = slash == std::string::npos ? "" : invoked.substr(0, slash + 1);
                if (bin.find("unix/") != std::string::npos)
                    bin = "/usr/bin/";
                selectDe = invoked.starts_with("/usr")
                               ? "/usr/lib/kasmvncserver/select-de.sh"
                               : (fs::path(invoked).parent_path() / "../builder/startup/deb/select-de.sh").string();
                if (invoked.find("vncserver-native/") != std::string::npos)
                    selectDe = (fs::path(invoked).parent_path() / "../../builder/startup/deb/select-de.sh").string();
                for (const auto &required: {"xauth"})
                    if (findCommand(required).empty())
                        throw std::runtime_error(program + ": couldn't find \"" + required + "\" on your PATH.");
                for (const auto &name: {"Xvnc", "vncpasswd"})
                    if (findCommand(tool(name)).empty())
                        throw std::runtime_error(program + ": couldn't find \"" + tool(name) + "\".");
                struct stat existingDirectory {};
                if (lstat(directory.c_str(), &existingDirectory) == 0 &&
                    (!S_ISDIR(existingDirectory.st_mode) || existingDirectory.st_uid != geteuid()))
                    throw std::runtime_error("Unsafe session directory");
                fonts();
                parse(argc, argv);
            }
            void launch() {
                if (!fs::exists(directory)) {
                    if (mkdir(directory.c_str(), 0755) != 0)
                        throw std::runtime_error("Could not create " + directory.string());
                }
                struct stat dirStatus {};
                if (lstat(directory.c_str(), &dirStatus) < 0 || !S_ISDIR(dirStatus.st_mode) ||
                    dirStatus.st_uid != geteuid())
                    throw std::runtime_error("Unsafe session directory");
                log = (directory / (host + ":" + std::to_string(display) + ".log")).string();
                pidFile = (directory / (host + ":" + std::to_string(display) + ".pid")).string();
                auto userConfig = (directory / "kasmvnc.yaml").string();
                if (!fs::exists(userConfig)) {
                    std::cerr << "Creating default config " << userConfig << "\n";
                    writeFile(
                        userConfig, "logging:\n  log_writer_name: all\n  log_dest: logfile\n  level: 30\n", 0644, true);
                }
                std::vector<std::string> files;
                if (has("-config"))
                    files = split(cli.at("-config"), ',');
                else
                    files = env("KASMVNC_DEVELOPMENT").empty()
                                ? std::vector<std::string>{"/usr/share/kasmvnc/kasmvnc_defaults.yaml",
                                      "/etc/kasmvnc/kasmvnc.yaml",
                                      userConfig}
                                : std::vector<std::string>{
                                      "/src/unix/kasmvnc_defaults.yaml", "/src/unix/kasmvnc.yaml", userConfig};
                if (debug)
                    for (const auto &file: files)
                        if (file != userConfig || access(file.c_str(), R_OK) == 0)
                            std::cerr << "Loading config " << file << "\n";
                config.load(files, userConfig);
                config.environment(debug, topic("validation"));
                config.validate(topic("validation"));
                auto promptValue = has("-prompt") ? cli.at("-prompt") : boolean(value("command_line.prompt"));
                prompting = !promptValue.empty() && promptValue != "0";
                // Preserve the historical disabled VNC password bytes without base64(1).
                writeFile(directory / "passwd", std::string("\x5a\xb2\xcd\xc0\xba\xdc\xaf\x13", 8), 0666, false, false);
                xauthority();
                userSetup();
                desktopSetup();
                Context context{has("-geometry") ? cli.at("-geometry") : "1024x768",
                    fontPath,
                    authority,
                    host + ":" + std::to_string(display) + " (" + user + ")",
                    display};
                auto args = command(context);
                desktop = context.desktop;
                certificates();
                if ((dry || debug) && topic("xvnc-cmd"))
                    std::cout << render(args, log, pass.size()) << "\n";
                if (dry)
                    return;
                if (!has("-interface") && value("network.interface").empty())
                    throw std::runtime_error("-interface has no default value and wasn't passed by user");
                fs::remove(log);
                auto child = start(args);
                if (!alive(child) && fontPath != safeFontPath) {
                    if (has("-fp"))
                        std::cerr << "\nWARNING: The first attempt to start Xvnc failed, probably because the font\n"
                                     "path you specified using the -fp argument is incorrect.  Attempting to\n"
                                     "determine an appropriate font path for this system and restart Xvnc using\n"
                                     "that font path ...\n";
                    else
                        std::cerr << "\nWARNING: The first attempt to start Xvnc failed, possibly because the font\n"
                                     "catalog is not properly configured.  Attempting to determine an appropriate\n"
                                     "font path for this system and restart Xvnc using that font path ...\n";
                    for (std::size_t i = 2; i < args.size(); ++i)
                        if (args[i] == "-fp") {
                            args.erase(args.begin() + i, args.begin() + std::min(i + 2, args.size()));
                            break;
                        }
                    if (!safeFontPath.empty()) {
                        args.push_back("-fp");
                        args.push_back(safeFontPath);
                    }
                    child = start(args);
                }
                if (!alive(child)) {
                    std::cerr << "Could not start Xvnc.\n\n";
                    fs::remove(pidFile);
                    std::cout << readFile(log);
                    std::cerr << "\n";
                    throw Exit{255};
                }
                std::cerr << "\nNew '" << desktop << "' desktop is " << host << ":" << display
                          << "\n\nUsers configured:\n";
                for (const auto &[name, p]: users) {
                    bool w = p.find('w') != std::string::npos, o = p.find('o') != std::string::npos;
                    std::cerr << name << " ("
                              << (w ? (o ? "can use keyboard and mouse, add/remove users"
                                         : "can use keyboard and mouse")
                                    : (o ? "can add/remove users" : "can only view"))
                              << ")\n";
                }
                std::cerr << "\nLog file is " << log << "\n\n";
                apps();
                browserUrl();
            }
        };
    } // namespace
} // namespace launcher
int main(int argc, char **argv) {
    // Writes to a failed helper pipe must be reported, not terminate the parent.
    std::signal(SIGPIPE, SIG_IGN);
    try {
        launcher::Session session(argc, argv);
        session.launch();
        return 0;
    } catch (const launcher::Exit &exit) {
        return exit.status;
    } catch (const std::exception &error) {
        std::cerr << error.what() << "\n";
        return 255;
    }
}
