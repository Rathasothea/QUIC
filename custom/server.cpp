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

class MSQuicServer {
private:
    const QUIC_API_TABLE* MsQuic = nullptr;
    HQUIC Registration = nullptr;
    HQUIC Configuration = nullptr;
    HQUIC Listener = nullptr;
    
    static const uint16_t DEFAULT_PORT = 4433;
    static const char* ALPN;

public:
    MSQuicServer() {
        InitializeQuic();
    }

    ~MSQuicServer() {
        Cleanup();
    }

    bool InitializeQuic() {
        // Load MsQuic
        if (QUIC_FAILED(MsQuicOpen2(&MsQuic))) {
            std::cerr << "MsQuicOpen2 failed!" << std::endl;
            return false;
        }

        // Create registration
        const QUIC_REGISTRATION_CONFIG RegConfig = { "msquic_server", QUIC_EXECUTION_PROFILE_LOW_LATENCY };
        if (QUIC_FAILED(MsQuic->RegistrationOpen(&RegConfig, &Registration))) {
            std::cerr << "RegistrationOpen failed!" << std::endl;
            return false;
        }

        // Configure server settings
        QUIC_SETTINGS Settings = {};
        Settings.IdleTimeoutMs = 60000;
        Settings.IsSet.IdleTimeoutMs = TRUE;
        Settings.ServerResumptionLevel = QUIC_SERVER_RESUME_AND_ZERORTT;
        Settings.IsSet.ServerResumptionLevel = TRUE;
        Settings.PeerBidiStreamCount = 1;
        Settings.IsSet.PeerBidiStreamCount = TRUE;

        // Load server certificate (self-signed for demo)
        QUIC_CREDENTIAL_CONFIG CredConfig = {};
        CredConfig.Type = QUIC_CREDENTIAL_TYPE_NONE;
        CredConfig.Flags = QUIC_CREDENTIAL_FLAG_NONE;

        // Create configuration
        const char* Alpn = "sample";
        QUIC_BUFFER AlpnBuffer = { sizeof("sample") - 1, (uint8_t*)"sample" };
        
        if (QUIC_FAILED(MsQuic->ConfigurationOpen(Registration, &AlpnBuffer, 1, &Settings, sizeof(Settings), nullptr, &Configuration))) {
            std::cerr << "ConfigurationOpen failed!" << std::endl;
            return false;
        }

        // Load credentials
        if (QUIC_FAILED(MsQuic->ConfigurationLoadCredential(Configuration, &CredConfig))) {
            std::cerr << "ConfigurationLoadCredential failed!" << std::endl;
            return false;
        }

        std::cout << "MSQUIC Server initialized successfully!" << std::endl;
        return true;
    }

    bool StartServer() {
        // Create listener
        if (QUIC_FAILED(MsQuic->ListenerOpen(Registration, ListenerCallback, this, &Listener))) {
            std::cerr << "ListenerOpen failed!" << std::endl;
            return false;
        }

        // Start listening
        QUIC_ADDR Address = {};
        QuicAddrSetFamily(&Address, QUIC_ADDRESS_FAMILY_INET);
        QuicAddrSetPort(&Address, DEFAULT_PORT);

        if (QUIC_FAILED(MsQuic->ListenerStart(Listener, &AlpnBuffer, 1, &Address))) {
            std::cerr << "ListenerStart failed!" << std::endl;
            return false;
        }

        std::cout << "Server listening on port " << DEFAULT_PORT << std::endl;
        return true;
    }

    void RunServer() {
        std::cout << "Server running. Press Enter to stop..." << std::endl;
        std::cin.get();
    }

    void Cleanup() {
        if (Listener) {
            MsQuic->ListenerClose(Listener);
            Listener = nullptr;
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
    static QUIC_BUFFER AlpnBuffer;

    static QUIC_STATUS QUIC_API ListenerCallback(
        _In_ HQUIC Listener,
        _In_opt_ void* Context,
        _Inout_ QUIC_LISTENER_EVENT* Event
    ) {
        MSQuicServer* server = static_cast<MSQuicServer*>(Context);
        
        switch (Event->Type) {
        case QUIC_LISTENER_EVENT_NEW_CONNECTION:
            std::cout << "New connection received!" << std::endl;
            Event->NEW_CONNECTION.SecurityConfig = server->Configuration;
            server->MsQuic->SetCallbackHandler(Event->NEW_CONNECTION.Connection, (void*)ConnectionCallback, Context);
            return QUIC_STATUS_SUCCESS;
            
        case QUIC_LISTENER_EVENT_STOP_COMPLETE:
            std::cout << "Listener stopped." << std::endl;
            return QUIC_STATUS_SUCCESS;
            
        default:
            return QUIC_STATUS_NOT_SUPPORTED;
        }
    }

    static QUIC_STATUS QUIC_API ConnectionCallback(
        _In_ HQUIC Connection,
        _In_opt_ void* Context,
        _Inout_ QUIC_CONNECTION_EVENT* Event
    ) {
        MSQuicServer* server = static_cast<MSQuicServer*>(Context);
        
        switch (Event->Type) {
        case QUIC_CONNECTION_EVENT_CONNECTED:
            std::cout << "Client connected!" << std::endl;
            return QUIC_STATUS_SUCCESS;
            
        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
            std::cout << "Connection shutting down." << std::endl;
            return QUIC_STATUS_SUCCESS;
            
        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            std::cout << "Connection shutdown complete." << std::endl;
            server->MsQuic->ConnectionClose(Connection);
            return QUIC_STATUS_SUCCESS;
            
        case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
            std::cout << "Peer stream started!" << std::endl;
            server->MsQuic->SetCallbackHandler(Event->PEER_STREAM_STARTED.Stream, (void*)StreamCallback, Context);
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
        MSQuicServer* server = static_cast<MSQuicServer*>(Context);
        
        switch (Event->Type) {
        case QUIC_STREAM_EVENT_SEND_COMPLETE:
            std::cout << "Send completed." << std::endl;
            return QUIC_STATUS_SUCCESS;
            
        case QUIC_STREAM_EVENT_RECEIVE:
            std::cout << "Received data: ";
            for (uint32_t i = 0; i < Event->RECEIVE.BufferCount; i++) {
                std::cout.write((char*)Event->RECEIVE.Buffers[i].Buffer, Event->RECEIVE.Buffers[i].Length);
            }
            std::cout << std::endl;
            
            // Echo the message back
            std::string response = "Server received: ";
            for (uint32_t i = 0; i < Event->RECEIVE.BufferCount; i++) {
                response.append((char*)Event->RECEIVE.Buffers[i].Buffer, Event->RECEIVE.Buffers[i].Length);
            }
            
            QUIC_BUFFER* SendBuffer = (QUIC_BUFFER*)malloc(sizeof(QUIC_BUFFER));
            SendBuffer->Length = response.length();
            SendBuffer->Buffer = (uint8_t*)malloc(response.length());
            memcpy(SendBuffer->Buffer, response.c_str(), response.length());
            
            server->MsQuic->StreamSend(Stream, SendBuffer, 1, QUIC_SEND_FLAG_FIN, SendBuffer);
            return QUIC_STATUS_SUCCESS;
            
        case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
            std::cout << "Peer finished sending." << std::endl;
            return QUIC_STATUS_SUCCESS;
            
        case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
            std::cout << "Peer aborted send." << std::endl;
            server->MsQuic->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
            return QUIC_STATUS_SUCCESS;
            
        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            std::cout << "Stream shutdown complete." << std::endl;
            server->MsQuic->StreamClose(Stream);
            return QUIC_STATUS_SUCCESS;
            
        default:
            return QUIC_STATUS_SUCCESS;
        }
    }
};

QUIC_BUFFER MSQuicServer::AlpnBuffer = { sizeof("sample") - 1, (uint8_t*)"sample" };

int main() {
    std::cout << "MSQUIC Server Starting..." << std::endl;
    
    MSQuicServer server;
    
    if (server.StartServer()) {
        server.RunServer();
    } else {
        std::cerr << "Failed to start server!" << std::endl;
        return 1;
    }
    
    std::cout << "Server shutting down..." << std::endl;
    return 0;
}