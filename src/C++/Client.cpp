#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

// Global variables
std::atomic<bool> stop_welcome(false);
std::atomic<bool> stop_heartbeat(false);
std::atomic<bool> welcoming_port_ready(false);
int welcoming_port_number = 0;

std::mutex upload_mutex;
std::vector<std::thread> upload_threads;

std::mutex download_mutex;
std::vector<std::thread> download_threads;

std::atomic<bool> server_shutdown(false);

// ==================== P2P Download ====================
void downloading_sequence(int peer_port_number,
                          const std::string &download_filename,
                          const std::string &peername) {
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		std::cerr << "Failed to create download socket\n";
		return;
	}

	sockaddr_in peer_addr{};
	peer_addr.sin_family = AF_INET;
	peer_addr.sin_port = htons(peer_port_number);
	inet_pton(AF_INET, "127.0.0.1", &peer_addr.sin_addr);

	std::cout << "Connecting to " << peername << " at port " << peer_port_number
	          << "...\n";
	if (connect(sock, (struct sockaddr *)&peer_addr, sizeof(peer_addr)) < 0) {
		std::cerr << "Connection failed\n";
		close(sock);
		return;
	}
	std::cout << "Connection established!\n";

	send(sock, download_filename.c_str(), download_filename.size(), 0);

	std::ofstream file(download_filename, std::ios::binary);
	if (!file.is_open()) {
		std::cerr << "Failed to open file for writing\n";
		close(sock);
		return;
	}

	char buffer[4096];
	while (true) {
		ssize_t bytes = recv(sock, buffer, sizeof(buffer), 0);
		if (bytes <= 0)
			break;
		file.write(buffer, bytes);
	}
	std::cout << "'" << download_filename << "' downloaded successfully!\n";
	close(sock);
}

// ==================== P2P Upload ====================
void uploading_sequence(int upload_socket, sockaddr_in peer_addr) {
	char buffer[4096];
	ssize_t bytes = recv(upload_socket, buffer, sizeof(buffer), 0);
	if (bytes <= 0) {
		close(upload_socket);
		return;
	}

	std::string requested_file(buffer, bytes);
	std::ifstream file(requested_file, std::ios::binary);
	if (!file.is_open()) {
		std::cerr << "Failed to open file: " << requested_file << "\n";
		close(upload_socket);
		return;
	}

	std::cout << "Sending '" << requested_file << "' to peer...\n";
	while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
		ssize_t sent = send(upload_socket, buffer, file.gcount(), 0);
		if (sent < 0) {
			std::cerr << "Send error!\n";
			break;
		}
	}
	std::cout << "'" << requested_file << "' uploaded successfully!\n";
	close(upload_socket);
}

// ==================== Welcoming Socket ====================
void create_welcoming_socket() {
	int welcoming_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (welcoming_sock < 0) {
		std::cerr << "Failed to create welcoming socket\n";
		return;
	}

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = 0;

	if (bind(welcoming_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		std::cerr << "Bind failed\n";
		close(welcoming_sock);
		return;
	}

	socklen_t addr_len = sizeof(addr);
	getsockname(welcoming_sock, (struct sockaddr *)&addr, &addr_len);
	welcoming_port_number = ntohs(addr.sin_port);
	welcoming_port_ready = true;

	std::cout << "Listening for P2P TCP connections on port "
	          << welcoming_port_number << "\n";
	listen(welcoming_sock, 5);

	while (!stop_welcome) {
		sockaddr_in client_addr{};
		socklen_t client_len = sizeof(client_addr);
		int upload_sock = accept(welcoming_sock,
		                         (struct sockaddr *)&client_addr, &client_len);
		if (upload_sock < 0) {
			if (stop_welcome)
				break;
			continue;
		}

		std::lock_guard<std::mutex> lock(upload_mutex);
		upload_threads.emplace_back(uploading_sequence, upload_sock,
		                            client_addr);
	}
	close(welcoming_sock);
}

// ==================== Heartbeat ====================
void send_heartbeat(int client_socket, const std::string &username,
                    const sockaddr_in &server_addr) {
	while (!welcoming_port_ready)
		std::this_thread::sleep_for(std::chrono::milliseconds(100));

	std::string heartbeat_msg =
	    "HBT " + username + " " + std::to_string(welcoming_port_number);
	while (!stop_heartbeat && !server_shutdown) {
		sendto(client_socket, heartbeat_msg.c_str(), heartbeat_msg.size(), 0,
		       (struct sockaddr *)&server_addr, sizeof(server_addr));
		std::this_thread::sleep_for(std::chrono::seconds(2));
	}
}

// ==================== UDP send/receive ====================
std::optional<std::vector<std::string>>
send_and_receive(int client_socket, const sockaddr_in &server_addr,
                 const std::string &msg, int timeout_sec = 3) {
	sendto(client_socket, msg.c_str(), msg.size(), 0,
	       (struct sockaddr *)&server_addr, sizeof(server_addr));

	timeval tv{timeout_sec, 0};
	setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	char buffer[4096];
	socklen_t addr_len = sizeof(server_addr);
	ssize_t bytes = recvfrom(client_socket, buffer, sizeof(buffer), 0,
	                         (struct sockaddr *)&server_addr, &addr_len);
	if (bytes <= 0) {
		std::cerr << "No response from server (timeout)\n";
		return std::nullopt;
	}

	std::string resp(buffer, bytes);
	if (resp == "SRV_SHUTDOWN") {
		std::cout << "Server is shutting down. Exiting client...\n";
		stop_welcome = true;
		stop_heartbeat = true;
		server_shutdown = true;
		return std::nullopt;
	}

	std::vector<std::string> tokens;
	std::istringstream iss(resp);
	for (std::string s; iss >> s;)
		tokens.push_back(s);
	return tokens;
}

// ==================== Command Handlers ====================
void handle_lap(int sock, const sockaddr_in &srv, const std::string &user) {
	auto resp = send_and_receive(sock, srv, "LAP " + user);
	if (!resp)
		return;
	std::cout << resp->size() - 1 << " active peers:\n";
	for (size_t i = 1; i < resp->size(); ++i)
		std::cout << " - " << (*resp)[i] << "\n";
}

void handle_lpf(int sock, const sockaddr_in &srv, const std::string &user) {
	auto resp = send_and_receive(sock, srv, "LPF " + user);
	if (!resp)
		return;

	if (resp->size() > 0 && (*resp)[0] == "ERR") {
		std::cout << "Server error: ";
		for (size_t i = 1; i < resp->size(); ++i)
			std::cout << (*resp)[i] << " ";
		std::cout << "\n";
		return;
	}

	if (resp->size() <= 1) {
		std::cout << "No files published\n";
		return;
	}

	std::cout << resp->size() - 1 << " files published:\n";
	for (size_t i = 1; i < resp->size(); ++i)
		std::cout << " - " << (*resp)[i] << "\n";
}

void handle_pub(int sock, const sockaddr_in &srv, const std::string &user,
                const std::string &filename) {
	auto resp = send_and_receive(sock, srv, "PUB " + user + " " + filename);
	if (!resp)
		return;
	std::cout << ((*resp)[0] == "OK" ? "File published: " :
	                                   "Failed to publish: ")
	          << filename << "\n";
}

void handle_unp(int sock, const sockaddr_in &srv, const std::string &user,
                const std::string &filename) {
	auto resp = send_and_receive(sock, srv, "UNP " + user + " " + filename);
	if (!resp)
		return;
	std::cout << ((*resp)[0] == "OK" ? "File unpublished: " :
	                                   "Failed to unpublish: ")
	          << filename << "\n";
}

void handle_sch(int sock, const sockaddr_in &srv, const std::string &user,
                const std::string &substr) {
	auto resp = send_and_receive(sock, srv, "SCH " + user + " " + substr);
	if (!resp || resp->size() <= 1) {
		std::cout << "No files found\n";
		return;
	}
	std::cout << resp->size() - 1 << " files found:\n";
	for (size_t i = 1; i < resp->size(); ++i)
		std::cout << " - " << (*resp)[i] << "\n";
}

void handle_get(int sock, const sockaddr_in &srv, const std::string &user,
                const std::string &filename) {
	auto resp = send_and_receive(sock, srv, "GET " + user + " " + filename);
	if (!resp || (*resp)[0] != "OK") {
		std::cout << "File not available\n";
		return;
	}
	std::string peername = (*resp)[1];
	int peer_port = std::stoi((*resp)[2]);
	std::lock_guard<std::mutex> lock(download_mutex);
	download_threads.emplace_back(downloading_sequence, peer_port, filename,
	                              peername);
}

// ==================== Main ====================
int main(int argc, char *argv[]) {
	if (argc != 2) {
		std::cerr << "Usage: " << argv[0] << " <server_port>\n";
		return 1;
	}

	int server_port = std::stoi(argv[1]);
	sockaddr_in server_addr{};
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(server_port);
	inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

	int client_sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (client_sock < 0) {
		std::cerr << "Failed to create socket\n";
		return 1;
	}

	std::string username, password;
	while (true) {
		std::cout << "Enter username: ";
		std::cin >> username;
		std::cout << "Enter password: ";
		std::cin >> password;
		std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

		std::string auth_msg = "AUTH " + username + " " + password;
		auto resp = send_and_receive(client_sock, server_addr, auth_msg);
		if (!resp)
			continue;
		if ((*resp)[0] == "OK") {
			std::cout << "Welcome to BitTrickle!\n";
			break;
		} else
			std::cout << "Authentication failed. Try again.\n";
	}

	std::thread welcome_thread(create_welcoming_socket);
	std::thread heartbeat_thread(send_heartbeat, client_sock, username,
	                             server_addr);

	std::cout << "Client online. Commands: get, lap, lpf, pub, sch, unp, xit\n";

	std::string line;
	while (!server_shutdown) {
		std::cout << "> ";
		std::getline(std::cin, line);
		if (line.empty())
			continue;

		std::istringstream iss(line);
		std::string cmd, arg;
		iss >> cmd >> arg;

		if (cmd == "xit")
			break;
		else if (cmd == "lap")
			handle_lap(client_sock, server_addr, username);
		else if (cmd == "lpf")
			handle_lpf(client_sock, server_addr, username);
		else if (cmd == "pub" && !arg.empty())
			handle_pub(client_sock, server_addr, username, arg);
		else if (cmd == "unp" && !arg.empty())
			handle_unp(client_sock, server_addr, username, arg);
		else if (cmd == "sch" && !arg.empty())
			handle_sch(client_sock, server_addr, username, arg);
		else if (cmd == "get" && !arg.empty())
			handle_get(client_sock, server_addr, username, arg);
		else
			std::cout << "Unknown command or missing argument\n";
	}

	stop_welcome = true;
	stop_heartbeat = true;

	// Unblock accept
	int tmp_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (tmp_sock >= 0) {
		sockaddr_in tmp_addr{};
		tmp_addr.sin_family = AF_INET;
		tmp_addr.sin_port = htons(welcoming_port_number);
		inet_pton(AF_INET, "127.0.0.1", &tmp_addr.sin_addr);
		connect(tmp_sock, (struct sockaddr *)&tmp_addr, sizeof(tmp_addr));
		close(tmp_sock);
	}

	welcome_thread.join();
	heartbeat_thread.join();
	close(client_sock);

	{
		std::lock_guard<std::mutex> lock(upload_mutex);
		for (auto &t : upload_threads)
			t.join();
	}
	{
		std::lock_guard<std::mutex> lock(download_mutex);
		for (auto &t : download_threads)
			t.join();
	}

	std::cout << "Client offline\n";
	return 0;
}
