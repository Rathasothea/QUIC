// client.rs - Improved bidirectional communication
use anyhow::Result;
use quinn::{ClientConfig, Endpoint};
use std::net::SocketAddr;
use std::sync::Arc;
use tokio::io::{AsyncBufReadExt, AsyncReadExt, AsyncWriteExt, BufReader};
use tokio_util::sync::CancellationToken;

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
        // Skip certificate verification for demo (self-signed cert)
        let crypto = rustls::ClientConfig::builder()
            .with_safe_defaults()
            .with_custom_certificate_verifier(Arc::new(SkipServerVerification::new()))
            .with_no_client_auth();
        
        ClientConfig::new(Arc::new(crypto))
    }

    pub async fn connect_and_chat(&self, server_addr: SocketAddr) -> Result<()> {
        println!("🔗 Connecting to server at {}...", server_addr);
        
        let connection = self.endpoint.connect(server_addr, "localhost")?.await?;
        println!("✅ Connected to server successfully!");
        println!("💬 You can now type messages and press Enter to send them.");
        println!("📨 You will also receive messages from the server.");
        println!("🚪 Type 'quit', 'bye', or 'exit' to close the connection.");
        println!("---");

        let (mut send, mut recv) = connection.open_bi().await?;
        let cancel_token = CancellationToken::new();

        // Handle incoming messages from server
        let recv_cancel = cancel_token.clone();
        let recv_task = tokio::spawn(async move {
            let mut buffer = [0u8; 1024];
            println!("📡 Listening for server messages...");
            
            loop {
                tokio::select! {
                    _ = recv_cancel.cancelled() => {
                        println!("📡 Stopping message receiver...");
                        break;
                    }
                    result = recv.read(&mut buffer) => {
                        match result {
                            Ok(Some(size)) => {
                                let message = String::from_utf8_lossy(&buffer[..size]);
                                println!("\n📨 {}", message.trim());
                                println!("💬 Your message: ");
                                use std::io::{self, Write};
                                io::stdout().flush().unwrap();
                            }
                            Ok(None) => {
                                println!("\n🔌 Server closed the connection");
                                break;
                            }
                            Err(e) => {
                                eprintln!("\n❌ Error reading from server: {}", e);
                                break;
                            }
                        }
                    }
                }
            }
        });

        // Handle user input and send messages
        let send_cancel = cancel_token.clone();
        let send_task = tokio::spawn(async move {
            let stdin = tokio::io::stdin();
            let mut reader = BufReader::new(stdin);
            let mut line = String::new();

            println!("💬 Your message: ");
            use std::io::{self, Write};
            io::stdout().flush().unwrap();
            
            loop {
                line.clear();
                
                tokio::select! {
                    _ = send_cancel.cancelled() => {
                        println!("📤 Stopping message sender...");
                        break;
                    }
                    result = reader.read_line(&mut line) => {
                        match result {
                            Ok(0) => {
                                println!("📝 EOF reached, closing connection...");
                                break;
                            }
                            Ok(_) => {
                                let message = line.trim();
                                
                                if message.is_empty() {
                                    println!("💬 Your message: ");
                                    io::stdout().flush().unwrap();
                                    continue;
                                }
                                
                                let client_message = format!("👤 Client: {}", message);
                                println!("📤 Sending: '{}'", message);
                                
                                // Send message to server
                                match send.write_all(client_message.as_bytes()).await {
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
                                
                                // Check if user wants to quit
                                let msg_lower = message.to_lowercase();
                                if msg_lower.contains("quit") || msg_lower.contains("bye") || msg_lower.contains("exit") {
                                    println!("👋 Closing connection...");
                                    // Give server time to process quit message
                                    tokio::time::sleep(tokio::time::Duration::from_millis(500)).await;
                                    break;
                                }
                                
                                println!("💬 Your message: ");
                                io::stdout().flush().unwrap();
                            }
                            Err(e) => {
                                eprintln!("❌ Error reading input: {}", e);
                                break;
                            }
                        }
                    }
                }
            }
            
            let _ = send.finish().await;
        });

        // Wait for either task to complete
        tokio::select! {
            _ = recv_task => {
                println!("🔄 Receive task finished");
            }
            _ = send_task => {
                println!("🔄 Send task finished");
            }
        }

        // Cleanup
        println!("🧹 Cleaning up connection...");
        cancel_token.cancel();
        
        // Give tasks a moment to finish
        tokio::time::sleep(tokio::time::Duration::from_millis(1000)).await;
        
        connection.close(0u32.into(), b"client closing");
        println!("👋 Chat ended. Goodbye!");
        
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

#[tokio::main]
async fn main() -> Result<()> {
    println!("🎯 Starting Bidirectional QUIC Client...");
    
    let server_addr: SocketAddr = "127.0.0.1:4433".parse()?;
    let client = QuicClient::new()?;
    
    // Handle Ctrl+C gracefully
    let ctrl_c = tokio::signal::ctrl_c();
    
    tokio::select! {
        result = client.connect_and_chat(server_addr) => {
            match result {
                Ok(_) => println!("✅ Client finished successfully"),
                Err(e) => {
                    eprintln!("❌ Client error: {}", e);
                    eprintln!("💡 Make sure the server is running at {}", server_addr);
                }
            }
        }
        _ = ctrl_c => {
            println!("\n🛑 Received Ctrl+C, closing client...");
        }
    }
    
    Ok(())
}