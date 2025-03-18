import threading
import time
import socket
import sys

# Global variables
stop_welcome = False
stop_heartbeat = False
welcoming_port_ready = threading.Event()
welcoming_port_number = None
download_threads = []  
upload_threads = []  

# Downloading Sequence: runs as a thread
def downloading_sequence(peer_port_number, download_filename, peername):
    download_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    peer_address = ('localhost', int(peer_port_number))
    
    print(f"Connecting to {peername} at {peer_address}")
    try:
        download_socket.connect(peer_address)
    except socket.error as e:
        print(f"Error connecting to {peername}: {e}")
        download_socket.close()
        return

    print("Connection established!")
    # Fixed: use download_filename instead of filename
    print(f"Telling peer which file is wanted: '{download_filename}'...")
    download_socket.sendall(download_filename.encode())

    try:
        with open(download_filename, "wb") as f:
            print(f"Downloading '{download_filename}' from {peername}...")
            while True:
                data = download_socket.recv(1024)  # Receive in 1KB chunks
                if not data:
                    break
                f.write(data)
        print(f"'{download_filename}' downloaded successfully!")
    except Exception as e:
        print(f"Error writing file '{download_filename}': {e}")
    finally:
        print(f"Closing P2P connection with {peername}...")
        download_socket.close()

# Uploading Sequence: runs as a thread
def uploading_sequence(upload_socket, peer_address):
    print("Receiving which file this peer wants...")
    try:
        requested_filename = upload_socket.recv(1024).decode().strip()
    except Exception as e:
        print(f"Error receiving filename: {e}")
        upload_socket.close()
        return

    print(f"Peer wants '{requested_filename}'")
    print("Sending data...")
    try:
        with open(requested_filename, "rb") as f:
            while True:
                data = f.read(1024)  # Send file in 1KB chunks
                if not data:
                    break
                upload_socket.send(data)
        print(f"'{requested_filename}' uploaded successfully!")
    except FileNotFoundError:
        print(f"Error: File '{requested_filename}' not found!")
        upload_socket.send("ERROR: File not found".encode())
    except Exception as e:
        print(f"Error during upload: {e}")
    finally:
        print(f"Closing P2P connection with {peer_address}...")
        upload_socket.close()

# Welcoming socket: listens for incoming TCP connections from peers
def create_welcoming_socket():
    welcoming_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    welcoming_socket.bind(('localhost', 0))  # OS chooses available port
    global welcoming_port_number 
    welcoming_port_number = welcoming_socket.getsockname()[1]  

    welcoming_port_ready.set()
    print(f"Listening for P2P TCP connection requests on port: {welcoming_port_number}")
    welcoming_socket.listen()

    while not stop_welcome:
        try:
            upload_socket, client_addr = welcoming_socket.accept()
        except socket.error as e:
            if stop_welcome:
                break
            print(f"Error accepting connection: {e}")
            continue

        print(f"New P2P connection from {client_addr}")
        if not stop_welcome:
            print(f"Starting file uploading sequence to {client_addr}...")
            UPLOAD_thread = threading.Thread(target=uploading_sequence, args=(upload_socket, client_addr))
            UPLOAD_thread.start()
            upload_threads.append(UPLOAD_thread)
        else:
            print("stop_welcome flag set; not processing new connection.")

    print("Closing welcoming socket...")
    welcoming_socket.close()

# Heartbeat function: sends a heartbeat message to the server periodically
def send_heartbeat(client_socket, username):
    heartbeat_code = "HBT"
    welcoming_port_ready.wait()  # Wait until the welcoming socket is ready
    heartbeat_msg = f"{heartbeat_code} {username} {welcoming_port_number}"
    while not stop_heartbeat:
        try:
            client_socket.sendto(heartbeat_msg.encode(), server_address)
        except Exception as e:
            print(f"Error sending heartbeat: {e}")
        time.sleep(2)

# Helper: send a message to the server and receive its response (UDP)
def send_and_receive(request_msg):
    client_socket.sendto(request_msg.encode(), server_address)
    server_response, _ = client_socket.recvfrom(1024)
    return server_response.decode().split(' ')

# Command handlers
def handle_get_request(filename):
    request_code = "GET"
    request_msg = f"{request_code} {username} {filename}"
    server_response = send_and_receive(request_msg)

    if server_response[0] == "OK":
        available_user = server_response[1]
        address_of_available_user = server_response[2]  # Port number
        print(f"User {available_user} has '{filename}'")
        print(f"{available_user} is online with welcoming port: {address_of_available_user}")    
        print(f"Starting file downloading sequence from {available_user}...")
        DOWNLOAD_thread = threading.Thread(target=downloading_sequence, args=(address_of_available_user, filename, available_user))
        DOWNLOAD_thread.start()
        download_threads.append(DOWNLOAD_thread)
    else:
        print("No file found")

def handle_lap_request():
    request_code = "LAP"
    request_msg = f"{request_code} {username}"
    server_response = send_and_receive(request_msg)
    
    number_of_active_peers = int(server_response[1]) - 1  # Exclude yourself
    active_peers = server_response[2:]
    if number_of_active_peers == 0: 
        print("No active peers")
        return
    elif number_of_active_peers == 1:
        print("1 active peer")
    else:
        print(f"{number_of_active_peers} active peers")
    
    for peer in active_peers:
        if peer != username:
            print(peer)

def handle_lpf_request():
    request_code = "LPF"
    request_msg = f"{request_code} {username}"
    server_response = send_and_receive(request_msg)

    number_of_files_published = int(server_response[1])
    if number_of_files_published == 0:
        print("No files published")
        return
    elif number_of_files_published == 1:
        print("1 file published:")
    else:
        print(f"{number_of_files_published} files published:")

    for file_name in server_response[2:]:
        print(file_name)

def handle_pub_request(filename):
    request_code = "PUB"
    request_msg = f"{request_code} {username} {filename}"
    server_response = send_and_receive(request_msg)
    if server_response[0] == "OK":
        print(f"File '{filename}' has been published successfully!")
    else:
        print(f"Failed to publish file '{filename}'")

def handle_unp_request(filename):
    request_code = "UNP"
    request_msg = f"{request_code} {username} {filename}"
    server_response = send_and_receive(request_msg)
    if server_response[0] == "OK":
        print("File unpublished successfully")
    else:
        print("File unpublication failed")

def handle_sch_request(substring):
    request_code = "SCH"
    request_msg = f"{request_code} {username} {substring}"
    server_response = send_and_receive(request_msg)
    
    if server_response[0] == "OK":
        number_files_found = server_response[1]
        if number_files_found == '0':
            print("File not found")
            return
        elif number_files_found == '1':
            print("1 file found:")
        else:
            print(f"{number_files_found} files found:")
        
        for fname in server_response[2:]:
            print(fname)
    else:
        print("Search failed")

# Create UDP socket to communicate with the server
server_port = int(sys.argv[1])
server_IP = '127.0.0.1'
server_address = (server_IP, server_port)
client_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

# Prompt for authentication
while True:
    auth_code = "AUTH"
    username = input("Enter username: ")
    password = input("Enter password: ")
    client_request = f"{auth_code} {username} {password}"
    client_socket.sendto(client_request.encode(), server_address)
    server_response, _ = client_socket.recvfrom(1024)
    server_response = server_response.decode().split(' ')
    response_type = server_response[0]
    if response_type == "OK":
        print("Welcome to BitTrickle!")
        print("Starting client booting sequence...")
        break
    else:
        print("Authentication failed. Please try again.")

# Start TCP welcoming thread and heartbeat thread
WELCOME_thread = threading.Thread(target=create_welcoming_socket)
HB_thread = threading.Thread(target=send_heartbeat, args=(client_socket, username))

print("Starting welcoming thread...")
WELCOME_thread.start()
print("Starting heartbeat thread...")
HB_thread.start() # welcoming port number will be sent via heart beat

print("Client is now online")
print("Available commands are: get, lap, lpf, pub, sch, unp, xit")

# Main command loop
while True:
    command = input("> ").strip().split(" ")
    req_type = command[0]
    req_content = command[1] if len(command) > 1 else None

    match req_type:
        case "get":
            if req_content is None:
                print("Missing filename")
            else:
                handle_get_request(req_content)
        case "lap":
            handle_lap_request()
        case "lpf":
            handle_lpf_request()
        case "pub":
            if req_content is None:
                print("Missing filename")
            else:
                handle_pub_request(req_content)
        case "sch":
            if req_content is None:
                print("Missing substring")
            else:
                handle_sch_request(req_content)
        case "unp":
            if req_content is None:
                print("Missing filename")
            else:
                handle_unp_request(req_content)
        case "xit":
            print("Starting client shutdown sequence...")
            print("A pseudo request will be sent to the welcoming socket to unblock accept()")
            break
        case _:
            print("Unknown command. Please try again.")

# Signal shutdown to threads
stop_welcome = True

# Unblock welcoming socket by opening a temporary connection
temp_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
try:
    temp_socket.connect(('localhost', welcoming_port_number))
    temp_socket.close()
except Exception as e:
    print(f"Error unblocking welcoming socket: {e}")

print("Waiting for welcoming thread to finish...")
WELCOME_thread.join()
print("Welcoming thread finished")

stop_heartbeat = True
print("Waiting for heartbeat thread to finish...")
HB_thread.join()
print("Heartbeat thread finished")

print("Closing UDP socket with server...")
client_socket.close()

print("Waiting for P2P upload threads to finish...")
for t in upload_threads:
    t.join()

print("Waiting for P2P download threads to finish...")
for t in download_threads:
    t.join()

print("All threads joined and client is now offline")
