// Cargo.toml dependencies:
// [dependencies]
// quinn = "0.10"
// tokio = { version = "1.0", features = ["full"] }
// rustls = { version = "0.21", features = ["dangerous_configuration"] }
// rustls-pemfile = "1.0"
// rcgen = "0.11"
// anyhow = "1.0"
// tokio-util = "0.7"

use anyhow::Result;
use quinn::{ClientConfig, Endpoint, ServerConfig};
use std::net::SocketAddr;
use std::sync::Arc;
use tokio::io::{AsyncBufReadExt, AsyncReadExt, AsyncWriteExt, BufReader};
use tokio_util::sync::CancellationToken;

// Server implementation
pub struct QuicServer {
    endpoint: Endpoint,
}

impl QuicServer {
    pub fn new(addr: SocketAddr) -> Result<Self> {
        let server_config = Self::configure_server()?;
        let endpoint = Endpoint::server(server_config, addr)?;
        
        println!("🚀 QUIC Server listening on: {}", addr);
        println!("📝 Server will echo back all received messages");
        println!("---");
        Ok(QuicServer { endpoint })
    }

    fn configure_server() -> Result<ServerConfig> {
        let cert = rcgen::generate_simple_self_signed(vec!["localhost".into()])?;
        let cert_der = cert.serialize_der()?;
        let priv_key = cert.serialize_private_key_der();
        
        let cert_chain = vec![rustls::Certificate(cert_der)];
        let key_der = rustls::PrivateKey(priv_key);
        
        let server_config = ServerConfig::with_single_cert(cert_chain, key_der)?;
        Ok(server_config)
    }

    pub async fn run(&mut self) -> Result<()> {
        while let Some(conn) = self.endpoint.accept().await {
            match conn.await {
                Ok(connection) => {
                    println!("✅ New connection from: {}", connection.remote_address());
                    
                    tokio::spawn(async move {
                        if let Err(e) = Self::handle_connection(connection).await {
                            eprintln!("❌ Connection error: {}", e);
                        }
                    });
                }
                Err(e) => {
                    eprintln!("❌ Failed to accept connection: {}", e);
                }
            }
        }
        Ok(())
    }

    async fn handle_connection(conn: quinn::Connection) -> Result<()> {
        loop {
            match conn.accept_bi().await {
                Ok((send, recv)) => {
                    tokio::spawn(async move {
                        if let Err(e) = Self::handle_stream(send, recv).await {
                            eprintln!("❌ Stream error: {}", e);
                        }
                    });
                }
                Err(quinn::ConnectionError::ApplicationClosed(_)) => {
                    println!("🔌 Client disconnected gracefully");
                    break;
                }
                Err(e) => {
                    eprintln!("❌ Error accepting stream: {}", e);
                    break;
                }
            }
        }
        Ok(())
    }

    async fn handle_stream(
        mut send: quinn::SendStream,
        mut recv: quinn::RecvStream,
    ) -> Result<()> {
        let mut buffer = [0u8; 1024];
        
        loop {
            match recv.read(&mut buffer).await {
                Ok(Some(size)) => {
                    let message = String::from_utf8_lossy(&buffer[..size]);
                    let trimmed_message = message.trim();
                    
                    println!("📨 Server received: '{}'", trimmed_message);
                    
                    // Create a response (you can customize this logic)
                    let response = if trimmed_message.to_lowercase().contains("hello") {
                        format!("👋 Hello there! You said: '{}'", trimmed_message)
                    } else if trimmed_message.to_lowercase().contains("how are you") {
                        "🤖 I'm doing great! Thanks for asking.".to_string()
                    } else if trimmed_message.to_lowercase().contains("bye") {
                        format!("👋 Goodbye! You said: '{}'", trimmed_message)
                    } else {
                        format!("✨ Echo: '{}'", trimmed_message)
                    };
                    
                    println!("📤 Server responding: '{}'", response);
                    
                    send.write_all(response.as_bytes()).await?;
                    send.flush().await?;
                    
                    // If client says bye, close the stream
                    if trimmed_message.to_lowercase().contains("bye") {
                        println!("👋 Closing stream as client said bye");
                        break;
                    }
                }
                Ok(None) => {
                    println!("🔌 Client closed the stream");
                    break;
                }
                Err(e) => {
                    eprintln!("❌ Error reading from stream: {}", e);
                    break;
                }
            }
        }
        
        send.finish().await?;
        Ok(())
    }
}

// Interactive Client implementation
pub struct QuicClient {
    endpoint: Endpoint,
}

impl QuicClient {
    pub fn new() -> Result<Self> {
        let client_config = Self::configure_client();
        let mut endpoint = Endpoint::client("0.0.0.0:0".parse()?)?;
        endpoint.set_default_client_config(client_config);
        
        Ok(QuicClient { endpoint })
    }

    fn configure_client() -> ClientConfig {
        let crypto = rustls::ClientConfig::builder()
            .with_safe_defaults()
            .with_custom_certificate_verifier(Arc::new(SkipServerVerification::new()))
            .with_no_client_auth();
        
        ClientConfig::new(Arc::new(crypto))
    }

    pub async fn interactive_chat(&self, server_addr: SocketAddr) -> Result<()> {
        let connection = self.endpoint.connect(server_addr, "localhost")?.await?;
        println!("✅ Connected to server: {}", server_addr);
        println!("💬 You can now type messages! Type 'quit' to exit.");
        println!("---");

        let (mut send, mut recv) = connection.open_bi().await?;
        let cancel_token = CancellationToken::new();

        // Spawn task to handle incoming messages from server
        let recv_cancel = cancel_token.clone();
        let recv_task = tokio::spawn(async move {
            let mut buffer = [0u8; 1024];
            loop {
                tokio::select! {
                    _ = recv_cancel.cancelled() => {
                        break;
                    }
                    result = recv.read(&mut buffer) => {
                        match result {
                            Ok(Some(size)) => {
                                let message = String::from_utf8_lossy(&buffer[..size]);
                                println!("📨 Server says: {}", message);
                                println!("💬 Type your message: ");
                            }
                            Ok(None) => {
                                println!("🔌 Server closed the connection");
                                break;
                            }
                            Err(e) => {
                                eprintln!("❌ Error reading from server: {}", e);
                                break;
                            }
                        }
                    }
                }
            }
        });

        // Handle user input
        let stdin = tokio::io::stdin();
        let mut reader = BufReader::new(stdin);
        let mut line = String::new();

        println!("💬 Type your message: ");
        
        loop {
            line.clear();
            match reader.read_line(&mut line).await {
                Ok(0) => {
                    println!("📝 EOF reached, exiting...");
                    break;
                }
                Ok(_) => {
                    let message = line.trim();
                    
                    if message.is_empty() {
                        continue;
                    }
                    
                    if message.to_lowercase() == "quit" {
                        println!("👋 Exiting chat...");
                        break;
                    }
                    
                    println!("📤 Sending: '{}'", message);
                    
                    match send.write_all(message.as_bytes()).await {
                        Ok(_) => {
                            if let Err(e) = send.flush().await {
                                eprintln!("❌ Error flushing message: {}", e);
                                break;
                            }
                        }
                        Err(e) => {
                            eprintln!("❌ Error sending message: {}", e);
                            break;
                        }
                    }
                    
                    // If user says bye, prepare to exit after server response
                    if message.to_lowercase().contains("bye") {
                        tokio::time::sleep(tokio::time::Duration::from_millis(500)).await;
                        break;
                    }
                }
                Err(e) => {
                    eprintln!("❌ Error reading input: {}", e);
                    break;
                }
            }
        }

        // Clean up
        cancel_token.cancel();
        send.finish().await?;
        recv_task.abort();
        connection.close(0u32.into(), b"client closing");
        
        println!("👋 Chat session ended!");
        Ok(())
    }
}

// Certificate verification bypass for demo purposes
#[derive(Debug)]
struct SkipServerVerification;

impl SkipServerVerification {
    fn new() -> Self {
        Self
    }
}

impl rustls::client::ServerCertVerifier for SkipServerVerification {
    fn verify_server_cert(
        &self,
        _end_entity: &rustls::Certificate,
        _intermediates: &[rustls::Certificate],
        _server_name: &rustls::ServerName,
        _scts: &mut dyn Iterator<Item = &[u8]>,
        _ocsp_response: &[u8],
        _now: std::time::SystemTime,
    ) -> Result<rustls::client::ServerCertVerified, rustls::Error> {
        Ok(rustls::client::ServerCertVerified::assertion())
    }
}

// Main function with menu
#[tokio::main]
async fn main() -> Result<()> {
    let server_addr: SocketAddr = "127.0.0.1:4433".parse()?;
    
    println!("🚀 QUIC Interactive Chat Demo");
    println!("============================");
    println!("Choose mode:");
    println!("1. Server mode - Wait for clients");
    println!("2. Client mode - Connect to server and chat");
    println!("3. Demo mode - Run both server and client");
    println!();
    print!("Enter your choice (1, 2, or 3): ");
    
    use std::io::{self, Write};
    io::stdout().flush()?;
    
    let mut input = String::new();
    io::stdin().read_line(&mut input)?;
    
    match input.trim() {
        "1" => {
            println!("\n🔧 Starting server mode...");
            let mut server = QuicServer::new(server_addr)?;
            server.run().await?;
        }
        "2" => {
            println!("\n🔧 Starting client mode...");
            println!("🔗 Make sure the server is running on {}", server_addr);
            tokio::time::sleep(tokio::time::Duration::from_millis(500)).await;
            
            let client = QuicClient::new()?;
            client.interactive_chat(server_addr).await?;
        }
        "3" => {
            println!("\n🔧 Starting demo mode (both server and client)...");
            
            // Start server in background
            let mut server = QuicServer::new(server_addr)?;
            tokio::spawn(async move {
                if let Err(e) = server.run().await {
                    eprintln!("❌ Server error: {}", e);
                }
            });
            
            // Give server time to start
            tokio::time::sleep(tokio::time::Duration::from_millis(500)).await;
            
            // Start interactive client
            let client = QuicClient::new()?;
            client.interactive_chat(server_addr).await?;
        }
        _ => {
            println!("❌ Invalid choice. Please run again and choose 1, 2, or 3.");
        }
    }
    
    Ok(())
}