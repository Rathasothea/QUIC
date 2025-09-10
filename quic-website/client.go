package main

import (
	"bufio"
	"context"
	"crypto/tls"
	"fmt"
	"io"
	"log"
	"os"
	"strings"
	"time"

	"github.com/quic-go/quic-go"
)

func main() {
	tlsConfig := &tls.Config{
		InsecureSkipVerify: true, // Only for demo - don't use in production
		NextProtos:         []string{"quic-example"},
	}

	// Add timeout for connection
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	conn, err := quic.DialAddr(ctx, "localhost:4242", tlsConfig, nil)
	if err != nil {
		log.Fatal(err)
	}
	defer conn.CloseWithError(0, "client disconnect")

	fmt.Println("🚀 Connected to QUIC server")
	fmt.Println("💬 Type messages to send to server. Special commands: ping, time, status, help, quit")
	fmt.Println("📝 Example: Try 'ping', 'hello', or 'help'")
	fmt.Println("---")

	stream, err := conn.OpenStreamSync(context.Background())
	if err != nil {
		log.Fatal(err)
	}
	defer stream.Close()

	// Start a goroutine to read responses from server
	go func() {
		reader := bufio.NewReader(stream)
		for {
			response, err := reader.ReadString('\n')
			if err != nil {
				if err == io.EOF {
					fmt.Println("\n🔌 Server disconnected")
					return
				}
				log.Printf("Error reading server response: %v", err)
				return
			}
			// Print server response and prompt for next input
			fmt.Printf("Server: %s", response)
			
		}
	}()

	// Read user input and send to server
	scanner := bufio.NewScanner(os.Stdin)
	writer := bufio.NewWriter(stream)

	fmt.Print("👤 Client: ")
	for scanner.Scan() {
		message := scanner.Text()
		
		// Send message to server
		writer.WriteString(message + "\n")
		writer.Flush()

		if strings.ToLower(message) == "quit" || strings.ToLower(message) == "exit" {
			// Give time for server response before closing
			time.Sleep(200 * time.Millisecond)
			break
		}

		// The response goroutine will handle printing "You: " prompt
	}

	if err := scanner.Err(); err != nil {
		log.Printf("Error reading input: %v", err)
	}
}