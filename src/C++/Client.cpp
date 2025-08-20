#include <arpa/inet.h>
#include <sys/socket.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

// Constants
constexpr size_t BUFFER_SIZE = 4096;
constexpr size_t MAX_FILENAME_LENGTH = 255;
constexpr size_t MAX_CONCURRENT_UPLOADS = 5;
constexpr size_t MAX_CONCURRENT_DOWNLOADS = 3;
constexpr size_t MAX_FILE_SIZE = 100 * 1024 * 1024;
constexpr int ACCEPT_POLL_TIMEOUT_MS = 1000;

// Global variables
std::atomic<bool> stop_welcome(false);
std::atomic<bool> stop_heartbeat(false);
std::atomic<bool> welcoming_port_ready(false);
int welcoming_port_number = 0;
std::atomic<bool> server_shutdown(false);

// Simple thread tracking (no complex pool needed)
std::mutex upload_mutex;
std::vector<std::thread> upload_threads;
std::atomic<size_t> active_uploads(0);

std::mutex download_mutex;
std::vector<std::thread> download_threads;
std::atomic<size_t> active_downloads(0);

// ==================== Input Validation ====================
bool validate_filename(const std::string& filename) {
	if (filename.empty() || filename.length() > MAX_FILENAME_LENGTH) {
		return false;
	}

	if (filename.find("..") != std::string::npos || filename.find("/") != std::string::npos
	    || filename.find("\\") != std::string::npos || filename[0] == '.')
	{
		std::cerr << "Error: Invalid filename\n";
		return false;
	}

	const std::string invalid_chars = "<>:\"|?*";
	if (filename.find_first_of(invalid_chars) != std::string::npos) {
		std::cerr << "Error: Invalid characters in filename\n";
		return false;
	}

	return true;
}

bool file_exists(const std::string& filename) {
	std::ifstream file(filename);
	return file.good();
}

size_t get_file_size(const std::string& filename) {
	try {
		return std::filesystem::file_size(filename);
	} catch (const std::filesystem::filesystem_error&) {
		return 0;
	}
}

// ==================== Socket Helpers ====================
// Set socket to non-blocking mode
bool set_non_blocking(int sock) {
	int flags = fcntl(sock, F_GETFL, 0);
	if (flags == -1)
		return false;
	return fcntl(sock, F_SETFL, flags | O_NONBLOCK) != -1;
}

// Simple recv with timeout
ssize_t safe_recv(int socket, char* buffer, size_t buffer_size, int timeout_seconds = 10) {
	struct timeval tv;
	tv.tv_sec = timeout_seconds;
	tv.tv_usec = 0;
	setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	ssize_t bytes = recv(socket, buffer, buffer_size - 1, 0);
	if (bytes > 0) {
		buffer[bytes] = '\0';
	}
	return bytes;
}

// Simple send with timeout
ssize_t safe_send(int socket, const void* buffer, size_t length) {
	struct timeval tv;
	tv.tv_sec = 10;
	tv.tv_usec = 0;
	setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	const char* data = static_cast<const char*>(buffer);
	size_t total_sent = 0;

	while (total_sent < length) {
		ssize_t sent = send(socket, data + total_sent, length - total_sent, MSG_NOSIGNAL);
		if (sent < 0) {
			return -1;
		}
		total_sent += sent;
	}

	return total_sent;
}

// ==================== P2P Download ====================
void downloading_sequence(int peer_port, const std::string& download_filename, const std::string& peername) {
	if (!validate_filename(download_filename)) {
		std::cerr << "Invalid filename: " << download_filename << std::endl;
		active_downloads--;
		return;
	}

	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		std::cerr << "Failed to create download socket\n";
		active_downloads--;
		return;
	}

	sockaddr_in peer_addr{};
	peer_addr.sin_family = AF_INET;
	peer_addr.sin_port = htons(peer_port);
	inet_pton(AF_INET, "127.0.0.1", &peer_addr.sin_addr);

	std::cout << "Connecting to " << peername << "...\n";

	// Set connection timeout
	struct timeval tv;
	tv.tv_sec = 10;
	tv.tv_usec = 0;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	if (connect(sock, (struct sockaddr*)&peer_addr, sizeof(peer_addr)) < 0) {
		std::cerr << "Connection failed to " << peername << std::endl;
		close(sock);
		active_downloads--;
		return;
	}

	// Send filename request
	if (safe_send(sock, download_filename.c_str(), download_filename.size()) < 0) {
		std::cerr << "Failed to send filename request\n";
		close(sock);
		active_downloads--;
		return;
	}

	// Create temporary file
	std::string temp_filename = download_filename + ".tmp";
	std::ofstream file(temp_filename, std::ios::binary);
	if (!file.is_open()) {
		std::cerr << "Failed to create temporary file\n";
		close(sock);
		active_downloads--;
		return;
	}

	char buffer[BUFFER_SIZE];
	size_t total_received = 0;
	bool success = true;

	while (true) {
		ssize_t bytes = safe_recv(sock, buffer, BUFFER_SIZE);
		if (bytes <= 0)
			break;

		total_received += bytes;
		if (total_received > MAX_FILE_SIZE) {
			std::cerr << "File too large, aborting\n";
			success = false;
			break;
		}

		file.write(buffer, bytes);
		if (file.fail()) {
			std::cerr << "Failed to write to file\n";
			success = false;
			break;
		}
	}

	file.close();
	close(sock);

	if (success && total_received > 0) {
		try {
			std::filesystem::rename(temp_filename, download_filename);
			std::cout << "'" << download_filename << "' downloaded successfully! (" << total_received << " bytes)\n";
		} catch (const std::filesystem::filesystem_error& ex) {
			std::cerr << "Failed to save download: " << ex.what() << std::endl;
			std::filesystem::remove(temp_filename);
		}
	}
	else {
		std::filesystem::remove(temp_filename);
		if (total_received == 0) {
			std::cerr << "File not found or empty\n";
		}
	}

	active_downloads--;
}

// ==================== P2P Upload ====================
void uploading_sequence(int upload_socket) {
	active_uploads++;

	char buffer[BUFFER_SIZE];
	ssize_t bytes = safe_recv(upload_socket, buffer, BUFFER_SIZE);
	if (bytes <= 0) {
		std::cerr << "Failed to receive filename request\n";
		close(upload_socket);
		active_uploads--;
		return;
	}

	std::string requested_file(buffer, bytes);

	if (!validate_filename(requested_file)) {
		std::cerr << "Invalid filename requested: " << requested_file << std::endl;
		close(upload_socket);
		active_uploads--;
		return;
	}

	if (!file_exists(requested_file)) {
		std::cerr << "Requested file does not exist: " << requested_file << std::endl;
		close(upload_socket);
		active_uploads--;
		return;
	}

	size_t file_size = get_file_size(requested_file);
	if (file_size == 0 || file_size > MAX_FILE_SIZE) {
		std::cerr << "File size invalid: " << requested_file << std::endl;
		close(upload_socket);
		active_uploads--;
		return;
	}

	std::ifstream file(requested_file, std::ios::binary);
	if (!file.is_open()) {
		std::cerr << "Failed to open file: " << requested_file << std::endl;
		close(upload_socket);
		active_uploads--;
		return;
	}

	std::cout << "Uploading '" << requested_file << "' (" << file_size << " bytes)...\n";

	size_t total_sent = 0;
	while (file.read(buffer, BUFFER_SIZE) || file.gcount() > 0) {
		ssize_t sent = safe_send(upload_socket, buffer, file.gcount());
		if (sent < 0) {
			std::cerr << "Send error for file: " << requested_file << std::endl;
			break;
		}
		total_sent += sent;
	}

	if (total_sent == file_size) {
		std::cout << "'" << requested_file << "' uploaded successfully!\n";
	}

	file.close();
	close(upload_socket);
	active_uploads--;
}

// ==================== Welcoming Socket (KEY FIX: non-blocking accept) ====================
void create_welcoming_socket() {
	int welcoming_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (welcoming_sock < 0) {
		std::cerr << "Failed to create welcoming socket\n";
		return;
	}

	int opt = 1;
	setsockopt(welcoming_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	// KEY FIX: Set non-blocking mode
	if (!set_non_blocking(welcoming_sock)) {
		std::cerr << "Failed to set non-blocking mode\n";
		close(welcoming_sock);
		return;
	}

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = 0;

	if (bind(welcoming_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		std::cerr << "Bind failed\n";
		close(welcoming_sock);
		return;
	}

	socklen_t addr_len = sizeof(addr);
	getsockname(welcoming_sock, (struct sockaddr*)&addr, &addr_len);
	welcoming_port_number = ntohs(addr.sin_port);
	welcoming_port_ready = true;

	std::cout << "P2P port: " << welcoming_port_number << "\n";

	if (listen(welcoming_sock, 5) < 0) {
		std::cerr << "Listen failed\n";
		close(welcoming_sock);
		return;
	}

	// KEY FIX: Use poll() for non-blocking accept
	struct pollfd pfd;
	pfd.fd = welcoming_sock;
	pfd.events = POLLIN;

	while (!stop_welcome) {
		int poll_result = poll(&pfd, 1, ACCEPT_POLL_TIMEOUT_MS);

		if (poll_result < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		if (poll_result == 0) {
			// Timeout - check stop flag and continue
			continue;
		}

		if (pfd.revents & POLLIN) {
			sockaddr_in client_addr{};
			socklen_t client_len = sizeof(client_addr);
			int upload_sock = accept(welcoming_sock, (struct sockaddr*)&client_addr, &client_len);

			if (upload_sock < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					continue;
				}
				std::cerr << "Accept error\n";
				continue;
			}

			// Simple check for max uploads
			if (active_uploads >= MAX_CONCURRENT_UPLOADS) {
				std::cerr << "Max uploads reached, rejecting connection\n";
				close(upload_sock);
				continue;
			}

			// Create upload thread
			std::lock_guard<std::mutex> lock(upload_mutex);
			upload_threads.emplace_back([upload_sock]() { uploading_sequence(upload_sock); });
		}
	}

	close(welcoming_sock);
	std::cout << "Welcoming socket closed\n";
}

// ==================== Heartbeat ====================
void send_heartbeat(int client_socket, const std::string& username, const sockaddr_in& server_addr) {
	// Wait for welcoming port
	int attempts = 0;
	while (!welcoming_port_ready && attempts < 50) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		attempts++;
	}

	if (!welcoming_port_ready) {
		std::cerr << "Welcoming port not ready\n";
		return;
	}

	std::string heartbeat_msg = "HBT " + username + " " + std::to_string(welcoming_port_number);

	while (!stop_heartbeat && !server_shutdown) {
		sendto(client_socket,
		       heartbeat_msg.c_str(),
		       heartbeat_msg.size(),
		       MSG_NOSIGNAL,
		       (struct sockaddr*)&server_addr,
		       sizeof(server_addr));
		std::this_thread::sleep_for(std::chrono::seconds(2));
	}
}

// ==================== UDP Communication ====================
std::optional<std::vector<std::string>>
send_and_receive(int client_socket, const sockaddr_in& server_addr, const std::string& msg, int timeout_sec = 5) {
	// Send
	ssize_t sent = sendto(client_socket, msg.c_str(), msg.size(), 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
	if (sent < 0) {
		std::cerr << "[DBG] sendto() failed: " << strerror(errno) << " (msg=\"" << msg << "\")\n";
		return std::nullopt;
	}
	std::cerr << "[DBG] Sent " << sent << " bytes -> \"" << msg << "\"\n";

	// Poll
	struct pollfd pfd;
	pfd.fd = client_socket;
	pfd.events = POLLIN;

	int poll_result = poll(&pfd, 1, timeout_sec * 1000);
	if (poll_result < 0) {
		std::cerr << "[DBG] poll() error: " << strerror(errno) << "\n";
		return std::nullopt;
	}
	else if (poll_result == 0) {
		std::cerr << "[DBG] poll() timeout after " << timeout_sec << "s (no data)\n";
		return std::nullopt;
	}
	else {
		std::cerr << "[DBG] poll() reports " << poll_result << " events\n";
	}

	// recvfrom
	char buffer[BUFFER_SIZE];
	sockaddr_in response_addr{};
	socklen_t addr_len = sizeof(response_addr);
	ssize_t bytes = recvfrom(client_socket, buffer, BUFFER_SIZE - 1, 0, (struct sockaddr*)&response_addr, &addr_len);

	if (bytes < 0) {
		std::cerr << "[DBG] recvfrom() error: " << strerror(errno) << "\n";
		return std::nullopt;
	}
	else if (bytes == 0) {
		std::cerr << "[DBG] recvfrom() returned 0 bytes\n";
		return std::nullopt;
	}

	buffer[bytes] = '\0';
	std::string resp(buffer, bytes);

	// Who sent it?
	char ipbuf[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &(response_addr.sin_addr), ipbuf, INET_ADDRSTRLEN);
	int srcport = ntohs(response_addr.sin_port);

	std::cerr << "[DBG] RAW SERVER REPLY (" << bytes << " bytes) from " << ipbuf << ":" << srcport << " -> \"" << resp
	          << "\"\n";

	// Show tokenization
	std::istringstream dbg_iss(resp);
	std::string dbg_tok;
	std::cerr << "[DBG] RAW TOKENS:";
	while (dbg_iss >> dbg_tok)
		std::cerr << " [" << dbg_tok << "]";
	std::cerr << "\n";

	if (resp == "SRV_SHUTDOWN") {
		std::cout << "Server is shutting down\n";
		server_shutdown = true;
		stop_welcome = true;
		stop_heartbeat = true;
		return std::nullopt;
	}

	// Parse response
	std::vector<std::string> tokens;
	std::istringstream iss(resp);
	std::string token;
	while (iss >> token && tokens.size() < 100) {
		tokens.push_back(token);
	}

	if (tokens[0] == "SRV_SHUTDOWN") {
		std::cout << "[INFO] Server is shutting down. Disconnecting...\n";
		stop_welcome = true;
		stop_heartbeat = true;
		server_shutdown = true;
	}

	return tokens;
}

// ==================== Command Handlers ====================
void handle_lap(int sock, const sockaddr_in& srv, const std::string& user) {
	auto resp = send_and_receive(sock, srv, "LAP " + user);
	if (!resp || resp->empty())
		return;

	if ((*resp)[0] == "OK" && resp->size() >= 2) {
		size_t count = std::stoul((*resp)[1]);
		std::cout << count << " active peers:\n";
		for (size_t i = 2; i < resp->size() && i < count + 2; ++i) {
			std::cout << " - " << (*resp)[i] << "\n";
		}
	}
}

void handle_lpf(int sock, const sockaddr_in& srv, const std::string& user) {
	auto resp = send_and_receive(sock, srv, "LPF " + user);
	if (!resp || resp->empty())
		return;

	if ((*resp)[0] == "OK" && resp->size() >= 2) {
		size_t count = std::stoul((*resp)[1]);
		if (count == 0) {
			std::cout << "No published files\n";
		}
		else {
			std::cout << "Your files (" << count << "):\n";
			for (size_t i = 2; i < resp->size() && i < count + 2; ++i) {
				std::cout << " - " << (*resp)[i] << "\n";
			}
		}
	}
}

void handle_pub(int sock, const sockaddr_in& srv, const std::string& user, const std::string& filename) {
	if (!validate_filename(filename) || !file_exists(filename)) {
		std::cout << "File not found: " << filename << std::endl;
		return;
	}

	auto resp = send_and_receive(sock, srv, "PUB " + user + " " + filename);
	if (!resp || resp->empty())
		return;

	std::cout << ((*resp)[0] == "OK" ? "Published: " : "Failed: ") << filename << std::endl;
}

void handle_unp(int sock, const sockaddr_in& srv, const std::string& user, const std::string& filename) {
	if (!validate_filename(filename))
		return;

	auto resp = send_and_receive(sock, srv, "UNP " + user + " " + filename);
	if (!resp || resp->empty())
		return;

	std::cout << ((*resp)[0] == "OK" ? "Unpublished: " : "Failed: ") << filename << std::endl;
}

void handle_sch(int sock, const sockaddr_in& srv, const std::string& user, const std::string& substr) {
	if (substr.empty() || substr.length() > 100) {
		std::cout << "Invalid search term\n";
		return;
	}

	auto resp = send_and_receive(sock, srv, "SCH " + user + " " + substr);
	if (!resp || resp->size() <= 1) {
		std::cout << "No files found\n";
		return;
	}

	if ((*resp)[0] == "OK") {
		size_t count = std::stoul((*resp)[1]);
		std::cout << count << " files found:\n";
		for (size_t i = 2; i < resp->size() && i < count + 2; ++i) {
			std::cout << " - " << (*resp)[i] << "\n";
		}
	}
}

void handle_get(int sock, const sockaddr_in& srv, const std::string& user, const std::string& filename) {
	if (!validate_filename(filename))
		return;

	auto resp = send_and_receive(sock, srv, "GET " + user + " " + filename);
	if (!resp || resp->empty() || (*resp)[0] != "OK" || resp->size() < 3) {
		std::cout << "File not available\n";
		return;
	}

	std::string peername = (*resp)[1];
	int peer_port = std::stoi((*resp)[2]);

	// Check download limit
	if (active_downloads >= MAX_CONCURRENT_DOWNLOADS) {
		std::cout << "Max downloads reached. Try again later.\n";
		return;
	}

	active_downloads++;

	// Create download thread
	std::lock_guard<std::mutex> lock(download_mutex);
	download_threads.emplace_back(
	    [peer_port, filename, peername]() { downloading_sequence(peer_port, filename, peername); });
}

// ==================== Thread Cleanup ====================
void cleanup_finished_threads() {
	// Clean up finished upload threads
	{
		std::lock_guard<std::mutex> lock(upload_mutex);
		auto it = upload_threads.begin();
		while (it != upload_threads.end()) {
			if (it->joinable()) {
				// Try to join with no wait (thread should be done)
				it->join();
			}
			it = upload_threads.erase(it);
		}
	}

	// Clean up finished download threads
	{
		std::lock_guard<std::mutex> lock(download_mutex);
		auto it = download_threads.begin();
		while (it != download_threads.end()) {
			if (it->joinable()) {
				it->join();
			}
			it = download_threads.erase(it);
		}
	}
}

// ==================== Signal Handler ====================
void signal_handler(int signal) {
	std::cout << "\nReceived signal " << signal << ", shutting down...\n";
	stop_welcome = true;
	stop_heartbeat = true;
	server_shutdown = true;
}

// ==================== Main ====================
int main(int argc, char* argv[]) {
	if (argc != 2) {
		std::cerr << "Usage: " << argv[0] << " <server_port>\n";
		return 1;
	}

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	int server_port;
	try {
		server_port = std::stoi(argv[1]);
		if (server_port <= 0 || server_port > 65535) {
			std::cerr << "Invalid port\n";
			return 1;
		}
	} catch (const std::exception&) {
		std::cerr << "Invalid port\n";
		return 1;
	}

	sockaddr_in server_addr{};
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(server_port);
	inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

	int client_sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (client_sock < 0) {
		std::cerr << "Socket creation failed\n";
		return 1;
	}

	// Authentication
	std::string username, password;
	int auth_attempts = 0;

	while (auth_attempts < 3) {
		std::cout << "Username: ";
		std::cin >> username;
		std::cout << "Password: ";
		std::cin >> password;
		std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

		if (username.length() > 50 || password.length() > 50) {
			std::cout << "Invalid input length\n";
			auth_attempts++;
			continue;
		}

		auto resp = send_and_receive(client_sock, server_addr, "AUTH " + username + " " + password);
		if (resp && !resp->empty() && (*resp)[0] == "OK") {
			std::cout << "Welcome, " << username << "!\n";
			break;
		}

		std::cout << "Authentication failed\n";
		auth_attempts++;
	}

	if (auth_attempts >= 3) {
		std::cout << "Too many failed attempts\n";
		close(client_sock);
		return 1;
	}

	// Start background threads
	std::thread welcome_thread(create_welcoming_socket);
	std::thread heartbeat_thread(send_heartbeat, client_sock, username, server_addr);

	std::cout << "\nCommands: get, lap, lpf, pub, sch, unp, status, xit\n\n";

	// Main command loop
	std::string line;
	int cleanup_counter = 0;

	while (!server_shutdown) {
		std::cout << "> ";
		if (!std::getline(std::cin, line))
			break;
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
		else if (cmd == "status") {
			std::cout << "Active uploads: " << active_uploads << "/" << MAX_CONCURRENT_UPLOADS
			          << ", Active downloads: " << active_downloads << "/" << MAX_CONCURRENT_DOWNLOADS << std::endl;
		}
		else
			std::cout << "Unknown command\n";

		// Periodic cleanup of finished threads
		if (++cleanup_counter % 10 == 0) {
			cleanup_finished_threads();
		}
	}

	// Shutdown
	std::cout << "Shutting down...\n";
	stop_welcome = true;
	stop_heartbeat = true;

	if (welcome_thread.joinable())
		welcome_thread.join();
	if (heartbeat_thread.joinable())
		heartbeat_thread.join();

	close(client_sock);

	// Final cleanup
	cleanup_finished_threads();

	std::cout << "Client offline\n";
	return 0;
}