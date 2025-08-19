import threading
import time
import socket
import sys

# ===================== Global variables =====================
stop_welcome = False
stop_heartbeat = False
welcoming_port_ready = threading.Event()
welcoming_port_number = None
download_threads = []
upload_threads = []

# ===================== Graceful shutdown =====================
def graceful_client_shutdown():
    global stop_welcome, stop_heartbeat
    print("\nStarting client shutdown sequence...")

    stop_welcome = True
    stop_heartbeat = True

    # Unblock welcoming socket if it exists
    if welcoming_port_number:
        try:
            temp_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            temp_socket.connect(('localhost', welcoming_port_number))
            temp_socket.close()
        except:
            pass

    # Safely join threads if they exist
    if 'WELCOME_thread' in globals() and WELCOME_thread.is_alive():
        WELCOME_thread.join()
    if 'HB_thread' in globals() and HB_thread.is_alive():
        HB_thread.join()

    client_socket.close()

    for t in upload_threads:
        t.join()
    for t in download_threads:
        t.join()

    print("Client shutdown complete. Exiting...")
    sys.exit(0)

# ===================== P2P file download =====================
def downloading_sequence(peer_port_number, download_filename, peername):
    download_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    peer_address = ('localhost', int(peer_port_number))
    
    print(f"Connecting to {peername} at {peer_address}...")
    try:
        download_socket.connect(peer_address)
    except socket.error as e:
        print(f"Error connecting to {peername}: {e}")
        download_socket.close()
        return

    print("Connection established!")
    download_socket.sendall(download_filename.encode())

    try:
        with open(download_filename, "wb") as f:
            print(f"Downloading '{download_filename}' from {peername}...")
            while True:
                data = download_socket.recv(1024)
                if not data:
                    break
                f.write(data)
        print(f"'{download_filename}' downloaded successfully!")
    except Exception as e:
        print(f"Error writing file '{download_filename}': {e}")
    finally:
        print(f"Closing P2P connection with {peername}...")
        download_socket.close()

# ===================== P2P file upload =====================
def uploading_sequence(upload_socket, peer_address):
    print("Receiving which file this peer wants...")
    try:
        requested_filename = upload_socket.recv(1024).decode().strip()
        if not requested_filename:
            print("No file requested. Closing connection.")
            upload_socket.close()
            return
    except Exception as e:
        print(f"Error receiving filename: {e}")
        upload_socket.close()
        return

    print(f"Peer wants '{requested_filename}'")
    try:
        with open(requested_filename, "rb") as f:
            while True:
                data = f.read(1024)
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

# ===================== Welcoming socket =====================
def create_welcoming_socket():
    welcoming_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    welcoming_socket.bind(('localhost', 0))
    global welcoming_port_number
    welcoming_port_number = welcoming_socket.getsockname()[1]

    welcoming_port_ready.set()
    print(f"Listening for P2P TCP connections on port {welcoming_port_number}")
    welcoming_socket.listen()

    while not stop_welcome:
        try:
            upload_socket, client_addr = welcoming_socket.accept()
        except socket.error:
            if stop_welcome:
                break
            continue

        print(f"New P2P connection from {client_addr}")
        UPLOAD_thread = threading.Thread(target=uploading_sequence, args=(upload_socket, client_addr))
        UPLOAD_thread.start()
        upload_threads.append(UPLOAD_thread)

    welcoming_socket.close()

# ===================== Heartbeat =====================
def send_heartbeat(client_socket, username):
    heartbeat_code = "HBT"
    welcoming_port_ready.wait()
    heartbeat_msg = f"{heartbeat_code} {username} {welcoming_port_number}"
    while not stop_heartbeat:
        try:
            client_socket.sendto(heartbeat_msg.encode(), server_address)
        except Exception as e:
            print(f"Heartbeat error (server may be down): {e}")
            graceful_client_shutdown()
        time.sleep(2)

# ===================== UDP send & receive =====================
def send_and_receive(request_msg, critical=False):
    try:
        client_socket.sendto(request_msg.encode(), server_address)
        server_response, _ = client_socket.recvfrom(1024)
        server_response = server_response.decode()

        if server_response == "SRV_SHUTDOWN":
            print("\nServer is shutting down. Client will exit now.")
            graceful_client_shutdown()

        return server_response.split(' ')
    except socket.timeout:
        print("No response from server (timeout).")
        if critical:
            graceful_client_shutdown()
        return ["ERR"]
    except Exception as e:
        print(f"Error communicating with server: {e}")
        if critical:
            graceful_client_shutdown()
        return ["ERR"]

# ===================== Command handlers =====================
def handle_get_request(filename):
    request_msg = f"GET {username} {filename}"
    response = send_and_receive(request_msg)
    if response[0] == "OK":
        available_user = response[1]
        address_of_available_user = response[2]
        print(f"User {available_user} has '{filename}' at port {address_of_available_user}")
        DOWNLOAD_thread = threading.Thread(target=downloading_sequence,
                                           args=(address_of_available_user, filename, available_user))
        DOWNLOAD_thread.start()
        download_threads.append(DOWNLOAD_thread)
    else:
        print("No file found")

def handle_lap_request():
    response = send_and_receive(f"LAP {username}")
    if response[0] != "OK":
        print("Failed to get active peers")
        return
    number_of_active_peers = int(response[1]) - 1
    active_peers = response[2:]
    if number_of_active_peers <= 0:
        print("No active peers")
    else:
        print(f"{number_of_active_peers} active peers:")
        for peer in active_peers:
            if peer != username:
                print(peer)

def handle_lpf_request():
    response = send_and_receive(f"LPF {username}")
    if response[0] != "OK":
        print("Failed to get published files")
        return
    number_of_files = int(response[1])
    if number_of_files == 0:
        print("No files published")
    else:
        print(f"{number_of_files} files published:")
        for fname in response[2:]:
            print(fname)

def handle_pub_request(filename):
    response = send_and_receive(f"PUB {username} {filename}")
    if response[0] == "OK":
        print(f"File '{filename}' published successfully")
    else:
        print(f"Failed to publish file '{filename}'")

def handle_unp_request(filename):
    response = send_and_receive(f"UNP {username} {filename}")
    if response[0] == "OK":
        print(f"File '{filename}' unpublished successfully")
    else:
        print(f"Failed to unpublish file '{filename}'")

def handle_sch_request(substring):
    response = send_and_receive(f"SCH {username} {substring}")
    if response[0] != "OK":
        print("Search failed")
        return
    num_files = int(response[1])
    if num_files == 0:
        print("No files found")
    else:
        print(f"{num_files} file(s) found:")
        for fname in response[2:]:
            print(fname)

# ===================== Client setup =====================
server_port = int(sys.argv[1])
server_IP = '127.0.0.1'
server_address = (server_IP, server_port)
client_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
client_socket.settimeout(3)  # 3-second timeout for server responses

# Authentication
while True:
    username = input("Enter username: ")
    password = input("Enter password: ")
    response = send_and_receive(f"AUTH {username} {password}", True)
    if response[0] == "OK":
        print("Welcome to BitTrickle!")
        break
    else:
        print("Authentication failed. Try again.")

# Start threads
WELCOME_thread = threading.Thread(target=create_welcoming_socket)
HB_thread = threading.Thread(target=send_heartbeat, args=(client_socket, username))
WELCOME_thread.start()
HB_thread.start()

print("Client is online. Commands: get, lap, lpf, pub, sch, unp, xit")

# ===================== Main command loop =====================
while True:
    try:
        command = input("> ").strip().split(" ")
        req_type = command[0]
        req_content = command[1] if len(command) > 1 else None
    except EOFError:
        req_type = "xit"

    match req_type:
        case "get":
            if req_content: handle_get_request(req_content)
            else: print("Missing filename")
        case "lap": handle_lap_request()
        case "lpf": handle_lpf_request()
        case "pub":
            if req_content: handle_pub_request(req_content)
            else: print("Missing filename")
        case "sch":
            if req_content: handle_sch_request(req_content)
            else: print("Missing substring")
        case "unp":
            if req_content: handle_unp_request(req_content)
            else: print("Missing filename")
        case "xit":
            graceful_client_shutdown()
        case _:
            print("Unknown command")
