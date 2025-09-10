#include <msquic.h>
#include <stdio.h>
#include <string.h>

const QUIC_API_TABLE* MsQuic = NULL;
HQUIC Registration = NULL;
HQUIC Configuration = NULL;
HQUIC Listener = NULL;

QUIC_STATUS QUIC_API ServerListenerCallback(HQUIC Listener, void* Context, QUIC_LISTENER_EVENT* Event) {
    switch (Event->Type) {
    case QUIC_LISTENER_EVENT_NEW_CONNECTION:
        printf("Client attempting to connect\n");
        return QUIC_STATUS_SUCCESS;
    default:
        return QUIC_STATUS_NOT_SUPPORTED;
    }
}

int main() {
    QUIC_STATUS Status;
    
    printf("=== Simple QUIC Server Test ===\n");
    
    // Open MsQuic
    if (QUIC_FAILED(Status = MsQuicOpen2(&MsQuic))) {
        printf("ERROR: MsQuicOpen2 failed with 0x%x\n", Status);
        return -1;
    }
    printf("✓ MsQuic opened successfully\n");
    
    // Create registration
    const QUIC_REGISTRATION_CONFIG RegConfig = { "TestServer", QUIC_EXECUTION_PROFILE_LOW_LATENCY };
    if (QUIC_FAILED(Status = MsQuic->RegistrationOpen(&RegConfig, &Registration))) {
        printf("ERROR: RegistrationOpen failed with 0x%x\n", Status);
        goto Error;
    }
    printf("✓ Registration created\n");
    
    // Create configuration without credentials first
    const QUIC_BUFFER Alpn = { sizeof("test") - 1, (uint8_t*)"test" };
    if (QUIC_FAILED(Status = MsQuic->ConfigurationOpen(Registration, &Alpn, 1, NULL, 0, NULL, &Configuration))) {
        printf("ERROR: ConfigurationOpen failed with 0x%x\n", Status);
        goto Error;
    }
    printf("✓ Configuration created\n");
    
    printf("Basic QUIC initialization successful!\n");
    printf("Press Enter to exit...\n");
    getchar();
    
Error:
    if (Configuration) MsQuic->ConfigurationClose(Configuration);
    if (Registration) MsQuic->RegistrationClose(Registration);
    if (MsQuic) MsQuicClose(MsQuic);
    
    return QUIC_SUCCEEDED(Status) ? 0 : -1;
}
