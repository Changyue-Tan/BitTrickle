import socket
import sys
import datetime
import time

# Load credentials from file into a dictionary
def load_credentials(credentials_file):
    credentials = {}
    with open(credentials_file, 'r') as f:
        for line in f:
            username, password = line.strip().split(' ')
            credentials[username] = password
    return credentials

# Logging functions
def display_msg_received(client_port, request_type, username):
    current_time = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
    print(f"{current_time}: {client_port}: Received {request_type} from {username}")

def display_msg_sent(client_port, response_type, username):
    current_time = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
    print(f"{current_time}: {client_port}: Sent {response_type} to {username}")

# Check if a user is active
def check_active(username):
    last_heartbeat_time = heartbeats_record.get(username, 0)
    if username in active_clients and (time.time() - last_heartbeat_time) > 3:
        active_clients.remove(username)
        return False
    return username in active_clients

# Validate user credentials
def check_credentials(username, password):
    return credentials.get(username) == password

# Receive request helper
def receive_request(client_request):
    if len(client_request) < 2:
        return None, None  # Invalid request
    return client_request[0], client_request[1]

# Send response helper
def send_response(client_port, response_type, username, response_content=""):
    response = f"{response_type} {response_content}"
    server_socket.sendto(response.encode(), client_address)
    display_msg_sent(client_port, response_type, username)

# Handle AUTH request
def handle_AUTH():
    request_type, username = receive_request(client_request)
    display_msg_received(client_port, request_type, username)

    if len(client_request) < 3:
        send_response(client_port, "ERR", username)
        return

    password = client_request[2]
    response_type = "ERR"

    if not check_active(username) and check_credentials(username, password):
        response_type = "OK"
        active_clients.add(username)
        heartbeats_record[username] = time.time()  # Authentication acts as a heartbeat

    send_response(client_port, response_type, username)

# Handle HBT (Heartbeat)
def handle_HBT():
    request_type, username = receive_request(client_request)
    display_msg_received(client_port, request_type, username)

    if len(client_request) < 3:
        send_response(client_port, "ERR", username)
        return

    heartbeats_record[username] = time.time()
    contact_book[username] = client_request[2]  # Welcoming port number

# Handle LAP (List Active Peers)
def handle_LAP():
    request_type, username = receive_request(client_request)
    display_msg_received(client_port, request_type, username)

    # Refresh active_clients set
    for user in list(active_clients):
        check_active(user)

    response_content = f"{len(active_clients)} " + " ".join(active_clients)
    send_response(client_port, "OK", username, response_content)

# Handle LPF (List Published Files)
def handle_LPF():
    request_type, username = receive_request(client_request)
    display_msg_received(client_port, request_type, username)

    files = file_publishing_users.get(username, set())
    response_content = f"{len(files)} " + " ".join(files)
    send_response(client_port, "OK", username, response_content)

# Handle PUB (Publish File)
def handle_PUB():
    request_type, username = receive_request(client_request)
    display_msg_received(client_port, request_type, username)

    if len(client_request) < 3:
        send_response(client_port, "ERR", username)
        return

    filename = client_request[2]
    published_files.setdefault(filename, set()).add(username)
    file_publishing_users.setdefault(username, set()).add(filename)

    send_response(client_port, "OK", username)

# Handle UNP (Unpublish File)
def handle_UNP():
    request_type, username = receive_request(client_request)
    display_msg_received(client_port, request_type, username)

    if len(client_request) < 3:
        send_response(client_port, "ERR", username)
        return

    filename = client_request[2]
    if filename in file_publishing_users.get(username, set()):
        file_publishing_users[username].remove(filename)
        published_files[filename].discard(username)
        send_response(client_port, "OK", username)
    else:
        send_response(client_port, "ERR", username)

# Handle SCH (Search File)
def handle_SCH():
    request_type, username = receive_request(client_request)
    display_msg_received(client_port, request_type, username)

    if len(client_request) < 3:
        send_response(client_port, "ERR", username)
        return

    substring = client_request[2]
    found_files = {file for user in active_clients if user != username for file in file_publishing_users.get(user, set()) if substring in file}
    
    response_content = f"{len(found_files)} " + " ".join(found_files)
    send_response(client_port, "OK", username, response_content)

# Handle GET (Retrieve File Owner)
def handle_GET():
    request_type, username = receive_request(client_request)
    display_msg_received(client_port, request_type, username)

    if len(client_request) < 3:
        send_response(client_port, "ERR", username)
        return

    filename = client_request[2]
    available_users = published_files.get(filename, set())

    for user in available_users:
        if user in active_clients and user != username:
            send_response(client_port, "OK", username, f"{user} {contact_book.get(user, '')}")
            return

    send_response(client_port, "ERR", username)

# Server setup
server_port = int(sys.argv[1])
server_IP = '127.0.0.1'
server_address = (server_IP, server_port)
server_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
server_socket.bind(server_address)

credentials = load_credentials('credentials.txt')                       # credentials dict for easier authentication
active_clients = set()                                                  # a set for unique usernames
heartbeats_record = {username: 0 for username in credentials}           # last heartbeat time for each user, initialised to be 0
published_files = {}                                                    # <filename: {set of clients with this file avaliable}>
file_publishing_users = {username: set() for username in credentials}   # <username: {set of files published by this user}>
contact_book = {}                                                       # <username(could be offline): (last known) welcoming port number>

print("Server is now online")
while True:
    for user in list(active_clients):
        check_active(user)

    client_request, client_address = server_socket.recvfrom(1024)
    client_request = client_request.decode().split(' ')
    client_port = client_address[1]

    if len(client_request) < 2:
        continue  # Skip malformed requests

    request_type = client_request[0]
    username = client_request[1]

    handlers = {
        "AUTH": handle_AUTH,
        "HBT": handle_HBT,
        "LAP": handle_LAP,
        "LPF": handle_LPF,
        "PUB": handle_PUB,
        "UNP": handle_UNP,
        "SCH": handle_SCH,
        "GET": handle_GET,
    }

    handler = handlers.get(request_type)
    if handler:
        handler()
    else:
        send_response(client_port, "ERR", username, "Invalid request")
