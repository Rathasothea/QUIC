package main

import (
	"crypto/tls"
	"fmt"
	"log"
	"net/http"
	"os"
	"runtime"
	"time"

	"github.com/quic-go/quic-go/http3"
)

func main() {
	// Check UDP buffer sizes and warn user
	checkUDPBuffers()

	mux := http.NewServeMux()

	// Serve static files
	fs := http.FileServer(http.Dir("./static/"))
	mux.Handle("/", fs)

	// Add a simple API endpoint to test HTTP/3
	mux.HandleFunc("/api/test", func(w http.ResponseWriter, r *http.Request) {
		protocol := r.Proto
		emoji := ""
		if r.Proto == "HTTP/3.0" {
			protocol = "HTTP/3.0 🚀"
			emoji = "🚀 "
		}

		// Add CORS headers
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
		w.Header().Set("Access-Control-Allow-Headers", "Content-Type")

		w.Header().Set("Content-Type", "application/json")
		fmt.Fprintf(w, `{%s"protocol": "%s", "method": "%s", "remote_addr": "%s", "timestamp": "%s", "user_agent": "%s"}`,
			emoji, protocol, r.Method, r.RemoteAddr, time.Now().Format(time.RFC3339), r.UserAgent())
	})

	// Add a test page endpoint
	mux.HandleFunc("/test", func(w http.ResponseWriter, r *http.Request) {
		protocol := r.Proto
		if r.Proto == "HTTP/3.0" {
			protocol = "HTTP/3.0 🚀"
		}

		w.Header().Set("Content-Type", "text/html")
		fmt.Fprintf(w, `<!DOCTYPE html>
<html>
<head><title>Protocol Test</title></head>
<body>
<h1>🚀 HTTP/3 Test Page</h1>
<p><strong>Protocol:</strong> %s</p>
<p><strong>Method:</strong> %s</p>
<p><strong>Time:</strong> %s</p>
<button onclick="fetch('/api/test').then(r=>r.json()).then(d=>console.log(d))">Test API</button>
<p>Check browser console for results!</p>
</body>
</html>`, protocol, r.Method, time.Now().Format(time.RFC3339))
	})

	// Logging middleware
	loggedMux := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		protocol := r.Proto
		emoji := ""
		if r.Proto == "HTTP/3.0" {
			protocol = "HTTP/3.0 🚀"
			emoji = "🚀 "
		}
		log.Printf("%s%s %s %s (Protocol: %s)", emoji, r.RemoteAddr, r.Method, r.URL.Path, protocol)

		// Set Alt-Svc header for HTTP/3 advertisement
		w.Header().Set("Alt-Svc", `h3=":9443"; ma=86400`)

		// Add debugging headers
		w.Header().Set("X-Server-Protocol", r.Proto)
		w.Header().Set("X-Alt-Svc-Sent", "h3=\":9443\"; ma=86400")

		mux.ServeHTTP(w, r)
	})

	// TLS configuration
	tlsConfig := &tls.Config{
		MinVersion: tls.VersionTLS12,
		MaxVersion: tls.VersionTLS13,
		NextProtos: []string{"h3", "h2", "http/1.1"},
	}

	// Start HTTP/1.1 & HTTP/2 server (TCP) in background
	go func() {
		tcpServer := &http.Server{
			Addr:         ":9443",
			Handler:      loggedMux,
			TLSConfig:    tlsConfig,
			ReadTimeout:  15 * time.Second,
			WriteTimeout: 15 * time.Second,
		}

		log.Println("🔄 Starting HTTP/1.1 & HTTP/2 server (TCP) on :9443")
		if err := tcpServer.ListenAndServeTLS("localhost+2.pem", "localhost+2-key.pem"); err != nil {
			log.Printf("❌ TCP server error: %v", err)
		}
	}()

	// Start simple HTTP server for comparison
	go func() {
		httpServer := &http.Server{
			Addr:    ":8080",
			Handler: loggedMux,
		}
		log.Println("🌐 Starting HTTP/1.1 server (no TLS) on :8080")
		if err := httpServer.ListenAndServe(); err != nil {
			log.Printf("❌ HTTP server error: %v", err)
		}
	}()

	// Give servers time to start
	time.Sleep(2 * time.Second)

	// Create HTTP/3 server (UDP)
	h3Server := &http3.Server{
		Addr:      ":9443",
		Handler:   loggedMux,
		TLSConfig: tlsConfig,
	}

	fmt.Println("")
	fmt.Println("🚀 HTTP/3 Server is Ready!")
	fmt.Println("=" + string(make([]rune, 50)))
	fmt.Println("")
	fmt.Println("🌐 Test URLs:")
	fmt.Println("   • Main page: https://localhost:9443")
	fmt.Println("   • Test page: https://localhost:9443/test")
	fmt.Println("   • API endpoint: https://localhost:9443/api/test")
	fmt.Println("   • HTTP only: http://localhost:8080")
	fmt.Println("")
	fmt.Println("🧪 Quick Tests:")
	fmt.Println("   curl -v http://localhost:8080/api/test")
	fmt.Println("   curl -v -k https://localhost:9443/api/test")
	fmt.Println("   curl -v --http2 -k https://localhost:9443/api/test")
	fmt.Println("")
	fmt.Println("🔍 Look for the 🚀 emoji in logs = HTTP/3 working!")
	fmt.Println("")

	log.Println("🚀 Starting HTTP/3 server (UDP) on :9443...")
	err := h3Server.ListenAndServeTLS("localhost+2.pem", "localhost+2-key.pem")
	if err != nil {
		log.Fatal("❌ HTTP/3 server failed to start:", err)
	}
}

func checkUDPBuffers() {
	if runtime.GOOS == "linux" {
		fmt.Println("⚠️  UDP Buffer Size Warning Detected!")
		fmt.Println("")
		fmt.Println("To fix UDP buffer sizes on Linux, run these commands as root:")
		fmt.Println("   sudo sysctl -w net.core.rmem_max=26214400")
		fmt.Println("   sudo sysctl -w net.core.rmem_default=26214400") 
		fmt.Println("   sudo sysctl -w net.core.wmem_max=26214400")
		fmt.Println("   sudo sysctl -w net.core.wmem_default=26214400")
		fmt.Println("")
		fmt.Println("Or add to /etc/sysctl.conf for permanent fix:")
		fmt.Println("   net.core.rmem_max = 26214400")
		fmt.Println("   net.core.rmem_default = 26214400")
		fmt.Println("   net.core.wmem_max = 26214400") 
		fmt.Println("   net.core.wmem_default = 26214400")
		fmt.Println("")
		fmt.Println("Then run: sudo sysctl -p")
		fmt.Println("")
		
		// Check if running as root
		if os.Geteuid() == 0 {
			fmt.Println("🔧 You're running as root - the server will still work!")
		} else {
			fmt.Println("ℹ️  Server will work but HTTP/3 performance may be limited.")
		}
		fmt.Println("=" + string(make([]rune, 60)))
		fmt.Println("")
	}
}
