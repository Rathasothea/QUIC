// quic_app.c - Single file QUIC-like client/server application
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <errno.h>
#include <signal.h>

#define MAX_BUFFER_SIZE 4096
#define MAX_MESSAGE_SIZE 1024
#define MAX_CONNECTIONS 100
#define MAX_PENDING_MESSAGES 1000
#define SERVER_PORT 4567

// Protocol structures
typedef enum {
    PACKET_INITIAL = 0x00,
    PACKET_HANDSHAKE = 0x01,
    PACKET_APPLICATION_DATA = 0x02,
    PACKET_CONNECTION_CLOSE = 0x03,
    PACKET_ACK = 0x04
} packet_type_t;

typedef enum {
    FRAME_MESSAGE = 0x0A,
    FRAME_RESPONSE = 0x0B
} frame_type_t;

typedef enum {
    STATE_INITIAL = 0,
    STATE_HANDSHAKE_SENT = 1,
    STATE_ESTABLISHED = 2,
    STATE_CLOSED = 3
} connection_state_t;

#pragma pack(push, 1)
typedef struct {
    packet_type_t type;
    uint32_t connection_id;
    uint32_t packet_number;
    uint16_t payload_length;
} packet_header_t;

typedef struct {
    frame_type_t type;
    uint32_t stream_id;
    uint16_t length;
} stream_frame_t;
#pragma pack(pop)

// Connection structure
typedef struct {
    uint32_t connection_id;
    struct sockaddr_in client_addr;
    connection_state_t state;
    time_t last_activity;
} connection_t;

// Message structure
typedef struct {
    uint32_t id;
    uint32_t connection_id;
    char message[MAX_MESSAGE_SIZE];
    time_t timestamp;
} pending_message_t;

// Global variables
static int running = 1;
static pthread_mutex_t connections_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t messages_mutex = PTHREAD_MUTEX_INITIALIZER;

// Server globals
static connection_t connections[MAX_CONNECTIONS];
static int connection_count = 0;
static uint32_t next_connection_id = 1;
static pending_message_t pending_messages[MAX_PENDING_MESSAGES];
static int message_count = 0;
static uint32_t next_message_id = 1;

// Client globals
static uint32_t client_connection_id = 0;
static connection_state_t client_state = STATE_INITIAL;
static int client_socket = -1;
static struct sockaddr_in server_addr;

// Signal handler
void signal_handler(int sig) {
    printf("\n[INFO] Received signal %d, shutting down...\n", sig);
    running = 0;
}

// Utility functions
void print_timestamp() {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    printf("[%02d:%02d:%02d] ", tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
}

void log_info(const char* format, ...) {
    va_list args;
    print_timestamp();
    printf("INFO: ");
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
}

void log_error(const char* format, ...) {
    va_list args;
    print_timestamp();
    printf("ERROR: ");
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
}

// Server functions
int find_connection(uint32_t connection_id) {
    pthread_mutex_lock(&connections_mutex);
    for (int i = 0; i < connection_count; i++) {
        if (connections[i].connection_id == connection_id) {
            pthread_mutex_unlock(&connections_mutex);
            return i;
        }
    }
    pthread_mutex_unlock(&connections_mutex);
    return -1;
}

int add_connection(struct sockaddr_in client_addr) {
    pthread_mutex_lock(&connections_mutex);
    
    if (connection_count >= MAX_CONNECTIONS) {
        pthread_mutex_unlock(&connections_mutex);
        return -1;
    }
    
    int index = connection_count++;
    connections[index].connection_id = next_connection_id++;
    connections[index].client_addr = client_addr;
    connections[index].state = STATE_ESTABLISHED;
    connections[index].last_activity = time(NULL);
    
    uint32_t conn_id = connections[index].connection_id;
    pthread_mutex_unlock(&connections_mutex);
    
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
    log_info("New connection established: %s:%d (ID: %u)", 
             client_ip, ntohs(client_addr.sin_port), conn_id);
    
    return index;
}

void remove_connection(uint32_t connection_id) {
    pthread_mutex_lock(&connections_mutex);
    
    for (int i = 0; i < connection_count; i++) {
        if (connections[i].connection_id == connection_id) {
            // Shift remaining connections
            for (int j = i; j < connection_count - 1; j++) {
                connections[j] = connections[j + 1];
            }
            connection_count--;
            break;
        }
    }
    
    pthread_mutex_unlock(&connections_mutex);
    log_info("Connection %u removed", connection_id);
}

void add_pending_message(uint32_t connection_id, const char* message) {
    pthread_mutex_lock(&messages_mutex);
    
    if (message_count >= MAX_PENDING_MESSAGES) {
        pthread_mutex_unlock(&messages_mutex);
        log_error("Message queue full");
        return;
    }
    
    int index = message_count++;
    pending_messages[index].id = next_message_id++;
    pending_messages[index].connection_id = connection_id;
    strncpy(pending_messages[index].message, message, MAX_MESSAGE_SIZE - 1);
    pending_messages[index].message[MAX_MESSAGE_SIZE - 1] = '\0';
    pending_messages[index].timestamp = time(NULL);
    
    pthread_mutex_unlock(&messages_mutex);
    
    log_info("Received message from connection %u: \"%s\"", connection_id, message);
}

void send_packet(int socket_fd, struct sockaddr_in* addr, packet_type_t type, 
                uint32_t connection_id, uint32_t packet_number, 
                const char* payload, uint16_t payload_len) {
    char buffer[MAX_BUFFER_SIZE];
    packet_header_t* header = (packet_header_t*)buffer;
    
    header->type = type;
    header->connection_id = connection_id;
    header->packet_number = packet_number;
    header->payload_length = payload_len;
    
    if (payload && payload_len > 0) {
        memcpy(buffer + sizeof(packet_header_t), payload, payload_len);
    }
    
    int total_size = sizeof(packet_header_t) + payload_len;
    
    if (sendto(socket_fd, buffer, total_size, 0, (struct sockaddr*)addr, sizeof(*addr)) < 0) {
        log_error("Failed to send packet: %s", strerror(errno));
    }
}

void send_response(int socket_fd, uint32_t connection_id, const char* message) {
    int conn_index = find_connection(connection_id);
    if (conn_index == -1) {
        log_error("Connection %u not found", connection_id);
        return;
    }
    
    pthread_mutex_lock(&connections_mutex);
    struct sockaddr_in client_addr = connections[conn_index].client_addr;
    pthread_mutex_unlock(&connections_mutex);
    
    // Create response frame
    char payload[MAX_BUFFER_SIZE];
    stream_frame_t* frame = (stream_frame_t*)payload;
    frame->type = FRAME_RESPONSE;
    frame->stream_id = 1;
    frame->length = strlen(message);
    
    strcpy(payload + sizeof(stream_frame_t), message);
    uint16_t total_payload_len = sizeof(stream_frame_t) + strlen(message);
    
    send_packet(socket_fd, &client_addr, PACKET_APPLICATION_DATA, 
               connection_id, 1, payload, total_payload_len);
    
    log_info("Response sent to connection %u: \"%s\"", connection_id, message);
}

void list_connections() {
    pthread_mutex_lock(&connections_mutex);
    
    printf("\nActive connections (%d):\n", connection_count);
    if (connection_count == 0) {
        printf("  No active connections\n");
    } else {
        for (int i = 0; i < connection_count; i++) {
            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &connections[i].client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
            printf("  [%u] %s:%d - State: %d\n", 
                   connections[i].connection_id, 
                   client_ip, 
                   ntohs(connections[i].client_addr.sin_port),
                   connections[i].state);
        }
    }
    
    pthread_mutex_unlock(&connections_mutex);
    printf("\n");
}

void show_pending_messages() {
    pthread_mutex_lock(&messages_mutex);
    
    printf("\nPending messages (%d):\n", message_count);
    if (message_count == 0) {
        printf("  No pending messages\n");
    } else {
        for (int i = 0; i < message_count; i++) {
            time_t age = time(NULL) - pending_messages[i].timestamp;
            printf("  [%d] Connection %u (%lds ago): \"%s\"\n", 
                   i, 
                   pending_messages[i].connection_id, 
                   age,
                   pending_messages[i].message);
        }
    }
    
    pthread_mutex_unlock(&messages_mutex);
    printf("\n");
}

void handle_respond_command(int socket_fd, const char* command) {
    int msg_index;
    char response[MAX_MESSAGE_SIZE];
    
    // Parse command: "respond <msg_index> <message>"
    if (sscanf(command, "respond %d %[^\n]", &msg_index, response) != 2) {
        printf("Usage: respond <msg_index> <message>\n");
        return;
    }
    
    pthread_mutex_lock(&messages_mutex);
    
    if (msg_index < 0 || msg_index >= message_count) {
        printf("Invalid message index. Use 'messages' to see available messages.\n");
        pthread_mutex_unlock(&messages_mutex);
        return;
    }
    
    uint32_t target_connection_id = pending_messages[msg_index].connection_id;
    
    // Remove message from queue
    for (int i = msg_index; i < message_count - 1; i++) {
        pending_messages[i] = pending_messages[i + 1];
    }
    message_count--;
    
    pthread_mutex_unlock(&messages_mutex);
    
    // Send response
    send_response(socket_fd, target_connection_id, response);
}

void* server_command_thread(void* arg) {
    int socket_fd = *(int*)arg;
    char command[MAX_MESSAGE_SIZE];
    
    log_info("Server command interface ready");
    printf("\nServer commands:\n");
    printf("  list - Show active connections\n");
    printf("  messages - Show pending messages\n");
    printf("  respond <msg_index> <message> - Respond to message\n");
    printf("  help - Show this help\n");
    printf("  quit - Stop server\n\n");
    
    while (running) {
        printf("> ");
        fflush(stdout);
        
        if (!fgets(command, sizeof(command), stdin)) {
            break;
        }
        
        // Remove newline
        command[strcspn(command, "\n")] = 0;
        
        if (strlen(command) == 0) {
            continue;
        }
        
        if (strcmp(command, "quit") == 0) {
            running = 0;
            break;
        } else if (strcmp(command, "list") == 0) {
            list_connections();
        } else if (strcmp(command, "messages") == 0) {
            show_pending_messages();
        } else if (strncmp(command, "respond", 7) == 0) {
            handle_respond_command(socket_fd, command);
        } else if (strcmp(command, "help") == 0) {
            printf("\nAvailable commands:\n");
            printf("  list - Show active connections\n");
            printf("  messages - Show pending messages\n");
            printf("  respond <msg_index> <message> - Respond to message\n");
            printf("  help - Show this help\n");
            printf("  quit - Stop server\n\n");
        } else {
            printf("Unknown command: %s. Type 'help' for available commands.\n", command);
        }
    }
    
    return NULL;
}

void handle_server_packet(int socket_fd, char* buffer, int buffer_len, struct sockaddr_in client_addr) {
    if (buffer_len < sizeof(packet_header_t)) {
        log_error("Packet too small: %d bytes", buffer_len);
        return;
    }
    
    packet_header_t* header = (packet_header_t*)buffer;
    char* payload = buffer + sizeof(packet_header_t);
    int payload_len = buffer_len - sizeof(packet_header_t);
    
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
    
    switch (header->type) {
        case PACKET_INITIAL: {
            // New connection request
            int conn_index = add_connection(client_addr);
            if (conn_index >= 0) {
                pthread_mutex_lock(&connections_mutex);
                uint32_t new_conn_id = connections[conn_index].connection_id;
                pthread_mutex_unlock(&connections_mutex);
                
                // Send handshake response
                send_packet(socket_fd, &client_addr, PACKET_HANDSHAKE, new_conn_id, 1, NULL, 0);
            }
            break;
        }
        
        case PACKET_APPLICATION_DATA: {
            // Message from client
            int conn_index = find_connection(header->connection_id);
            if (conn_index >= 0 && payload_len >= sizeof(stream_frame_t)) {
                stream_frame_t* frame = (stream_frame_t*)payload;
                char* message_data = payload + sizeof(stream_frame_t);
                int message_len = payload_len - sizeof(stream_frame_t);
                
                if (frame->type == FRAME_MESSAGE && message_len > 0) {
                    char message[MAX_MESSAGE_SIZE];
                    int copy_len = (message_len < MAX_MESSAGE_SIZE - 1) ? message_len : MAX_MESSAGE_SIZE - 1;
                    memcpy(message, message_data, copy_len);
                    message[copy_len] = '\0';
                    
                    add_pending_message(header->connection_id, message);
                    
                    // Send ACK
                    send_packet(socket_fd, &client_addr, PACKET_ACK, header->connection_id, header->packet_number, NULL, 0);
                }
            }
            break;
        }
        
        case PACKET_CONNECTION_CLOSE: {
            // Client disconnecting
            remove_connection(header->connection_id);
            break;
        }
        
        default:
            log_info("Unhandled packet type: %d from %s:%d", 
                    header->type, client_ip, ntohs(client_addr.sin_port));
            break;
    }
}

int run_server() {
    int socket_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    char buffer[MAX_BUFFER_SIZE];
    
    // Create socket
    socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        log_error("Failed to create socket: %s", strerror(errno));
        return -1;
    }
    
    // Set socket options
    int opt = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Set socket timeout
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    
    // Bind socket
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SERVER_PORT);
    
    if (bind(socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        log_error("Failed to bind socket: %s", strerror(errno));
        close(socket_fd);
        return -1;
    }
    
    log_info("QUIC Server started on port %d", SERVER_PORT);
    
    // Start command thread
    pthread_t command_thread;
    pthread_create(&command_thread, NULL, server_command_thread, &socket_fd);
    
    // Main server loop
    while (running) {
        int bytes_received = recvfrom(socket_fd, buffer, sizeof(buffer), 0,
                                    (struct sockaddr*)&client_addr, &client_len);
        
        if (bytes_received > 0) {
            handle_server_packet(socket_fd, buffer, bytes_received, client_addr);
        } else if (bytes_received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            if (running) {
                log_error("recvfrom error: %s", strerror(errno));
            }
        }
    }
    
    pthread_join(command_thread, NULL);
    close(socket_fd);
    log_info("Server stopped");
    return 0;
}

// Client functions
void handle_client_packet(char* buffer, int buffer_len) {
    if (buffer_len < sizeof(packet_header_t)) {
        return;
    }
    
    packet_header_t* header = (packet_header_t*)buffer;
    char* payload = buffer + sizeof(packet_header_t);
    int payload_len = buffer_len - sizeof(packet_header_t);
    
    switch (header->type) {
        case PACKET_HANDSHAKE:
            if (client_state == STATE_HANDSHAKE_SENT) {
                client_connection_id = header->connection_id;
                client_state = STATE_ESTABLISHED;
                log_info("Handshake complete, connection ID: %u", client_connection_id);
            }
            break;
            
        case PACKET_APPLICATION_DATA:
            if (payload_len >= sizeof(stream_frame_t)) {
                stream_frame_t* frame = (stream_frame_t*)payload;
                char* message_data = payload + sizeof(stream_frame_t);
                int message_len = payload_len - sizeof(stream_frame_t);
                
                if (frame->type == FRAME_RESPONSE && message_len > 0) {
                    char response[MAX_MESSAGE_SIZE];
                    int copy_len = (message_len < MAX_MESSAGE_SIZE - 1) ? message_len : MAX_MESSAGE_SIZE - 1;
                    memcpy(response, message_data, copy_len);
                    response[copy_len] = '\0';
                    
                    printf("\n*** Server Response: \"%s\" ***\n", response);
                    printf("Enter message (or 'quit' to exit): ");
                    fflush(stdout);
                }
            }
            break;
            
        case PACKET_ACK:
            // Message acknowledged
            break;
            
        default:
            break;
    }
}

void* client_receive_thread(void* arg) {
    char buffer[MAX_BUFFER_SIZE];
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    
    while (running && client_state != STATE_CLOSED) {
        int bytes_received = recvfrom(client_socket, buffer, sizeof(buffer), 0,
                                    (struct sockaddr*)&from_addr, &from_len);
        
        if (bytes_received > 0) {
            handle_client_packet(buffer, bytes_received);
        } else if (bytes_received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            if (running) {
                log_error("Client receive error: %s", strerror(errno));
            }
            break;
        }
    }
    
    return NULL;
}

int client_send_message(const char* message) {
    if (client_state != STATE_ESTABLISHED) {
        log_error("Not connected to server");
        return -1;
    }
    
    // Create message frame
    char payload[MAX_BUFFER_SIZE];
    stream_frame_t* frame = (stream_frame_t*)payload;
    frame->type = FRAME_MESSAGE;
    frame->stream_id = 1;
    frame->length = strlen(message);
    
    strcpy(payload + sizeof(stream_frame_t), message);
    uint16_t total_payload_len = sizeof(stream_frame_t) + strlen(message);
    
    send_packet(client_socket, &server_addr, PACKET_APPLICATION_DATA,
               client_connection_id, 1, payload, total_payload_len);
    
    log_info("Message sent: \"%s\"", message);
    return 0;
}

int run_client(const char* server_ip) {
    pthread_t receive_thread;
    char message[MAX_MESSAGE_SIZE];
    
    // Create socket
    client_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (client_socket < 0) {
        log_error("Failed to create socket: %s", strerror(errno));
        return -1;
    }
    
    // Set socket timeout
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    
    // Setup server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        log_error("Invalid server IP address: %s", server_ip);
        close(client_socket);
        return -1;
    }
    
    log_info("Connecting to %s:%d...", server_ip, SERVER_PORT);
    
    // Send initial packet
    send_packet(client_socket, &server_addr, PACKET_INITIAL, 0, 1, NULL, 0);
    client_state = STATE_HANDSHAKE_SENT;
    
    // Start receive thread
    pthread_create(&receive_thread, NULL, client_receive_thread, NULL);
    
    // Wait for connection
    log_info("Waiting for handshake...");
    for (int i = 0; i < 100 && client_state != STATE_ESTABLISHED; i++) {
        usleep(100000); // 100ms
    }
    
    if (client_state != STATE_ESTABLISHED) {
        log_error("Handshake timeout - server not responding");
        running = 0;
        pthread_join(receive_thread, NULL);
        close(client_socket);
        return -1;
    }
    
    log_info("Successfully connected to %s:%d", server_ip, SERVER_PORT);
    printf("\nConnected! Type messages (or 'quit' to exit):\n");
    
    // Chat loop
    while (running) {
        printf("Enter message (or 'quit' to exit): ");
        fflush(stdout);
        
        if (!fgets(message, sizeof(message), stdin)) {
            break;
        }
        
        // Remove newline
        message[strcspn(message, "\n")] = 0;
        
        if (strlen(message) == 0) {
            continue;
        }
        
        if (strcmp(message, "quit") == 0) {
            break;
        }
        
        if (client_send_message(message) < 0) {
            break;
        }
    }
    
    // Send disconnect
    if (client_connection_id != 0) {
        send_packet(client_socket, &server_addr, PACKET_CONNECTION_CLOSE, client_connection_id, 1, NULL, 0);
    }
    
    client_state = STATE_CLOSED;
    running = 0;
    pthread_join(receive_thread, NULL);
    close(client_socket);
    log_info("Disconnected from server");
    
    return 0;
}

void show_usage(const char* program_name) {
    printf("Usage:\n");
    printf("  %s server                 - Run as server\n", program_name);
    printf("  %s client [server_ip]     - Run as client (default: 127.0.0.1)\n", program_name);
    printf("\nExamples:\n");
    printf("  %s server\n", program_name);
    printf("  %s client\n", program_name);
    printf("  %s client 192.168.1.100\n", program_name);
}

int main(int argc, char* argv[]) {
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    if (argc < 2) {
        show_usage(argv[0]);
        return 1;
    }
    
    if (strcmp(argv[1], "server") == 0) {
        return run_server();
    } else if (strcmp(argv[1], "client") == 0) {
        const char* server_ip = (argc > 2) ? argv[2] : "127.0.0.1";
        return run_client(server_ip);
    } else {
        printf("Invalid command: %s\n\n", argv[1]);
        show_usage(argv[0]);
        return 1;
    }
}