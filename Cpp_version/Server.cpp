#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <ctime>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <chrono>
#include <iomanip>

#define BUFFER_SIZE 1024
#define HEARTBEAT_TIMEOUT 3

// 服务器类
class Server {
private:
    int port;
    int sockfd;
    std::map<std::string, std::string> credentials; // 用户名 -> 密码
    std::map<std::string, time_t> heartbeats;       // 用户名 -> 上次心跳时间
    std::set<std::string> active_clients;           // 活跃用户
    std::map<std::string, std::set<std::string>> published_files; // 文件名 -> 发布用户集合
    std::map<std::string, std::set<std::string>> user_files;      // 用户名 -> 发布文件集合
    std::map<std::string, int> contact_book;        // 用户名 -> 端口号

public:
    Server(int port) : port(port) {}

    // 加载用户凭证
    void load_credentials(const std::string &filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            std::cerr << "Failed to open credentials file" << std::endl;
            exit(EXIT_FAILURE);
        }

        std::string username, password;
        while (file >> username >> password) {
            credentials[username] = password;
        }
        file.close();
    }

    // 检查用户是否活跃
    bool is_active(const std::string &username) {
        if (heartbeats.find(username) == heartbeats.end()) return false;

        time_t current_time = time(nullptr);
        if (current_time - heartbeats[username] > HEARTBEAT_TIMEOUT) {
            active_clients.erase(username);
            return false;
        }
        return true;
    }

    // 检查用户凭证
    bool check_credentials(const std::string &username, const std::string &password) {
        return credentials.find(username) != credentials.end() && credentials[username] == password;
    }

    // 处理认证请求
    void handle_auth(const std::string &username, const std::string &password, const struct sockaddr_in &client_addr) {
        std::string response = "ERR";
        if (!is_active(username) && check_credentials(username, password)) {
            active_clients.insert(username);
            heartbeats[username] = time(nullptr);
            contact_book[username] = ntohs(client_addr.sin_port);
            response = "OK";
        }
        send_response(response, client_addr);
    }

    // 处理心跳请求
    void handle_hbt(const std::string &username, const struct sockaddr_in &client_addr) {
        heartbeats[username] = time(nullptr);
        contact_book[username] = ntohs(client_addr.sin_port);
    }

    // 处理列出活跃用户请求
    void handle_lap(const struct sockaddr_in &client_addr) {
        std::string response = "OK " + std::to_string(active_clients.size());
        for (const auto &client : active_clients) {
            response += " " + client;
        }
        send_response(response, client_addr);
    }

    // 处理发布文件请求
    void handle_pub(const std::string &username, const std::string &filename, const struct sockaddr_in &client_addr) {
        published_files[filename].insert(username);
        user_files[username].insert(filename);
        send_response("OK", client_addr);
    }

    // 处理取消发布文件请求
    void handle_unp(const std::string &username, const std::string &filename, const struct sockaddr_in &client_addr) {
        if (user_files[username].find(filename) != user_files[username].end()) {
            user_files[username].erase(filename);
            published_files[filename].erase(username);
            send_response("OK", client_addr);
        } else {
            send_response("ERR", client_addr);
        }
    }

    // 处理搜索文件请求
    void handle_sch(const std::string &username, const std::string &substring, const struct sockaddr_in &client_addr) {
        (void)username; // 显式忽略未使用的参数

        std::set<std::string> files_found;
        for (const auto &[file, users] : published_files) {
            if (file.find(substring) != std::string::npos) {
                files_found.insert(file);
            }
        }
        std::string response = "OK " + std::to_string(files_found.size());
        for (const auto &file : files_found) {
            response += " " + file;
        }
        send_response(response, client_addr);
    }

    // 处理获取文件请求
    void handle_get(const std::string &username, const std::string &filename, const struct sockaddr_in &client_addr) {
        std::string response = "ERR";
        if (published_files.find(filename) != published_files.end() && !published_files[filename].empty()) {
            for (const auto &user : published_files[filename]) {
                if (user != username && active_clients.find(user) != active_clients.end()) {
                    response = "OK " + user + " " + std::to_string(contact_book[user]);
                    break;
                }
            }
        }
        send_response(response, client_addr);
    }

    // 发送响应
    void send_response(const std::string &response, const struct sockaddr_in &client_addr) {
        socklen_t client_len = sizeof(client_addr);
        if (sendto(sockfd, response.c_str(), response.size(), 0, (struct sockaddr *)&client_addr, client_len) < 0) {
            std::cerr << "Error sending response" << std::endl;
        } else {
            // 获取当前时间
            auto now = std::chrono::system_clock::now();
            std::time_t now_time = std::chrono::system_clock::to_time_t(now);
            std::tm now_tm = *std::localtime(&now_time);

            // 打印时间戳和发送的内容
            std::cout << "[" << std::put_time(&now_tm, "%Y-%m-%d %H:%M:%S") << "] ";
            std::cout << "Sent response: " << response << std::endl;
        }
    }
    
    // 启动服务器
    void start() {
        // 创建UDP套接字
        sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0) {
            std::cerr << "Socket creation failed" << std::endl;
            exit(EXIT_FAILURE);
        }

        // 绑定地址
        struct sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(port);

        if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
            std::cerr << "Bind failed" << std::endl;
            close(sockfd);
            exit(EXIT_FAILURE);
        }

        std::cout << "Server is now online on port " << port << std::endl;

        while (true) {
            // 接收客户端请求
            char buffer[BUFFER_SIZE];
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int n = recvfrom(sockfd, buffer, BUFFER_SIZE, 0, (struct sockaddr *)&client_addr, &client_len);
            if (n < 0) {
                std::cerr << "Error receiving data" << std::endl;
                continue;
            }
            buffer[n] = '\0';

            // 获取当前时间
            auto now = std::chrono::system_clock::now();
            std::time_t now_time = std::chrono::system_clock::to_time_t(now);
            std::tm now_tm = *std::localtime(&now_time);

            // 打印时间戳和接收到的数据
            std::cout << "[" << std::put_time(&now_tm, "%Y-%m-%d %H:%M:%S") << "] ";
            std::cout << "Received data: " << buffer << std::endl;

            // 解析请求
            std::string request(buffer);
            std::vector<std::string> tokens;
            std::istringstream iss(request); // 将字符串包装为 istringstream
            std::string token;

            // 按空格分割字符串
            while (iss >> token) {
                tokens.push_back(token);
            }

            // 检查字段数量
            if (tokens.size() < 2) continue;

            // 解析请求字段
            std::string request_type = tokens[0];
            std::string username = tokens[1];

            // 处理请求
            if (request_type == "AUTH") {
                handle_auth(username, tokens[2], client_addr);
            } else if (request_type == "HBT") {
                handle_hbt(username, client_addr);
            } else if (request_type == "LAP") {
                handle_lap(client_addr);
            } else if (request_type == "PUB") {
                handle_pub(username, tokens[2], client_addr);
            } else if (request_type == "UNP") {
                handle_unp(username, tokens[2], client_addr);
            } else if (request_type == "SCH") {
                handle_sch(username, tokens[2], client_addr);
            } else if (request_type == "GET") {
                handle_get(username, tokens[2], client_addr);
            }
        }

        // 通常不会执行到这里
        std::cout << "Shutting down server..." << std::endl;
        close(sockfd);
    }
};

int main(int argc, char *argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <port>" << std::endl;
        return EXIT_FAILURE;
    }

    int port = std::stoi(argv[1]);
    Server server(port);
    server.load_credentials("credentials.txt");
    server.start();

    return 0;
}