#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "msquic.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include <msquic.h>

#ifndef UNREFERENCED_PARAMETER
#define UNREFERENCED_PARAMETER(P) (void)(P)
#endif

// Global variables
const QUIC_API_TABLE* MsQuic = NULL;
HQUIC Registration = NULL;
HQUIC Configuration = NULL;
HQUIC Listener = NULL;
volatile int ServerRunning = 1;

// Server settings
#define SERVER_PORT 4567
static const char ALPN[] = "sample-app";

// Client connection management
typedef struct {
    HQUIC Connection;
    int Connected;
} ClientConnection;

ClientConnection ActiveClient = { NULL, 0 };
pthread_mutex_t clientMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t outputMutex = PTHREAD_MUTEX_INITIALIZER;

// Debug logging for Wireshark
FILE* keylog_file = NULL;

// Function declarations
void PrintStatus(const char* message);
void PrintError(const char* message);
void InitializeKeyLogging();
QUIC_STATUS ServerListenerCallback(HQUIC Listener, void* Context, QUIC_LISTENER_EVENT* Event);
QUIC_STATUS ServerConnectionCallback(HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event);
QUIC_STATUS ServerStreamCallback(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event);
void SignalHandler(int signum);
void CleanupServer();
int GenerateSelfSignedCertificate();
int SendMessageToClient(const char* message);
void* ServerInputThread(void* arg);

void PrintStatus(const char* message) {
    pthread_mutex_lock(&outputMutex);
    printf("[INFO] %s\n", message);
    pthread_mutex_unlock(&outputMutex);
}

void PrintError(const char* message) {
    pthread_mutex_lock(&outputMutex);
    printf("[ERROR] %s\n", message);
    pthread_mutex_unlock(&outputMutex);
}

void InitializeKeyLogging() {
    // Enable QUIC key logging for Wireshark decryption
    const char* keylog_path = getenv("SSLKEYLOGFILE");
    if (!keylog_path) {
        keylog_path = "quic_keys.log";
        // Set environment variable for this process
        setenv("SSLKEYLOGFILE", keylog_path, 1);
    }
    
    keylog_file = fopen(keylog_path, "w");
    if (keylog_file) {
        PrintStatus("QUIC key logging enabled for Wireshark decryption");
        printf("[DEBUG] Key log file: %s\n", keylog_path);
    } else {
        PrintError("Failed to open key log file for Wireshark");
    }
}

void SignalHandler(int signum) {
    UNREFERENCED_PARAMETER(signum);
    PrintStatus("Received signal to shutdown server...");
    ServerRunning = 0;
}

int GenerateSelfSignedCertificate() {
    PrintStatus("Generating self-signed certificate...");
    int result = system("openssl req -x509 -newkey rsa:2048 -keyout server.key -out server.crt -days 365 -nodes -subj \"/CN=localhost\" 2>/dev/null");
    return result == 0;
}

// Fixed SendMessageToClient with proper memory management
int SendMessageToClient(const char* message) {
    pthread_mutex_lock(&clientMutex);
    
    if (!ActiveClient.Connected || !ActiveClient.Connection) {
        pthread_mutex_unlock(&clientMutex);
        PrintError("No client connected to send message");
        return 0;
    }
    
    HQUIC Connection = ActiveClient.Connection;
    pthread_mutex_unlock(&clientMutex);
    
    printf("[DEBUG] Creating stream to send message: %s\n", message);
    
    // Create a new stream for sending the message
    HQUIC Stream = NULL;
    QUIC_STATUS status = MsQuic->StreamOpen(Connection, QUIC_STREAM_OPEN_FLAG_NONE, ServerStreamCallback, NULL, &Stream);
    
    if (QUIC_FAILED(status)) {
        printf("[ERROR] Failed to open stream: 0x%x\n", status);
        return 0;
    }
    
    // Start the stream
    status = MsQuic->StreamStart(Stream, QUIC_STREAM_START_FLAG_NONE);
    if (QUIC_FAILED(status)) {
        printf("[ERROR] Failed to start stream: 0x%x\n", status);
        MsQuic->StreamClose(Stream);
        return 0;
    }
    
    // Allocate memory for the message buffer (will be freed in callback)
    size_t messageLen = strlen(message);
    QUIC_BUFFER* SendBuffer = (QUIC_BUFFER*)malloc(sizeof(QUIC_BUFFER));
    if (!SendBuffer) {
        PrintError("Failed to allocate send buffer");
        MsQuic->StreamClose(Stream);
        return 0;
    }
    
    SendBuffer->Length = messageLen;
    SendBuffer->Buffer = (uint8_t*)malloc(messageLen);
    if (!SendBuffer->Buffer) {
        PrintError("Failed to allocate buffer data");
        free(SendBuffer);
        MsQuic->StreamClose(Stream);
        return 0;
    }
    
    memcpy(SendBuffer->Buffer, message, messageLen);
    
    printf("[DEBUG] Sending %zu bytes via QUIC stream\n", messageLen);
    
    // Send with FIN flag to properly close the stream
    status = MsQuic->StreamSend(Stream, SendBuffer, 1, QUIC_SEND_FLAG_FIN, SendBuffer);
    
    if (QUIC_FAILED(status)) {
        printf("[ERROR] Failed to send message: 0x%x\n", status);
        free(SendBuffer->Buffer);
        free(SendBuffer);
        MsQuic->StreamClose(Stream);
        return 0;
    }
    
    pthread_mutex_lock(&outputMutex);
    printf("[SERVER -> CLIENT] %s\n", message);
    printf("[SERVER] > ");
    fflush(stdout);
    pthread_mutex_unlock(&outputMutex);
    
    return 1;
}

void* ServerInputThread(void* arg) {
    UNREFERENCED_PARAMETER(arg);
    
    char input[1024];
    printf("\n=== Server Chat Ready ===\n");
    printf("Type messages to send to client (type '/quit' to stop server):\n");
    printf("Wireshark tip: Filter with 'quic and udp.port == %d'\n", SERVER_PORT);
    printf("[SERVER] > ");
    fflush(stdout);
    
    while (ServerRunning) {
        if (!fgets(input, sizeof(input), stdin)) {
            break;
        }
        
        input[strcspn(input, "\n")] = 0;
        
        if (strlen(input) == 0) {
            printf("[SERVER] > ");
            fflush(stdout);
            continue;
        }
        
        if (strcmp(input, "/quit") == 0) {
            ServerRunning = 0;
            break;
        }
        
        if (!SendMessageToClient(input)) {
            PrintError("Failed to send message to client");
        }
        
        usleep(100000);
    }
    
    return NULL;
}

QUIC_STATUS ServerListenerCallback(HQUIC Listener, void* Context, QUIC_LISTENER_EVENT* Event) {
    UNREFERENCED_PARAMETER(Listener);
    UNREFERENCED_PARAMETER(Context);
    
    switch (Event->Type) {
        case QUIC_LISTENER_EVENT_NEW_CONNECTION:
            printf("[DEBUG] New QUIC connection attempt received\n");
            PrintStatus("New client connection received!");
            MsQuic->SetCallbackHandler(Event->NEW_CONNECTION.Connection, (void*)ServerConnectionCallback, NULL);
            return MsQuic->ConnectionSetConfiguration(Event->NEW_CONNECTION.Connection, Configuration);
            
        case QUIC_LISTENER_EVENT_STOP_COMPLETE:
            PrintStatus("Listener stopped successfully");
            return QUIC_STATUS_SUCCESS;
            
        default:
            return QUIC_STATUS_NOT_SUPPORTED;
    }
}

QUIC_STATUS ServerConnectionCallback(HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event) {
    UNREFERENCED_PARAMETER(Context);
    
    switch (Event->Type) {
        case QUIC_CONNECTION_EVENT_CONNECTED:
            printf("[DEBUG] QUIC handshake completed successfully\n");
            PrintStatus("Client successfully connected!");
            pthread_mutex_lock(&clientMutex);
            ActiveClient.Connection = Connection;
            ActiveClient.Connected = 1;
            pthread_mutex_unlock(&clientMutex);
            return QUIC_STATUS_SUCCESS;
            
        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
            printf("[DEBUG] QUIC connection shutdown initiated\n");
            PrintStatus("Connection shutdown initiated");
            pthread_mutex_lock(&clientMutex);
            ActiveClient.Connected = 0;
            ActiveClient.Connection = NULL;
            pthread_mutex_unlock(&clientMutex);
            return QUIC_STATUS_SUCCESS;
            
        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            printf("[DEBUG] QUIC connection completely closed\n");
            PrintStatus("Connection shutdown complete");
            pthread_mutex_lock(&clientMutex);
            ActiveClient.Connected = 0;
            ActiveClient.Connection = NULL;
            pthread_mutex_unlock(&clientMutex);
            MsQuic->ConnectionClose(Connection);
            return QUIC_STATUS_SUCCESS;
            
        case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
            printf("[DEBUG] Client initiated new QUIC stream\n");
            PrintStatus("Client started a new stream");
            MsQuic->SetCallbackHandler(Event->PEER_STREAM_STARTED.Stream, (void*)ServerStreamCallback, NULL);
            return QUIC_STATUS_SUCCESS;
            
        default:
            return QUIC_STATUS_SUCCESS;
    }
}

// Fixed ServerStreamCallback with proper memory management
QUIC_STATUS ServerStreamCallback(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event) {
    UNREFERENCED_PARAMETER(Context);
    
    switch (Event->Type) {
        case QUIC_STREAM_EVENT_SEND_COMPLETE:
            printf("[DEBUG] Stream send completed\n");
            // Clean up allocated send buffer
            if (Event->SEND_COMPLETE.ClientContext) {
                QUIC_BUFFER* Buffer = (QUIC_BUFFER*)Event->SEND_COMPLETE.ClientContext;
                if (Buffer->Buffer) {
                    free(Buffer->Buffer);
                }
                free(Buffer);
            }
            return QUIC_STATUS_SUCCESS;
            
        case QUIC_STREAM_EVENT_RECEIVE:
            {
                // Process received message from client
                printf("[DEBUG] Received data on QUIC stream\n");
                
                pthread_mutex_lock(&outputMutex);
                printf("\n[CLIENT -> SERVER] ");
                
                for (uint32_t i = 0; i < Event->RECEIVE.BufferCount; i++) {
                    printf("%.*s", (int)Event->RECEIVE.Buffers[i].Length, 
                           Event->RECEIVE.Buffers[i].Buffer);
                }
                
                printf("\n[SERVER] > ");
                fflush(stdout);
                pthread_mutex_unlock(&outputMutex);
                
                return QUIC_STATUS_SUCCESS;
            }
            
        case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
            printf("[DEBUG] Peer closed send direction of stream\n");
            return QUIC_STATUS_SUCCESS;
            
        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            printf("[DEBUG] Stream completely shut down\n");
            MsQuic->StreamClose(Stream);
            return QUIC_STATUS_SUCCESS;
            
        default:
            return QUIC_STATUS_SUCCESS;
    }
}

void CleanupServer() {
    if (keylog_file) {
        fclose(keylog_file);
        keylog_file = NULL;
    }
    
    if (Listener) {
        MsQuic->ListenerClose(Listener);
        Listener = NULL;
    }
    if (Configuration) {
        MsQuic->ConfigurationClose(Configuration);
        Configuration = NULL;
    }
    if (Registration) {
        MsQuic->RegistrationClose(Registration);
        Registration = NULL;
    }
    if (MsQuic) {
        MsQuicClose(MsQuic);
        MsQuic = NULL;
    }
    remove("server.key");
    remove("server.crt");
    PrintStatus("Server cleanup completed");
}

int main() {
    printf("=== QUIC Chat Server with Wireshark Debug ===\n");
    PrintStatus("Starting MSQUIC Chat Server...");
    
    // Initialize key logging for Wireshark
    InitializeKeyLogging();
    
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    
    if (!GenerateSelfSignedCertificate()) {
        PrintError("Failed to generate certificate");
        return 1;
    }
    
    if (QUIC_FAILED(MsQuicOpen2(&MsQuic))) {
        PrintError("Failed to open MsQuic");
        return 1;
    }
    
    const QUIC_REGISTRATION_CONFIG RegConfig = { "quic-server", QUIC_EXECUTION_PROFILE_LOW_LATENCY };
    if (QUIC_FAILED(MsQuic->RegistrationOpen(&RegConfig, &Registration))) {
        PrintError("Failed to open registration");
        CleanupServer();
        return 1;
    }
    
    QUIC_SETTINGS Settings = {0};
    Settings.IdleTimeoutMs = 60000;
    Settings.IsSet.IdleTimeoutMs = TRUE;
    Settings.PeerBidiStreamCount = 10;
    Settings.IsSet.PeerBidiStreamCount = TRUE;
    
    QUIC_BUFFER AlpnBuffer;
    AlpnBuffer.Buffer = (uint8_t*)ALPN;
    AlpnBuffer.Length = sizeof(ALPN) - 1;
    
    if (QUIC_FAILED(MsQuic->ConfigurationOpen(Registration, &AlpnBuffer, 1, &Settings, sizeof(Settings), NULL, &Configuration))) {
        PrintError("Failed to open configuration");
        CleanupServer();
        return 1;
    }
    
    QUIC_CERTIFICATE_FILE CertConfig = {0};
    CertConfig.CertificateFile = "server.crt";
    CertConfig.PrivateKeyFile = "server.key";
    
    QUIC_CREDENTIAL_CONFIG CredConfig = {0};
    CredConfig.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    CredConfig.CertificateFile = &CertConfig;
    CredConfig.Flags = QUIC_CREDENTIAL_FLAG_NONE;
    
    QUIC_STATUS status = MsQuic->ConfigurationLoadCredential(Configuration, &CredConfig);
    if (QUIC_FAILED(status)) {
        printf("ERROR: ConfigurationLoadCredential failed with status 0x%x\n", status);
        CleanupServer();
        return 1;
    }
    
    PrintStatus("Server credentials loaded successfully!");
    
    if (QUIC_FAILED(MsQuic->ListenerOpen(Registration, ServerListenerCallback, NULL, &Listener))) {
        PrintError("Failed to open listener");
        CleanupServer();
        return 1;
    }
    
    QUIC_ADDR Address = {0};
    QuicAddrSetFamily(&Address, QUIC_ADDRESS_FAMILY_INET);
    QuicAddrSetPort(&Address, SERVER_PORT);
    
    if (QUIC_FAILED(MsQuic->ListenerStart(Listener, &AlpnBuffer, 1, &Address))) {
        PrintError("Failed to start listener");
        CleanupServer();
        return 1;
    }
    
    printf("[SUCCESS] Server listening on port %d\n", SERVER_PORT);
    printf("[DEBUG] Wireshark capture ready - use filter: quic and udp.port == %d\n", SERVER_PORT);
    
    // Start input thread for server to send messages
    pthread_t inputThread;
    if (pthread_create(&inputThread, NULL, ServerInputThread, NULL) != 0) {
        PrintError("Failed to create input thread");
        CleanupServer();
        return 1;
    }
    
    PrintStatus("Server chat is ready! Type messages to send to client.");
    
    while (ServerRunning) {
        usleep(100000);
    }
    
    pthread_join(inputThread, NULL);
    PrintStatus("Shutting down server...");
    CleanupServer();
    
    return 0;
}