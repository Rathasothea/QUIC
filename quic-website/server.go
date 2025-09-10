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
	"sync"

	"github.com/quic-go/quic-go"
)

// Store active client connections
var (
	clients = make(map[int]*ClientConnection)
	clientMu sync.RWMutex
	nextID = 1
)

type ClientConnection struct {
	ID     int
	Stream *quic.Stream
	Writer *bufio.Writer
	Addr   string
}

func main() {
	// Generate a self-signed certificate for demo purposes
	cert, err := generateSelfSignedCert()
	if err != nil {
		log.Fatal(err)
	}

	tlsConfig := &tls.Config{
		Certificates: []tls.Certificate{cert},
		NextProtos:   []string{"quic-example"},
	}

	listener, err := quic.ListenAddr("localhost:4242", tlsConfig, nil)
	if err != nil {
		log.Fatal(err)
	}
	defer listener.Close()

	fmt.Println("🚀 QUIC Server listening on localhost:4242")
	fmt.Println("💬 Server Console - Type messages to send to clients:")
	fmt.Println("📝 Commands: 'list' (show clients), 'to <id> <message>' (send to specific client), or just type to broadcast")
	fmt.Println("---")

	// Start server input handler in a goroutine
	go handleServerInput()

	for {
		conn, err := listener.Accept(context.Background())
		if err != nil {
			log.Printf("Failed to accept connection: %v", err)
			continue
		}

		go handleConnection(conn)
	}
}

func handleConnection(conn *quic.Conn) {
	defer conn.CloseWithError(0, "connection closed")
	
	fmt.Printf("📱 New connection from: %s\n", conn.RemoteAddr())

	for {
		stream, err := conn.AcceptStream(context.Background())
		if err != nil {
			log.Printf("Failed to accept stream: %v", err)
			return
		}

		go handleStream(stream, conn.RemoteAddr().String())
	}
}

func handleStream(stream *quic.Stream, addr string) {
	defer stream.Close()

	reader := bufio.NewReader(stream)
	writer := bufio.NewWriter(stream)

	// Register this client
	clientMu.Lock()
	clientID := nextID
	nextID++
	clients[clientID] = &ClientConnection{
		ID:     clientID,
		Stream: stream,
		Writer: writer,
		Addr:   addr,
	}
	clientMu.Unlock()

	fmt.Printf("✅ Client #%d connected from %s\n", clientID, addr)
	
	// Send welcome message
	welcomeMsg := fmt.Sprintf("Welcome! You are client #%d\n", clientID)
	writer.WriteString(welcomeMsg)
	writer.Flush()

	// Handle client disconnection
	defer func() {
		clientMu.Lock()
		delete(clients, clientID)
		clientMu.Unlock()
		fmt.Printf("👋 Client #%d disconnected\n", clientID)
	}()

	for {
		message, err := reader.ReadString('\n')
		if err != nil {
			if err == io.EOF {
				return
			}
			log.Printf("Error reading from client #%d: %v", clientID, err)
			return
		}

		// Remove newline character
		message = strings.TrimSpace(message)
		fmt.Printf("💬 Client #%d says: %s\n", clientID, message)

		// Handle special client commands
		if strings.ToLower(message) == "quit" || strings.ToLower(message) == "exit" {
			response := "Goodbye! Thanks for chatting!"
			writer.WriteString(response + "\n")
			writer.Flush()
			return
		}

		// Auto-acknowledge receipt (optional)
		ack := fmt.Sprintf("Message received: %s", message)
		writer.WriteString(ack + "\n")
		writer.Flush()
	}
}

func handleServerInput() {
	scanner := bufio.NewScanner(os.Stdin)
	fmt.Print("Server> ")

	for scanner.Scan() {
		input := scanner.Text()
		
		if input == "" {
			fmt.Print("Server> ")
			continue
		}

		// Handle server commands
		switch {
		case input == "list":
			listClients()
		case strings.HasPrefix(input, "to "):
			handleDirectMessage(input)
		case input == "help":
			showHelp()
		case input == "quit":
			fmt.Println("Shutting down server...")
			os.Exit(0)
		default:
			// Broadcast message to all clients
			broadcastMessage(input)
		}

		fmt.Print("Server> ")
	}
}

func listClients() {
	clientMu.RLock()
	defer clientMu.RUnlock()

	if len(clients) == 0 {
		fmt.Println("📝 No clients connected")
		return
	}

	fmt.Println("📋 Connected clients:")
	for id, client := range clients {
		fmt.Printf("  #%d - %s\n", id, client.Addr)
	}
}

func handleDirectMessage(input string) {
	// Parse "to <id> <message>"
	parts := strings.SplitN(input, " ", 3)
	if len(parts) < 3 {
		fmt.Println("❌ Usage: to <client_id> <message>")
		return
	}

	var targetID int
	_, err := fmt.Sscanf(parts[1], "%d", &targetID)
	if err != nil {
		fmt.Println("❌ Invalid client ID")
		return
	}

	message := parts[2]
	
	clientMu.RLock()
	client, exists := clients[targetID]
	clientMu.RUnlock()

	if !exists {
		fmt.Printf("❌ Client #%d not found\n", targetID)
		return
	}

	// Send message to specific client
	_, err = client.Writer.WriteString(fmt.Sprintf("Server (private): %s\n", message))
	if err != nil {
		fmt.Printf("❌ Failed to send to client #%d: %v\n", targetID, err)
		return
	}
	client.Writer.Flush()
	
	fmt.Printf("✅ Sent to client #%d: %s\n", targetID, message)
}

func broadcastMessage(message string) {
	clientMu.RLock()
	defer clientMu.RUnlock()

	if len(clients) == 0 {
		fmt.Println("📝 No clients to send message to")
		return
	}

	sentCount := 0
	for id, client := range clients {
		_, err := client.Writer.WriteString(fmt.Sprintf("Server: %s\n", message))
		if err != nil {
			fmt.Printf("❌ Failed to send to client #%d: %v\n", id, err)
			continue
		}
		client.Writer.Flush()
		sentCount++
	}

	fmt.Printf("✅ Message sent to %d client(s): %s\n", sentCount, message)
}

func showHelp() {
	fmt.Println("🆘 Server Commands:")
	fmt.Println("  list                 - Show connected clients")
	fmt.Println("  to <id> <message>    - Send private message to specific client")
	fmt.Println("  help                 - Show this help")
	fmt.Println("  quit                 - Shutdown server")
	fmt.Println("  <message>            - Broadcast message to all clients")
}