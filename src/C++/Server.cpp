#include <arpa/inet.h>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <set>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#define BUFFER_SIZE       1024
#define HEARTBEAT_TIMEOUT 5  // seconds

class Server {
  private:
	int port;
	int sockfd;
	std::mutex mtx;

	std::map<std::string, std::string> credentials;  // username -> password
	std::map<std::string, time_t> heartbeats;  // username -> last heartbeat
	std::set<std::string> active_clients;
	std::map<std::string, std::set<std::string>>
	    published_files;  // filename -> usernames
	std::map<std::string, std::set<std::string>>
	    user_files;                           // username -> filenames
	std::map<std::string, int> contact_book;  // username -> port

	bool running = true;

  public:
	Server(int port) : port(port) {}

	void load_credentials(const std::string &filename) {
		std::ifstream file(filename);
		if (!file.is_open()) {
			std::cerr << "Failed to open credentials file\n";
			exit(EXIT_FAILURE);
		}
		std::string user, pass;
		while (file >> user >> pass) {
			credentials[user] = pass;
		}
		file.close();
	}

	void log(const std::string &msg) {
		auto now = std::chrono::system_clock::now();
		std::time_t now_time = std::chrono::system_clock::to_time_t(now);
		std::tm now_tm = *std::localtime(&now_time);
		std::cout << "[" << std::put_time(&now_tm, "%Y-%m-%d %H:%M:%S") << "] "
		          << msg << std::endl;
	}

	bool check_credentials(const std::string &username,
	                       const std::string &password) {
		return credentials.count(username) && credentials[username] == password;
	}

	void send_response(const std::string &response,
	                   const struct sockaddr_in &client_addr) {
		socklen_t addr_len = sizeof(client_addr);
		sendto(sockfd, response.c_str(), response.size(), 0,
		       (struct sockaddr *)&client_addr, addr_len);
		log("Sent response: " + response);
	}

	void broadcast(const std::string &message) {
		for (const auto &user : active_clients) {
			struct sockaddr_in client_addr{};
			client_addr.sin_family = AF_INET;
			client_addr.sin_port = htons(contact_book[user]);
			inet_pton(AF_INET, "127.0.0.1", &client_addr.sin_addr);
			send_response(message, client_addr);
		}
	}

	void handle_auth(const std::string &username, const std::string &password,
	                 const sockaddr_in &client_addr) {
		std::lock_guard<std::mutex> lock(mtx);
		std::string response = "ERR";
		if (check_credentials(username, password)) {
			active_clients.insert(username);
			heartbeats[username] = time(nullptr);
			contact_book[username] = ntohs(client_addr.sin_port);
			response = "OK";
		}
		send_response(response, client_addr);
	}

	void handle_hbt(const std::string &username,
	                const sockaddr_in &client_addr) {
		std::lock_guard<std::mutex> lock(mtx);
		if (active_clients.count(username)) {
			heartbeats[username] = time(nullptr);
			contact_book[username] = ntohs(client_addr.sin_port);
		}
	}

	void handle_lap(const sockaddr_in &client_addr) {
		std::lock_guard<std::mutex> lock(mtx);
		std::string response = "OK " + std::to_string(active_clients.size());
		for (const auto &user : active_clients) {
			response += " " + user;
		}
		send_response(response, client_addr);
	}

	void handle_pub(const std::string &username, const std::string &filename,
	                const sockaddr_in &client_addr) {
		std::lock_guard<std::mutex> lock(mtx);
		published_files[filename].insert(username);
		user_files[username].insert(filename);
		send_response("OK", client_addr);
	}

	void handle_unp(const std::string &username, const std::string &filename,
	                const sockaddr_in &client_addr) {
		std::lock_guard<std::mutex> lock(mtx);
		if (user_files[username].count(filename)) {
			user_files[username].erase(filename);
			published_files[filename].erase(username);
			send_response("OK", client_addr);
		} else {
			send_response("ERR", client_addr);
		}
	}

	void handle_sch(const std::string &username, const std::string &substring,
	                const sockaddr_in &client_addr) {
		(void)username;
		std::lock_guard<std::mutex> lock(mtx);
		std::set<std::string> files_found;
		for (const auto &[file, users] : published_files) {
			if (file.find(substring) != std::string::npos)
				files_found.insert(file);
		}
		std::string response = "OK " + std::to_string(files_found.size());
		for (const auto &file : files_found)
			response += " " + file;
		send_response(response, client_addr);
	}

	void handle_get(const std::string &username, const std::string &filename,
	                const sockaddr_in &client_addr) {
		std::lock_guard<std::mutex> lock(mtx);
		std::string response = "ERR";
		if (published_files.count(filename) &&
		    !published_files[filename].empty()) {
			for (const auto &user : published_files[filename]) {
				if (user != username && active_clients.count(user)) {
					response =
					    "OK " + user + " " + std::to_string(contact_book[user]);
					break;
				}
			}
			if (response == "ERR")
				response = "ERR No active peer has file";
		} else {
			response = "ERR File not published";
		}
		send_response(response, client_addr);
	}

	void monitor_heartbeats() {
		while (running) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
			std::lock_guard<std::mutex> lock(mtx);
			time_t now = time(nullptr);
			for (auto it = heartbeats.begin(); it != heartbeats.end();) {
				if (now - it->second > HEARTBEAT_TIMEOUT) {
					log("User " + it->first + " timed out.");
					active_clients.erase(it->first);
					it = heartbeats.erase(it);
				} else {
					++it;
				}
			}
		}
	}

	void start() {
		sockfd = socket(AF_INET, SOCK_DGRAM, 0);
		if (sockfd < 0) {
			std::cerr << "Socket creation failed\n";
			exit(EXIT_FAILURE);
		}

		sockaddr_in server_addr{};
		server_addr.sin_family = AF_INET;
		server_addr.sin_addr.s_addr = INADDR_ANY;
		server_addr.sin_port = htons(port);

		if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) <
		    0) {
			std::cerr << "Bind failed\n";
			close(sockfd);
			exit(EXIT_FAILURE);
		}

		log("Server is now online on port " + std::to_string(port));

		std::thread hb_thread(&Server::monitor_heartbeats, this);
		hb_thread.detach();

		while (running) {
			char buffer[BUFFER_SIZE];
			sockaddr_in client_addr{};
			socklen_t client_len = sizeof(client_addr);
			int n = recvfrom(sockfd, buffer, BUFFER_SIZE, 0,
			                 (struct sockaddr *)&client_addr, &client_len);
			if (n < 0)
				continue;
			buffer[n] = '\0';
			std::string request(buffer);

			log("Received: " + request);

			std::istringstream iss(request);
			std::vector<std::string> tokens;
			std::string token;
			while (iss >> token)
				tokens.push_back(token);
			if (tokens.empty())
				continue;

			std::string req_type = tokens[0];
			std::string username = tokens.size() > 1 ? tokens[1] : "";

			if (req_type == "AUTH" && tokens.size() > 2)
				handle_auth(username, tokens[2], client_addr);
			else if (req_type == "HBT")
				handle_hbt(username, client_addr);
			else if (req_type == "LAP")
				handle_lap(client_addr);
			else if (req_type == "PUB" && tokens.size() > 2)
				handle_pub(username, tokens[2], client_addr);
			else if (req_type == "UNP" && tokens.size() > 2)
				handle_unp(username, tokens[2], client_addr);
			else if (req_type == "SCH" && tokens.size() > 2)
				handle_sch(username, tokens[2], client_addr);
			else if (req_type == "GET" && tokens.size() > 2)
				handle_get(username, tokens[2], client_addr);
			else
				send_response("ERR Invalid command", client_addr);
		}
	}

	void shutdown_server() {
		running = false;
		broadcast("SRV_SHUTDOWN");
		close(sockfd);
	}
};

int main(int argc, char *argv[]) {
	if (argc != 2) {
		std::cerr << "Usage: " << argv[0] << " <port>\n";
		return EXIT_FAILURE;
	}

	int port = std::stoi(argv[1]);
	Server server(port);
	server.load_credentials("credentials.txt");
	server.start();
	return 0;
}
