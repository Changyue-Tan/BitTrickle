#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <ctime>
#include <errno.h>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <poll.h> // Added for non-blocking I/O
#include <queue>
#include <set>
#include <sstream>
#include <string.h>
#include <string>
#include <thread>
#include <unistd.h>

// Constants
#define BUFFER_SIZE 4096
#define HEARTBEAT_TIMEOUT 10
#define MAX_FILENAME_LENGTH 255
#define MAX_USERNAME_LENGTH 50
#define MAX_CLIENTS 100
#define MAX_FILES_PER_USER 50
#define POLL_TIMEOUT_MS 1000 // Added for poll timeout

// Rate limiting
constexpr size_t MAX_REQUESTS_PER_MINUTE = 60;
constexpr size_t REQUEST_WINDOW_SIZE = 60;

class RateLimiter {
 private:
	std::mutex mtx;
	std::map<std::string, std::queue<std::chrono::steady_clock::time_point>> client_requests;

 public:
	bool is_rate_limited(const std::string& client_ip) {
		std::lock_guard<std::mutex> lock(mtx);
		auto now = std::chrono::steady_clock::now();
		auto& requests = client_requests[client_ip];

		while (!requests.empty()
		       && std::chrono::duration_cast<std::chrono::seconds>(now - requests.front()).count() > REQUEST_WINDOW_SIZE)
		{
			requests.pop();
		}

		if (requests.size() >= MAX_REQUESTS_PER_MINUTE) {
			return true;
		}

		requests.push(now);
		return false;
	}
};

class Server {
 private:
	int port;
	int sockfd;
	std::mutex state_mtx;
	std::atomic<bool> running{true};
	RateLimiter rate_limiter;

	std::map<std::string, std::string> credentials;
	std::map<std::string, std::chrono::steady_clock::time_point> heartbeats;
	std::set<std::string> active_clients;
	std::map<std::string, std::set<std::string>> published_files;
	std::map<std::string, std::set<std::string>> user_files;
	std::map<std::string, int> udp_port_map; // Last UDP source port for server messages
	std::map<std::string, int> p2p_port_map; // Client’s listening port for P2P TCP

	// Statistics - using atomic for lock-free access
	std::atomic<size_t> total_connections{0};
	std::atomic<size_t> failed_auths{0};
	std::atomic<size_t> published_files_count{0};

	static volatile sig_atomic_t shutdown_requested;

 public:
	Server(int port)
	: port(port)
	, sockfd(-1) {}

	~Server() {
		if (sockfd >= 0) {
			close(sockfd);
		}
	}

	bool load_credentials(const std::string& filename) {
		std::ifstream file(filename);
		if (!file.is_open()) {
			std::cerr << "Warning: Could not open credentials file. Using default admin/admin.\n";
			credentials["admin"] = "admin";
			return false;
		}

		std::string user, pass;
		size_t line_count = 0;
		while (file >> user >> pass && line_count < 1000) {
			if (validate_username(user) && validate_password(pass)) {
				credentials[user] = pass;
				line_count++;
			}
		}
		file.close();

		if (credentials.empty()) {
			credentials["admin"] = "admin";
		}

		// Simple logging without mutex
		std::cout << "[INFO] Loaded " << credentials.size() << " user credentials\n";
		return true;
	}

	// Simple logging - no mutex needed, just avoid calling from critical sections
	void log(const std::string& msg) {
		auto now = std::chrono::system_clock::now();
		std::time_t now_time = std::chrono::system_clock::to_time_t(now);
		std::tm now_tm = *std::localtime(&now_time);

		// Use cerr for unbuffered output
		std::cerr << "[" << std::put_time(&now_tm, "%Y-%m-%d %H:%M:%S") << "] " << msg << std::endl;
	}

	bool validate_username(const std::string& username) {
		if (username.empty() || username.length() > MAX_USERNAME_LENGTH) {
			return false;
		}
		for (char c : username) {
			if (!std::isalnum(c) && c != '_') {
				return false;
			}
		}
		return true;
	}

	bool validate_password(const std::string& password) {
		return !password.empty() && password.length() <= 100;
	}

	bool validate_filename(const std::string& filename) {
		if (filename.empty() || filename.length() > MAX_FILENAME_LENGTH) {
			return false;
		}
		if (filename.find("..") != std::string::npos || filename.find("/") != std::string::npos
		    || filename.find("\\") != std::string::npos || filename[0] == '.')
		{
			return false;
		}
		const std::string invalid_chars = "<>:\\\"|?*";
		return filename.find_first_of(invalid_chars) == std::string::npos;
	}

	bool check_credentials(const std::string& username, const std::string& password) {
		return credentials.count(username) && credentials[username] == password;
	}

	std::string get_client_ip(const sockaddr_in& client_addr) {
		char ip_str[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &(client_addr.sin_addr), ip_str, INET_ADDRSTRLEN);
		return std::string(ip_str);
	}

	// Simple non-blocking send using MSG_DONTWAIT
	void send_response(const std::string& response, const struct sockaddr_in& client_addr) {
		if (sockfd < 0)
			return;

		socklen_t addr_len = sizeof(client_addr);
		std::string client_ip = get_client_ip(client_addr);
		int client_port = ntohs(client_addr.sin_port);

		// Log outgoing message (size, destination and content)
		try {
			std::ostringstream oss;
			oss << "Sending (" << response.size() << " bytes) to " << client_ip << ":" << client_port << " -> \"";
			// Truncate long responses in logs to keep output readable
			const size_t MAX_LOG_LEN = 1024;
			if (response.size() > MAX_LOG_LEN) {
				oss << response.substr(0, MAX_LOG_LEN) << "...[truncated]\"";
			}
			else {
				oss << response << "\"";
			}
			log(oss.str());
		} catch (...) {
			// Logging must not crash the server
		}

		// Use MSG_DONTWAIT for non-blocking send
		ssize_t sent =
		    sendto(sockfd, response.c_str(), response.size(), MSG_DONTWAIT, (struct sockaddr*)&client_addr, addr_len);

		if (sent < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				log(std::string("sendto() would block for ") + client_ip + ":" + std::to_string(client_port));
			}
			else {
				log(std::string("sendto() failed: ") + strerror(errno));
			}
		}
		else if ((size_t)sent != response.size()) {
			std::ostringstream oss2;
			oss2 << "Partial send to " << client_ip << ":" << client_port << " (" << sent << "/" << response.size()
			     << " bytes)";
			log(oss2.str());
		}
	}

	void broadcast(const std::string& message) {
		std::vector<std::pair<std::string, int>> clients_to_notify;

		{
			std::lock_guard<std::mutex> state_lock(state_mtx);
			for (const auto& [user, udp_port] : udp_port_map) {
				clients_to_notify.emplace_back(user, udp_port); // use UDP port
			}
		}

		// Log broadcast summary
		try {
			std::ostringstream oss;
			oss << "Broadcasting message to " << clients_to_notify.size() << " clients -> \"";
			const size_t MAX_LOG_LEN = 512;
			if (message.size() > MAX_LOG_LEN)
				oss << message.substr(0, MAX_LOG_LEN) << "...[truncated]\"";
			else
				oss << message << "\"";
			log(oss.str());
		} catch (...) {
		}

		for (const auto& [user, udp_port] : clients_to_notify) {
			struct sockaddr_in client_addr{};
			client_addr.sin_family = AF_INET;
			client_addr.sin_port = htons(udp_port); // send to correct UDP port
			inet_pton(AF_INET, "127.0.0.1", &client_addr.sin_addr);
			send_response(message, client_addr);
		}
	}

	void handle_auth(const std::string& username, const std::string& password, const sockaddr_in& client_addr) {
		std::string client_ip = get_client_ip(client_addr);

		if (rate_limiter.is_rate_limited(client_ip)) {
			send_response("ERR Rate limit exceeded", client_addr);
			return;
		}

		if (!validate_username(username) || !validate_password(password)) {
			send_response("ERR Invalid credentials format", client_addr);
			failed_auths++;
			return;
		}

		std::string response;
		bool log_success = false;
		std::string log_msg;

		{
			std::lock_guard<std::mutex> state_lock(state_mtx);

			if (active_clients.size() >= MAX_CLIENTS) {
				response = "ERR Server full";
			}
			else if (check_credentials(username, password)) {
				if (active_clients.count(username)) {
					response = "ERR Already connected";
				}
				else {
					active_clients.insert(username);
					heartbeats[username] = std::chrono::steady_clock::now();
					udp_port_map[username] = ntohs(client_addr.sin_port);
					response = "OK";
					log_success = true;
					total_connections++;
				}
			}
			else {
				response = "ERR Invalid credentials";
				failed_auths++;
			}
		}

		send_response(response, client_addr);

		// Log after releasing lock
		if (log_success) {
			log("User " + username + " authenticated from " + client_ip);
		}
	}

	void handle_hbt(const std::string& username,
	                const std::string& welcoming_port_str, // received as string
	                const sockaddr_in& client_addr) {
		if (!validate_username(username)) {
			send_response("ERR Invalid username", client_addr);
			return;
		}

		int welcoming_port = 0;
		try {
			welcoming_port = std::stoi(welcoming_port_str);
		} catch (...) {
			send_response("ERR Invalid port", client_addr);
			return;
		}

		int udp_port = ntohs(client_addr.sin_port); // source UDP port for server messages
		std::string client_ip = get_client_ip(client_addr);

		{
			std::lock_guard<std::mutex> lock(state_mtx);
			if (active_clients.count(username)) {
				heartbeats[username] = std::chrono::steady_clock::now();
				udp_port_map[username] = udp_port; // for server broadcasts
				p2p_port_map[username] = welcoming_port; // for P2P TCP connections
			}
		}

		log("[HBT] Updated " + username + " port=" + std::to_string(welcoming_port) + " ip=" + client_ip);

		// Do NOT send HBT_ACK to avoid confusion
	}

	void handle_lap(const std::string& username, const sockaddr_in& client_addr) {
		if (!validate_username(username)) {
			send_response("ERR Invalid username", client_addr);
			return;
		}

		std::string response;
		{
			std::lock_guard<std::mutex> state_lock(state_mtx);

			if (!active_clients.count(username)) {
				response = "ERR Not authenticated";
			}
			else {
				response = "OK " + std::to_string(active_clients.size());
				for (const auto& user : active_clients) {
					response += " " + user;
				}
			}
		}

		send_response(response, client_addr);
	}

	void handle_lpf(const std::string& username, const sockaddr_in& client_addr) {
		if (!validate_username(username)) {
			send_response("ERR Invalid username", client_addr);
			return;
		}

		std::string response;
		{
			std::lock_guard<std::mutex> state_lock(state_mtx);

			if (!active_clients.count(username)) {
				response = "ERR Not authenticated";
			}
			else {
				auto user_files_it = user_files.find(username);
				if (user_files_it == user_files.end() || user_files_it->second.empty()) {
					response = "OK 0";
				}
				else {
					response = "OK " + std::to_string(user_files_it->second.size());
					for (const auto& file : user_files_it->second) {
						response += " " + file;
					}
				}
			}
		}

		send_response(response, client_addr);
	}

	void handle_pub(const std::string& username, const std::string& filename, const sockaddr_in& client_addr) {
		if (!validate_username(username) || !validate_filename(filename)) {
			send_response("ERR Invalid input", client_addr);
			return;
		}

		std::string response;
		bool success = false;

		{
			std::lock_guard<std::mutex> state_lock(state_mtx);

			if (!active_clients.count(username)) {
				response = "ERR Not authenticated";
			}
			else if (user_files[username].size() >= MAX_FILES_PER_USER) {
				response = "ERR Too many files published";
			}
			else if (user_files[username].count(filename)) {
				response = "ERR File already published";
			}
			else {
				published_files[filename].insert(username);
				user_files[username].insert(filename);
				published_files_count++;
				response = "OK";
				success = true;
			}
		}

		send_response(response, client_addr);

		if (success) {
			log("User " + username + " published: " + filename);
		}
	}

	void handle_unp(const std::string& username, const std::string& filename, const sockaddr_in& client_addr) {
		if (!validate_username(username) || !validate_filename(filename)) {
			send_response("ERR Invalid input", client_addr);
			return;
		}

		std::string response;
		bool success = false;

		{
			std::lock_guard<std::mutex> state_lock(state_mtx);

			if (!active_clients.count(username)) {
				response = "ERR Not authenticated";
			}
			else if (user_files[username].count(filename)) {
				user_files[username].erase(filename);
				published_files[filename].erase(username);
				if (published_files[filename].empty()) {
					published_files.erase(filename);
				}
				response = "OK";
				success = true;
			}
			else {
				response = "ERR File not published by you";
			}
		}

		send_response(response, client_addr);

		if (success) {
			log("User " + username + " unpublished: " + filename);
		}
	}

	void handle_sch(const std::string& username, const std::string& substring, const sockaddr_in& client_addr) {
		if (!validate_username(username)) {
			send_response("ERR Invalid username", client_addr);
			return;
		}

		if (substring.empty() || substring.length() > 100) {
			send_response("ERR Invalid search term", client_addr);
			return;
		}

		std::string response;
		std::set<std::string> files_found;

		{
			std::lock_guard<std::mutex> state_lock(state_mtx);

			if (!active_clients.count(username)) {
				response = "ERR Not authenticated";
			}
			else {
				for (const auto& [file, users] : published_files) {
					if (!users.empty() && file.find(substring) != std::string::npos) {
						bool has_active_publisher = false;
						for (const auto& publisher : users) {
							if (active_clients.count(publisher)) {
								has_active_publisher = true;
								break;
							}
						}
						if (has_active_publisher) {
							files_found.insert(file);
						}
					}
				}

				response = "OK " + std::to_string(files_found.size());
				for (const auto& file : files_found) {
					response += " " + file;
				}
			}
		}

		send_response(response, client_addr);
	}

	void handle_get(const std::string& username, const std::string& filename, const sockaddr_in& client_addr) {
		if (!validate_username(username) || !validate_filename(filename)) {
			send_response("ERR Invalid input", client_addr);
			return;
		}

		std::string response = "ERR File not available";

		{
			std::lock_guard<std::mutex> state_lock(state_mtx);

			if (!active_clients.count(username)) {
				response = "ERR Not authenticated";
			}
			else if (published_files.count(filename) && !published_files[filename].empty()) {
				for (const auto& user : published_files[filename]) {
					if (user != username && active_clients.count(user)) {
						// Use the P2P port for actual peer-to-peer connection
						auto port_it = p2p_port_map.find(user);
						if (port_it != p2p_port_map.end()) {
							response = "OK " + user + " " + std::to_string(port_it->second);
							break;
						}
					}
				}
				if (response == "ERR File not available") {
					response = "ERR No active peer has file";
				}
			}
			else {
				response = "ERR File not published";
			}
		}

		send_response(response, client_addr);
	}

	void handle_stats(const std::string& username, const sockaddr_in& client_addr) {
		if (!validate_username(username)) {
			send_response("ERR Invalid username", client_addr);
			return;
		}

		std::string response;
		size_t active_count, files_count;

		{
			std::lock_guard<std::mutex> state_lock(state_mtx);

			if (!active_clients.count(username)) {
				response = "ERR Not authenticated";
			}
			else {
				active_count = active_clients.size();
				files_count = published_files.size();
			}
		}

		if (response.empty()) {
			response = "OK STATS active_clients:" + std::to_string(active_count) + " total_files:"
			           + std::to_string(files_count) + " total_connections:" + std::to_string(total_connections.load())
			           + " failed_auths:" + std::to_string(failed_auths.load());
		}

		send_response(response, client_addr);
	}

	void monitor_heartbeats() {
		while (running && shutdown_requested == 0) {
			std::this_thread::sleep_for(std::chrono::seconds(2));

			std::vector<std::string> timed_out_users;

			{
				std::lock_guard<std::mutex> state_lock(state_mtx);
				auto now = std::chrono::steady_clock::now();

				auto it = heartbeats.begin();
				while (it != heartbeats.end()) {
					auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second);
					if (elapsed.count() > HEARTBEAT_TIMEOUT) {
						const std::string& username = it->first;
						timed_out_users.push_back(username);

						active_clients.erase(username);
						udp_port_map.erase(username);
						p2p_port_map.erase(username);

						for (const auto& filename : user_files[username]) {
							published_files[filename].erase(username);
							if (published_files[filename].empty()) {
								published_files.erase(filename);
							}
						}
						user_files.erase(username);

						it = heartbeats.erase(it);
					}
					else {
						++it;
					}
				}
			}

			// Log after releasing lock
			for (const auto& username : timed_out_users) {
				log("User " + username + " timed out");
			}
		}
	}

	void start() {
		sockfd = socket(AF_INET, SOCK_DGRAM, 0);
		if (sockfd < 0) {
			std::cerr << "Socket creation failed\n";
			return;
		}

		int opt = 1;
		setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

		sockaddr_in server_addr{};
		server_addr.sin_family = AF_INET;
		server_addr.sin_addr.s_addr = INADDR_ANY;
		server_addr.sin_port = htons(port);

		if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
			std::cerr << "Bind failed on port " << port << std::endl;
			close(sockfd);
			sockfd = -1;
			return;
		}

		log("Server started on port " + std::to_string(port));
		std::thread hb_thread(&Server::monitor_heartbeats, this);

		// Use poll() for timeout handling (THE KEY FIX)
		struct pollfd pfd;
		pfd.fd = sockfd;
		pfd.events = POLLIN;

		while (running && shutdown_requested == 0) {
			// Poll with timeout so we can check shutdown flag
			int poll_result = poll(&pfd, 1, POLL_TIMEOUT_MS);

			if (poll_result < 0) {
				if (errno == EINTR)
					continue; // Interrupted by signal
				log("Poll error");
				break;
			}

			if (poll_result == 0) {
				// Timeout - just check shutdown flag and continue
				continue;
			}

			if (pfd.revents & POLLIN) {
				char buffer[BUFFER_SIZE];
				sockaddr_in client_addr{};
				socklen_t client_len = sizeof(client_addr);

				ssize_t n = recvfrom(sockfd, buffer, BUFFER_SIZE - 1, 0, (struct sockaddr*)&client_addr, &client_len);

				if (n <= 0)
					continue;

				buffer[n] = '\0';
				std::string request(buffer, n);

				// --- NEW: verbose logging of raw messages ---
				std::string client_ip = get_client_ip(client_addr);
				int client_port = ntohs(client_addr.sin_port);
				log("Received (" + std::to_string(n) + " bytes) from " + client_ip + ":" + std::to_string(client_port)
				    + " -> \"" + request + "\"");

				// Parse request
				std::istringstream iss(request);
				std::vector<std::string> tokens;
				std::string token;
				while (iss >> token && tokens.size() < 10) {
					tokens.push_back(token);
				}

				// --- NEW: log parsed tokens ---
				std::string token_line = "Parsed tokens:";
				for (const auto& t : tokens)
					token_line += " [" + t + "]";
				log(token_line);

				if (tokens.empty()) {
					send_response("ERR Empty command", client_addr);
					continue;
				}

				std::string req_type = tokens[0];
				std::string username = tokens.size() > 1 ? tokens[1] : "";

				// Route commands
				try {
					if (req_type == "AUTH" && tokens.size() >= 3) {
						handle_auth(username, tokens[2], client_addr);
					}
					else if (req_type == "HBT" && tokens.size() >= 2) {
						handle_hbt(username, tokens[2], client_addr);
					}
					else if (req_type == "LAP" && tokens.size() >= 2) {
						handle_lap(username, client_addr);
					}
					else if (req_type == "LPF" && tokens.size() >= 2) {
						handle_lpf(username, client_addr);
					}
					else if (req_type == "PUB" && tokens.size() >= 3) {
						handle_pub(username, tokens[2], client_addr);
					}
					else if (req_type == "UNP" && tokens.size() >= 3) {
						handle_unp(username, tokens[2], client_addr);
					}
					else if (req_type == "SCH" && tokens.size() >= 3) {
						handle_sch(username, tokens[2], client_addr);
					}
					else if (req_type == "GET" && tokens.size() >= 3) {
						handle_get(username, tokens[2], client_addr);
					}
					else if (req_type == "STATS" && tokens.size() >= 2) {
						handle_stats(username, client_addr);
					}
					else {
						send_response("ERR Invalid command", client_addr);
					}
				} catch (const std::exception& ex) {
					log("Exception: " + std::string(ex.what()));
					send_response("ERR Internal server error", client_addr);
				}
			}
		}

		if (shutdown_requested) {
			shutdown_server();
		}

		if (hb_thread.joinable()) {
			hb_thread.join();
		}
	}

	void shutdown_server() {
		log("Server shutdown initiated");
		running = false;

		broadcast("SRV_SHUTDOWN");
		std::this_thread::sleep_for(std::chrono::seconds(1));

		if (sockfd >= 0) {
			close(sockfd);
			sockfd = -1;
		}

		log("Server shutdown complete");
	}

	static Server* instance;
	static void signal_handler(int signal) {
		shutdown_requested = true;
		if (instance) {
			std::cout << "\nReceived signal " << signal << ", shutting down...\n";
		}
	}
};

Server* Server::instance = nullptr;
volatile sig_atomic_t Server::shutdown_requested = 0;

int main(int argc, char* argv[]) {
	if (argc != 2) {
		std::cerr << "Usage: " << argv[0] << " <port>\n";
		return 1;
	}

	int port;
	try {
		port = std::stoi(argv[1]);
		if (port <= 0 || port > 65535) {
			std::cerr << "Error: Port must be between 1 and 65535\n";
			return 1;
		}
	} catch (const std::exception&) {
		std::cerr << "Error: Invalid port number\n";
		return 1;
	}

	Server server(port);
	Server::instance = &server;

	signal(SIGINT, Server::signal_handler);
	signal(SIGTERM, Server::signal_handler);

	if (!server.load_credentials("credentials.txt")) {
		std::cout << "Using default credentials.\n";
	}

	std::cout << "Starting P2P server...\n";
	std::cout << "Press Ctrl+C to shutdown gracefully.\n\n";

	server.start();
	return 0;
}
