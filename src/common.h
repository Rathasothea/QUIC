#ifndef COMMON_H
#define COMMON_H

#include <msquic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#else
#include <unistd.h>
#include <pthread.h>
#include <termios.h>
#include <fcntl.h>
#endif

#define SERVER_PORT 4567
#define SERVER_ADDRESS "127.0.0.1"
#define BUFFER_SIZE 1024
#define MAX_MESSAGE_SIZE 512

// Global MsQuic API table
extern const QUIC_API_TABLE* MsQuic;

// Helper macros
#define UNREFERENCED_PARAMETER(P) (void)(P)
// Note: QUIC_FAILED and QUIC_SUCCEEDED are already defined in msquic headers

// Function prototypes
void PrintError(const char* function, QUIC_STATUS status);
void QuicCleanup(void);
QUIC_STATUS QuicInitialize(void);

#endif // COMMON_H