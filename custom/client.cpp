#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "msquic.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include <msquic.h>

class MSQuicClient {
private:
    const QUIC_API_TABLE* MsQuic = nullptr;
    HQUIC Registration = nullptr;
    HQUIC Configuration = nullptr;
    HQUIC Connection = nullptr;
    HQUIC Stream = nullptr;
    
    static const uint16_t DEFAULT_PORT = 4433;
    static const char* DEFAULT_HOST;
    bool ConnectionReady = false;
    bool StreamReady = false;

public:
    MSQuicClient() {
        InitializeQuic();
    }

    ~MSQuicClient() {
        Cleanup();
    }

    bool InitializeQuic() {
        // Load MsQuic
        if (QUIC_FAILED(MsQuicOpen2(&MsQuic))) {
            std::cerr << "MsQuicOpen2 failed!" << std::endl;
            return false;
        }

        // Create registration
        const QUIC_REGISTRATION_CONFIG RegConfig = { "msquic_client", QUIC_EXECUTION_PROFILE_LOW_LATENCY };
        if (QUIC_FAILED(MsQuic->RegistrationOpen(&RegConfig, &Registration))) {
            std::cerr << "RegistrationOpen failed!" << std::endl;
            return false;
        }

        // Configure client settings
        QUIC_SETTINGS Settings = {};
        Settings.IdleTimeoutMs = 60000;
        Settings.IsSet.IdleTimeoutMs = TRUE;

        // Create configuration
        QUIC_BUFFER AlpnBuffer = { sizeof("sample") - 1, (uint8_t*)"sample" };
        
        if (QUIC_FAILED(MsQuic->ConfigurationOpen(Registration, &AlpnBuffer, 1, &Settings, sizeof(Settings), nullptr, &Configuration))) {
            std::cerr << "ConfigurationOpen failed!" << std::endl;
            return false;
        }

        // Configure credentials (client doesn't need certificates)
        QUIC_CREDENTIAL_CONFIG CredConfig = {};
        CredConfig.Type = QUIC_CREDENTIAL_TYPE_NONE;
        CredConfig.Flags = QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;

        if (QUIC_FAILED(MsQuic->ConfigurationLoadCredential(Configuration, &CredConfig))) {
            std::cerr << "ConfigurationLoadCredential failed!" << std::endl;
            return false;
        }

        std::cout << "MSQUIC Client initialized successfully!" << std::endl;
        return true;
    }

    bool ConnectToServer(const std::string& hostname = "127.0.0.1", uint16_t port = DEFAULT_PORT) {
        // Create connection
        if (QUIC_FAILED(MsQuic->ConnectionOpen(Registration, ConnectionCallback, this, &Connection))) {
            std::cerr << "ConnectionOpen failed!" << std::endl;
            return false;
        }

        // Start connection
        if (QUIC_FAILED(MsQuic->ConnectionStart(Connection, Configuration, QUIC_ADDRESS_FAMILY_UNSPEC, hostname.c_str(), port))) {
            std::cerr << "ConnectionStart failed!" << std::endl;
            return false;
        }

        std::cout << "Connecting to " << hostname << ":" << port << std::endl;

        // Wait for connection to be established
        int timeout = 5000; // 5 seconds
        while (!ConnectionReady && timeout > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            timeout -= 100;
        }

        if (!ConnectionReady) {
            std::cerr << "Connection timeout!" << std::endl;
            return false;
        }

        return true;
    }

    bool SendMessage(const std::string& message) {
        if (!ConnectionReady) {
            std::cerr << "Connection not ready!" << std::endl;
            return false;
        }

        // Create stream
        if (QUIC_FAILED(MsQuic->StreamOpen(Connection, QUIC_STREAM_OPEN_FLAG_NONE, StreamCallback, this, &Stream))) {
            std::cerr << "StreamOpen failed!" << std::endl;
            return false;
        }

        // Start stream
        if (QUIC_FAILED(MsQuic->StreamStart(Stream, QUIC_STREAM_START_FLAG_NONE))) {
            std::cerr << "StreamStart failed!" << std::endl;
            return false;
        }

        // Wait for stream to be ready
        int timeout = 2000; // 2 seconds
        while (!StreamReady && timeout > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            timeout -= 50;
        }

        // Prepare message buffer
        QUIC_BUFFER* SendBuffer = (QUIC_BUFFER*)malloc(sizeof(QUIC_BUFFER));
        SendBuffer->Length = message.length();
        SendBuffer->Buffer = (uint8_t*)malloc(message.length());
        memcpy(SendBuffer->Buffer, message.c_str(), message.length());

        // Send message
        if (QUIC_FAILED(MsQuic->StreamSend(Stream, SendBuffer, 1, QUIC_SEND_FLAG_FIN, SendBuffer))) {
            std::cerr << "StreamSend failed!" << std::endl;
            free(SendBuffer->Buffer);
            free(SendBuffer);
            return false;
        }

        std::cout << "Sent message: " << message << std::endl;
        return true;
    }

    void RunClient() {
        std::string input;
        std::cout << "Connected! Type messages to send (type 'quit' to exit):" << std::endl;
        
        while (true) {
            std::cout << "> ";
            std::getline(std::cin, input);
            
            if (input == "quit" || input == "exit") {
                break;
            }
            
            if (!input.empty()) {
                SendMessage(input);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
    }

    void Cleanup() {
        if (Stream) {
            MsQuic->StreamClose(Stream);
            Stream = nullptr;
        }
        if (Connection) {
            MsQuic->ConnectionClose(Connection);
            Connection = nullptr;
        }
        if (Configuration) {
            MsQuic->ConfigurationClose(Configuration);
            Configuration = nullptr;
        }
        if (Registration) {
            MsQuic->RegistrationClose(Registration);
            Registration = nullptr;
        }
        if (MsQuic) {
            MsQuicClose(MsQuic);
            MsQuic = nullptr;
        }
    }

private:
    static QUIC_STATUS QUIC_API ConnectionCallback(
        _In_ HQUIC Connection,
        _In_opt_ void* Context,
        _Inout_ QUIC_CONNECTION_EVENT* Event
    ) {
        MSQuicClient* client = static_cast<MSQuicClient*>(Context);
        
        switch (Event->Type) {
        case QUIC_CONNECTION_EVENT_CONNECTED:
            std::cout << "Connected to server!" << std::endl;
            client->ConnectionReady = true;
            return QUIC_STATUS_SUCCESS;
            
        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
            std::cout << "Connection shutting down." << std::endl;
            return QUIC_STATUS_SUCCESS;
            
        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            std::cout << "Connection shutdown complete." << std::endl;
            client->ConnectionReady = false;
            return QUIC_STATUS_SUCCESS;
            
        case QUIC_CONNECTION_EVENT_RESUMPTION_TICKET_RECEIVED:
            std::cout << "Resumption ticket received." << std::endl;
            return QUIC_STATUS_SUCCESS;
            
        default:
            return QUIC_STATUS_SUCCESS;
        }
    }

    static QUIC_STATUS QUIC_API StreamCallback(
        _In_ HQUIC Stream,
        _In_opt_ void* Context,
        _Inout_ QUIC_STREAM_EVENT* Event
    ) {
        MSQuicClient* client = static_cast<MSQuicClient*>(Context);
        
        switch (Event->Type) {
        case QUIC_STREAM_EVENT_START_COMPLETE:
            std::cout << "Stream started." << std::endl;
            client->StreamReady = true;
            return QUIC_STATUS_SUCCESS;
            
        case QUIC_STREAM_EVENT_SEND_COMPLETE:
            // Free the send buffer
            if (Event->SEND_COMPLETE.ClientContext) {
                QUIC_BUFFER* Buffer = (QUIC_BUFFER*)Event->SEND_COMPLETE.ClientContext;
                free(Buffer->Buffer);
                free(Buffer);
            }
            return QUIC_STATUS_SUCCESS;
            
        case QUIC_STREAM_EVENT_RECEIVE:
            std::cout << "Server response: ";
            for (uint32_t i = 0; i < Event->RECEIVE.BufferCount; i++) {
                std::cout.write((char*)Event->RECEIVE.Buffers[i].Buffer, Event->RECEIVE.Buffers[i].Length);
            }
            std::cout << std::endl;
            return QUIC_STATUS_SUCCESS;
            
        case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
            std::cout << "Server finished sending." << std::endl;
            return QUIC_STATUS_SUCCESS;
            
        case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
            std::cout << "Server aborted send." << std::endl;
            return QUIC_STATUS_SUCCESS;
            
        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            std::cout << "Stream shutdown complete." << std::endl;
            client->StreamReady = false;
            return QUIC_STATUS_SUCCESS;
            
        default:
            return QUIC_STATUS_SUCCESS;
        }
    }
};

const char* MSQuicClient::DEFAULT_HOST = "127.0.0.1";

int main(int argc, char* argv[]) {
    std::string hostname = "127.0.0.1";
    uint16_t port = 4433;
    
    if (argc >= 2) {
        hostname = argv[1];
    }
    if (argc >= 3) {
        port = static_cast<uint16_t>(std::stoi(argv[2]));
    }
    
    std::cout << "MSQUIC Client Starting..." << std::endl;
    
    MSQuicClient client;
    
    if (client.ConnectToServer(hostname, port)) {
        client.RunClient();
    } else {
        std::cerr << "Failed to connect to server!" << std::endl;
        return 1;
    }
    
    std::cout << "Client shutting down..." << std::endl;
    return 0;
}