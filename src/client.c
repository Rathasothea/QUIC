#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <conio.h>
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
HQUIC Connection = NULL;

#define SERVER_PORT 4567
#define DEFAULT_SERVER_IP "127.0.0.1"
static const char ALPN[] = "sample-app";

volatile int ConnectionReady = 0;
volatile int ClientRunning = 1;
pthread_mutex_t outputMutex = PTHREAD_MUTEX_INITIALIZER;

// Function declarations
void PrintStatus(const char* message);
void PrintError(const char* message);
QUIC_STATUS ClientConnectionCallback(HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event);
QUIC_STATUS ClientStreamCallback(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event);
int SendMessage(const char* message);
void CleanupClient();
void* ClientInputThread(void* arg);

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

void* ClientInputThread(void* arg) {
    UNREFERENCED_PARAMETER(arg);
    
    char input[1024];
    printf("\n=== Client Chat Ready ===\n");
    printf("Type messages to send to server (type '/quit' to stop client):\n");
    printf("[CLIENT] > ");
    fflush(stdout);
    
    while (ClientRunning && ConnectionReady) {
        if (!fgets(input, sizeof(input), stdin)) {
            break;
        }
        
        input[strcspn(input, "\n")] = 0;
        
        if (strlen(input) == 0) {
            printf("[CLIENT] > ");
            fflush(stdout);
            continue;
        }
        
        if (strcmp(input, "/quit") == 0) {
            ClientRunning = 0;
            break;
        }
        
        if (!SendMessage(input)) {
            PrintError("Failed to send message");
        }
        
        usleep(100000);
    }
    
    return NULL;
}

QUIC_STATUS ClientConnectionCallback(HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event) {
    UNREFERENCED_PARAMETER(Connection);
    UNREFERENCED_PARAMETER(Context);
    
    switch (Event->Type) {
        case QUIC_CONNECTION_EVENT_CONNECTED:
            PrintStatus("Successfully connected to server!");
            ConnectionReady = 1;
            return QUIC_STATUS_SUCCESS;
            
        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
            printf("[INFO] Connection shutdown by transport (Error: 0x%llx)\n", 
                   (unsigned long long)Event->SHUTDOWN_INITIATED_BY_TRANSPORT.ErrorCode);
            ConnectionReady = 0;
            ClientRunning = 0;
            return QUIC_STATUS_SUCCESS;
            
        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
            printf("[INFO] Connection shutdown by peer (Error: 0x%llx)\n", 
                   (unsigned long long)Event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
            ConnectionReady = 0;
            ClientRunning = 0;
            return QUIC_STATUS_SUCCESS;
            
        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            PrintStatus("Connection shutdown complete");
            ConnectionReady = 0;
            ClientRunning = 0;
            return QUIC_STATUS_SUCCESS;
            
        case QUIC_CONNECTION_EVENT_PEER_CERTIFICATE_RECEIVED:
            return QUIC_STATUS_SUCCESS;
            
        case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
            PrintStatus("Server started a new stream");
            MsQuic->SetCallbackHandler(Event->PEER_STREAM_STARTED.Stream, (void*)ClientStreamCallback, NULL);
            return QUIC_STATUS_SUCCESS;
            
        default:
            return QUIC_STATUS_SUCCESS;
    }
}

// Fixed ClientStreamCallback with proper memory management
QUIC_STATUS ClientStreamCallback(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event) {
    UNREFERENCED_PARAMETER(Context);
    
    switch (Event->Type) {
        case QUIC_STREAM_EVENT_START_COMPLETE:
            if (QUIC_FAILED(Event->START_COMPLETE.Status)) {
                printf("[ERROR] Stream start failed with status 0x%x\n", Event->START_COMPLETE.Status);
            }
            return QUIC_STATUS_SUCCESS;
            
        case QUIC_STREAM_EVENT_SEND_COMPLETE:
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
                // Process received message from server
                pthread_mutex_lock(&outputMutex);
                printf("\n[SERVER -> CLIENT] ");
                
                for (uint32_t i = 0; i < Event->RECEIVE.BufferCount; i++) {
                    printf("%.*s", (int)Event->RECEIVE.Buffers[i].Length, 
                           Event->RECEIVE.Buffers[i].Buffer);
                }
                
                printf("\n[CLIENT] > ");
                fflush(stdout);
                pthread_mutex_unlock(&outputMutex);
                
                return QUIC_STATUS_SUCCESS;
            }
            
        case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
            return QUIC_STATUS_SUCCESS;
            
        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            MsQuic->StreamClose(Stream);
            return QUIC_STATUS_SUCCESS;
            
        default:
            return QUIC_STATUS_SUCCESS;
    }
}

int SendMessage(const char* message) {
    if (!ConnectionReady) {
        PrintError("Connection not ready");
        return 0;
    }
    
    HQUIC Stream = NULL;
    
    if (QUIC_FAILED(MsQuic->StreamOpen(Connection, QUIC_STREAM_OPEN_FLAG_NONE, ClientStreamCallback, NULL, &Stream))) {
        PrintError("Failed to open stream");
        return 0;
    }
    
    if (QUIC_FAILED(MsQuic->StreamStart(Stream, QUIC_STREAM_START_FLAG_NONE))) {
        PrintError("Failed to start stream");
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
    
    // Send with FIN flag to properly close the stream
    if (QUIC_FAILED(MsQuic->StreamSend(Stream, SendBuffer, 1, QUIC_SEND_FLAG_FIN, SendBuffer))) {
        PrintError("Failed to send message");
        free(SendBuffer->Buffer);
        free(SendBuffer);
        MsQuic->StreamClose(Stream);
        return 0;
    }
    
    pthread_mutex_lock(&outputMutex);
    printf("[CLIENT -> SERVER] %s\n", message);
    printf("[CLIENT] > ");
    fflush(stdout);
    pthread_mutex_unlock(&outputMutex);
    
    return 1;
}

void CleanupClient() {
    if (Connection) {
        MsQuic->ConnectionClose(Connection);
        Connection = NULL;
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
    PrintStatus("Client cleanup completed");
}

int main(int argc, char* argv[]) {
    const char* serverIP = DEFAULT_SERVER_IP;
    int serverPort = SERVER_PORT;
    
    if (argc >= 2) {
        serverIP = argv[1];
    }
    if (argc >= 3) {
        serverPort = atoi(argv[2]);
    }
    
    printf("=== QUIC Chat Client ===\n");
    printf("Connecting to %s:%d...\n", serverIP, serverPort);
    
    if (QUIC_FAILED(MsQuicOpen2(&MsQuic))) {
        PrintError("Failed to open MsQuic");
        return 1;
    }
    
    const QUIC_REGISTRATION_CONFIG RegConfig = { "quic-client", QUIC_EXECUTION_PROFILE_LOW_LATENCY };
    if (QUIC_FAILED(MsQuic->RegistrationOpen(&RegConfig, &Registration))) {
        PrintError("Failed to open registration");
        CleanupClient();
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
        CleanupClient();
        return 1;
    }
    
    PrintStatus("Loading client credentials...");
    QUIC_CREDENTIAL_CONFIG CredConfig = {0};
    CredConfig.Type = QUIC_CREDENTIAL_TYPE_NONE;
    CredConfig.Flags = QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
    
    QUIC_STATUS status = MsQuic->ConfigurationLoadCredential(Configuration, &CredConfig);
    if (QUIC_FAILED(status)) {
        printf("ERROR: ConfigurationLoadCredential failed with status 0x%x\n", status);
        PrintError("Failed to load client credentials");
        CleanupClient();
        return 1;
    }

    PrintStatus("Client credentials loaded successfully!");
    
    if (QUIC_FAILED(MsQuic->ConnectionOpen(Registration, ClientConnectionCallback, NULL, &Connection))) {
        PrintError("Failed to open connection");
        CleanupClient();
        return 1;
    }
    
    if (QUIC_FAILED(MsQuic->ConnectionStart(Connection, Configuration, QUIC_ADDRESS_FAMILY_UNSPEC, serverIP, serverPort))) {
        PrintError("Failed to start connection");
        CleanupClient();
        return 1;
    }
    
    PrintStatus("Connecting to server...");
    
    int timeout = 100;
    while (!ConnectionReady && timeout > 0 && ClientRunning) {
        usleep(100000);
        timeout--;
    }
    
    if (!ConnectionReady) {
        if (ClientRunning) {
            PrintError("Connection timeout - server may not be running");
        } else {
            PrintError("Connection failed during handshake");
        }
        CleanupClient();
        return 1;
    }
    
    // Start input thread for client to send messages
    pthread_t inputThread;
    if (pthread_create(&inputThread, NULL, ClientInputThread, NULL) != 0) {
        PrintError("Failed to create input thread");
        CleanupClient();
        return 1;
    }
    
    while (ClientRunning && ConnectionReady) {
        usleep(100000);
    }
    
    pthread_join(inputThread, NULL);
    PrintStatus("Shutting down client...");
    CleanupClient();
    
    return 0;
}