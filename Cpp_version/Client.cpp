#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fstream>

// 全局变量
std::atomic<bool> stop_welcome(false);
std::atomic<bool> stop_heartbeat(false);
std::atomic<bool> welcoming_port_ready(false);
int welcoming_port_number = 0;
std::vector<std::thread> download_threads;
std::vector<std::thread> upload_threads;

// 下载序列
void downloading_sequence(int peer_port_number, const std::string& download_filename, const std::string& peername) {
    int download_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (download_socket < 0) {
        std::cerr << "Failed to create download socket" << std::endl;
        return;
    }

    sockaddr_in peer_address{};
    peer_address.sin_family = AF_INET;
    peer_address.sin_port = htons(peer_port_number);
    inet_pton(AF_INET, "127.0.0.1", &peer_address.sin_addr);

    std::cout << "Connecting to " << peername << " at port " << peer_port_number << std::endl;
    if (connect(download_socket, (struct sockaddr*)&peer_address, sizeof(peer_address)) < 0) {
        std::cerr << "Connection failed" << std::endl;
        close(download_socket);
        return;
    }
    std::cout << "Connection established!" << std::endl;

    std::cout << "Telling peer which file is wanted: '" << download_filename << "'..." << std::endl;
    send(download_socket, download_filename.c_str(), download_filename.size(), 0);

    std::ofstream file(download_filename, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Failed to open file for writing" << std::endl;
        close(download_socket);
        return;
    }

    std::cout << "Downloading '" << download_filename << "' from " << peername << "..." << std::endl;
    char buffer[1024];
    while (true) {
        int bytes_received = recv(download_socket, buffer, sizeof(buffer), 0);
        if (bytes_received <= 0) break;
        file.write(buffer, bytes_received);
    }

    std::cout << "'" << download_filename << "' downloaded successfully!" << std::endl;
    std::cout << "Closing P2P connection with " << peername << "..." << std::endl;
    close(download_socket);
}

// 上传序列
void uploading_sequence(int upload_socket, const sockaddr_in& peer_address) {
    char buffer[1024];
    int bytes_received = recv(upload_socket, buffer, sizeof(buffer), 0);
    if (bytes_received <= 0) {
        close(upload_socket);
        return;
    }

    std::string requested_filename(buffer, bytes_received);
    std::cout << "Peer wants " << requested_filename << std::endl;

    std::ifstream file(requested_filename, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Failed to open file for reading" << std::endl;
        close(upload_socket);
        return;
    }

    std::cout << "Sending data..." << std::endl;
    while (file.read(buffer, sizeof(buffer))) {
        send(upload_socket, buffer, file.gcount(), 0);
    }

    std::cout << "'" << requested_filename << "' uploaded successfully!" << std::endl;
    std::cout << "Closing P2P connection with peer..." << std::endl;
    close(upload_socket);
}

// 创建欢迎套接字
void create_welcoming_socket() {
    int welcoming_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (welcoming_socket < 0) {
        std::cerr << "Failed to create welcoming socket" << std::endl;
        return;
    }

    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = 0; // Let OS choose a port

    if (bind(welcoming_socket, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
        std::cerr << "Bind failed" << std::endl;
        close(welcoming_socket);
        return;
    }

    socklen_t addr_len = sizeof(server_address);
    getsockname(welcoming_socket, (struct sockaddr*)&server_address, &addr_len);
    welcoming_port_number = ntohs(server_address.sin_port);
    welcoming_port_ready = true;

    std::cout << "Listening for P2P TCP connection request from port: " << welcoming_port_number << std::endl;
    listen(welcoming_socket, 5);

    while (!stop_welcome) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int upload_socket = accept(welcoming_socket, (struct sockaddr*)&client_addr, &client_len);
        if (upload_socket < 0) {
            std::cerr << "Accept failed" << std::endl;
            continue;
        }

        if (!stop_welcome) {
            std::cout << "New P2P connection from " << inet_ntoa(client_addr.sin_addr) << ":" << ntohs(client_addr.sin_port) << std::endl;
            upload_threads.emplace_back(uploading_sequence, upload_socket, client_addr);
        } else {
            std::cout << "STOP_WELCOME_FLAG is set, this is a pseudo request" << std::endl;
            close(upload_socket);
        }
    }

    std::cout << "Closing welcoming socket..." << std::endl;
    close(welcoming_socket);
}

// 心跳函数
void send_heartbeat(int client_socket, const std::string& username, const sockaddr_in& server_address) {
    while (!welcoming_port_ready) std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::string heartbeat_msg = "HBT " + username + " " + std::to_string(welcoming_port_number);
    while (!stop_heartbeat) {
        sendto(client_socket, heartbeat_msg.c_str(), heartbeat_msg.size(), 0, (struct sockaddr*)&server_address, sizeof(server_address));
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

// 主函数
int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <server_port>" << std::endl;
        return 1;
    }

    int server_port = std::stoi(argv[1]);
    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(server_port);
    inet_pton(AF_INET, "127.0.0.1", &server_address.sin_addr);

    int client_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (client_socket < 0) {
        std::cerr << "Failed to create client socket" << std::endl;
        return 1;
    }

    // 认证
    std::string username, password;
    while (true) {
        std::cout << "Enter username: ";
        std::cin >> username;
        std::cout << "Enter password: ";
        std::cin >> password;
        std::string auth_msg = "AUTH " + username + " " + password;
        sendto(client_socket, auth_msg.c_str(), auth_msg.size(), 0, (struct sockaddr*)&server_address, sizeof(server_address));
        std::cout << "AUTH Request Sent" << std::endl;
        char buffer[1024];
        socklen_t addr_len = sizeof(server_address);
        int bytes_received = recvfrom(client_socket, buffer, sizeof(buffer), 0, (struct sockaddr*)&server_address, &addr_len);
        std::cout << "AUTH Response Received" << std::endl;
        if (bytes_received <= 0) {
            std::cerr << "Failed to receive server response" << std::endl;
            continue;
        }

        std::string response(buffer, bytes_received);
        if (response.substr(0, 2) == "OK") {
            std::cout << "Welcome to BitTrickle!" << std::endl;
            break;
        } else {
            std::cout << "Authentication failed. Please try again." << std::endl;
        }
    }

    // 启动欢迎套接字线程
    std::thread welcome_thread(create_welcoming_socket);

    // 启动心跳线程
    std::thread heartbeat_thread(send_heartbeat, client_socket, username, server_address);

    std::cout << "Client is now online" << std::endl;

    // 处理用户命令
    std::string command;
    while (true) {
        std::cout << "> ";
        std::getline(std::cin, command);

        if (command == "xit") {
            std::cout << "Starting client shutdown sequence..." << std::endl;
            break;
        }

        // 其他命令处理逻辑
        // ...
    }

    // 关闭线程
    stop_welcome = true;
    stop_heartbeat = true;

    // 唤醒欢迎套接字的 accept 调用
    int temp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (temp_socket >= 0) {
        sockaddr_in temp_addr{};
        temp_addr.sin_family = AF_INET;
        temp_addr.sin_port = htons(welcoming_port_number);
        inet_pton(AF_INET, "127.0.0.1", &temp_addr.sin_addr);
        connect(temp_socket, (struct sockaddr*)&temp_addr, sizeof(temp_addr));
        close(temp_socket);
    }

    welcome_thread.join();
    heartbeat_thread.join();

    // 关闭套接字
    close(client_socket);

    // 等待所有 P2P 线程完成
    for (auto& thread : upload_threads) thread.join();
    for (auto& thread : download_threads) thread.join();

    std::cout << "Client is now offline" << std::endl;
    return 0;
}