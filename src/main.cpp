#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ----------------------- Config / defaults -----------------------

static const char* DEFAULT_HOST = "0.0.0.0";
static const int   DEFAULT_PORT = 11111;
static const char* DEFAULT_CONFIG_PATH = "/config/config.json";
static const char* FALLBACK_CONFIG_PATH = "config/default_config.json";

static std::atomic<bool> RUNNING{true};

// ----------------------- Process interface -----------------------

struct Process {
    virtual ~Process() = default;
    virtual json inputs() const = 0;
    virtual json outputs() const = 0;
    virtual json update(const json& state, double interval) = 0;
};

// ----------------------- Example process -------------------------
// CounterProcess: counter(t+dt) = counter(t) + rate * dt

class CounterProcess : public Process {
public:
    explicit CounterProcess(double rate = 1.0) : rate_(rate) {}

    json inputs() const override {
        return json{
            {"counter", {{"_type", "number"}}}
        };
    }

    json outputs() const override {
        return json{
            {"counter", {{"_type", "number"}, {"_apply", "set"}}}
        };
    }

    json update(const json& state, double interval) override {
        double current = 0.0;
        if (state.contains("counter")) {
            try {
                current = state.at("counter").get<double>();
            } catch (...) {
                current = 0.0;
            }
        }
        double newval = current + rate_ * interval;
        return json{{"counter", newval}};
    }

private:
    double rate_;
};

// ----------------------- Config helpers --------------------------

json read_json_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return json::object();
    json j;
    try {
        f >> j;
    } catch (...) {
        return json::object();
    }
    return j;
}

json read_config() {
    const char* env_path = std::getenv("CONFIG_PATH");
    std::string primary = env_path ? std::string(env_path) : std::string(DEFAULT_CONFIG_PATH);

    // Try primary, else fallback
    std::ifstream f1(primary);
    if (f1.good()) {
        return read_json_file(primary);
    }
    return read_json_file(FALLBACK_CONFIG_PATH);
}

std::unique_ptr<Process> build_process_from_config(const json& cfg) {
    std::string pname = "counter";
    if (cfg.contains("process")) {
        try { pname = cfg.at("process").get<std::string>(); } catch (...) {}
    }

    if (pname == "counter") {
        double rate = 1.0;
        if (cfg.contains("rate")) {
            try { rate = cfg.at("rate").get<double>(); } catch (...) {}
        }
        return std::make_unique<CounterProcess>(rate);
    }

    // default
    return std::make_unique<CounterProcess>();
}

// ----------------------- Networking utils ------------------------

int create_server_socket(const std::string& host, int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        ::close(fd);
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        perror("inet_pton");
        ::close(fd);
        return -1;
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        ::close(fd);
        return -1;
    }

    if (::listen(fd, 16) < 0) {
        perror("listen");
        ::close(fd);
        return -1;
    }

    return fd;
}

std::optional<std::string> recv_line(int client_fd) {
    std::string line;
    char c;
    while (true) {
        ssize_t n = ::recv(client_fd, &c, 1, 0);
        if (n == 0) {
            // peer closed
            if (line.empty()) return std::nullopt;
            break;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recv");
            return std::nullopt;
        }
        if (c == '\n') break;
        line.push_back(c);
    }
    return line;
}

bool send_line(int client_fd, const std::string& s) {
    std::string out = s;
    out.push_back('\n');
    const char* buf = out.c_str();
    size_t total = 0;
    size_t len = out.size();
    while (total < len) {
        ssize_t n = ::send(client_fd, buf + total, len - total, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("send");
            return false;
        }
        total += static_cast<size_t>(n);
    }
    return true;
}

// ----------------------- Command router --------------------------

json run_command(const json& cmd, Process& process) {
    if (!cmd.contains("command")) {
        return json{{"error", "missing 'command' field"}};
    }
    std::string cname;
    try {
        cname = cmd.at("command").get<std::string>();
    } catch (...) {
        return json{{"error", "invalid 'command' field"}};
    }

    if (cname == "inputs") {
        return process.inputs();
    } else if (cname == "outputs") {
        return process.outputs();
    } else if (cname == "update") {
        json args = json::object();
        if (cmd.contains("arguments")) {
            try { args = cmd.at("arguments"); } catch (...) {}
        }
        json state = args.value("state", json::object());
        double interval = 0.0;
        try {
            interval = args.at("interval").get<double>();
        } catch (...) {
            interval = 0.0;
        }
        return process.update(state, interval);
    } else {
        return json{{"error", std::string("unknown command: ") + cname}};
    }
}

void handle_client(int client_fd, std::unique_ptr<Process> process) {
    while (RUNNING.load()) {
        auto maybe_line = recv_line(client_fd);
        if (!maybe_line.has_value()) break;
        std::string line = *maybe_line;

        // ignore empty lines
        if (line.find_first_not_of(" \t\r") == std::string::npos) {
            continue;
        }

        json cmd;
        try {
            cmd = json::parse(line);
        } catch (...) {
            send_line(client_fd, json{{"error", "invalid json"}}.dump());
            continue;
        }

        json result = run_command(cmd, *process);
        send_line(client_fd, result.dump());
    }
    ::close(client_fd);
}

void sigint_handler(int) {
    RUNNING.store(false);
}

// ----------------------- main ------------------------------------

int main(int argc, char** argv) {
    std::signal(SIGINT, sigint_handler);
    std::signal(SIGTERM, sigint_handler);

    // env overrides
    const char* port_env = std::getenv("PORT");
    int port = port_env ? std::atoi(port_env) : DEFAULT_PORT;

    const char* host_env = std::getenv("HOST");
    std::string host = host_env ? std::string(host_env) : std::string(DEFAULT_HOST);

    // load config & build process
    json cfg = read_config();
    auto process = build_process_from_config(cfg);

    int server_fd = create_server_socket(host, port);
    if (server_fd < 0) {
        std::cerr << "Failed to start server\n";
        return 1;
    }
    std::cout << "process is listening on " << host << ":" << port << std::endl;

    std::vector<std::thread> workers;
    while (RUNNING.load()) {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);
        int client_fd = ::accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        // Create a fresh process instance per connection if desired,
        // or share one across connections. Here, we clone by rebuilding.
        auto per_conn_proc = build_process_from_config(cfg);

        workers.emplace_back(std::thread([client_fd, p = std::move(per_conn_proc)]() mutable {
            handle_client(client_fd, std::move(p));
        }));
        workers.back().detach(); // detach to avoid joining on shutdown
    }

    ::close(server_fd);
    return 0;
}
