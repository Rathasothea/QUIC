// server.rs - Improved bidirectional communication
use anyhow::Result;
use quinn::{Endpoint, ServerConfig};
use std::net::SocketAddr;
use std::sync::Arc;
use tokio::io::{AsyncBufReadExt, AsyncReadExt, AsyncWriteExt, BufReader};
use tokio::sync::mpsc;
use tokio_util::sync::CancellationToken;

pub struct QuicServer {
    endpoint: Endpoint,
}

impl QuicServer {
    pub fn new(addr: SocketAddr) -> Result<Self> {
        let server_config = Self::configure_server()?;
        let endpoint = Endpoint::server(server_config, addr)?;
        
        println!("🚀 QUIC Server listening on: {}", addr);
        println!("📝 Waiting for clients to connect...");
        println!("💡 Server can send and receive messages");
        println!("📨 Type messages to send to all connected clients");
        println!("---");
        Ok(QuicServer { endpoint })
    }

    fn configure_server() -> Result<ServerConfig> {
        // Generate self-signed certificate for demo
        let cert = rcgen::generate_simple_self_signed(vec!["localhost".into()])?;
        let cert_der = cert.serialize_der()?;
        let priv_key = cert.serialize_private_key_der();
        
        let cert_chain = vec![rustls::Certificate(cert_der)];
        let key_der = rustls::PrivateKey(priv_key);
        
        let server_config = ServerConfig::with_single_cert(cert_chain, key_der)?;
        Ok(server_config)
    }

    pub async fn run(&mut self) -> Result<()> {
        // Channel for broadcasting server messages to all clients
        let (broadcast_tx, _) = tokio::sync::broadcast::channel::<String>(100);
        let broadcast_tx = Arc::new(broadcast_tx);
        
        // Start server input handler
        let server_broadcast_tx = broadcast_tx.clone();
        tokio::spawn(async move {
            Self::handle_server_input(server_broadcast_tx).await;
        });

        while let Some(conn) = self.endpoint.accept().await {
            match conn.await {
                Ok(connection) => {
                    println!("✅ New client connected from: {}", connection.remote_address());
                    
                    let client_broadcast_rx = broadcast_tx.subscribe();
                    tokio::spawn(async move {
                        if let Err(e) = Self::handle_connection(connection, client_broadcast_rx).await {
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

    async fn handle_server_input(broadcast_tx: Arc<tokio::sync::broadcast::Sender<String>>) {
        println!("💬 Server ready for input. Type messages to send to clients:");
        
        let stdin = tokio::io::stdin();
        let mut reader = BufReader::new(stdin);
        let mut line = String::new();

        loop {
            line.clear();
            match reader.read_line(&mut line).await {
                Ok(0) => break, // EOF
                Ok(_) => {
                    let message = line.trim();
                    if !message.is_empty() {
                        println!("📤 Server broadcasting: '{}'", message);
                        let server_msg = format!("🖥️ Server: {}", message);
                        let _ = broadcast_tx.send(server_msg);
                    }
                }
                Err(e) => {
                    eprintln!("❌ Error reading server input: {}", e);
                    break;
                }
            }
        }
    }

    async fn handle_connection(
        conn: quinn::Connection, 
        mut broadcast_rx: tokio::sync::broadcast::Receiver<String>
    ) -> Result<()> {
        println!("🔗 Handling connection from: {}", conn.remote_address());
        let remote_addr = conn.remote_address();
        
        loop {
            match conn.accept_bi().await {
                Ok((send, recv)) => {
                    println!("📡 New bidirectional stream opened for {}", remote_addr);
                    
                    let stream_broadcast_rx = broadcast_rx.resubscribe();
                    tokio::spawn(async move {
                        if let Err(e) = Self::handle_stream(send, recv, stream_broadcast_rx, remote_addr).await {
                            eprintln!("❌ Stream error for {}: {}", remote_addr, e);
                        }
                    });
                }
                Err(quinn::ConnectionError::ApplicationClosed(_)) => {
                    println!("🔌 Client {} disconnected gracefully", remote_addr);
                    break;
                }
                Err(e) => {
                    eprintln!("❌ Error accepting stream from {}: {}", remote_addr, e);
                    break;
                }
            }
        }
        Ok(())
    }

    async fn handle_stream(
        mut send: quinn::SendStream,
        mut recv: quinn::RecvStream,
        mut broadcast_rx: tokio::sync::broadcast::Receiver<String>,
        remote_addr: SocketAddr,
    ) -> Result<()> {
        let cancel_token = CancellationToken::new();
        
        // Handle incoming messages from client
        let recv_cancel = cancel_token.clone();
        let recv_task = tokio::spawn(async move {
            let mut buffer = [0u8; 1024];
            
            loop {
                tokio::select! {
                    _ = recv_cancel.cancelled() => break,
                    result = recv.read(&mut buffer) => {
                        match result {
                            Ok(Some(size)) => {
                                let message = String::from_utf8_lossy(&buffer[..size]);
                                let trimmed_message = message.trim();
                                
                                println!("📨 Received from {}: '{}'", remote_addr, trimmed_message);
                                
                                // Check for quit commands
                                if trimmed_message.to_lowercase().contains("quit") 
                                    || trimmed_message.to_lowercase().contains("bye") 
                                    || trimmed_message.to_lowercase().contains("exit") {
                                    println!("👋 Client {} requested to close", remote_addr);
                                    break;
                                }
                            }
                            Ok(None) => {
                                println!("🔌 Client {} closed the stream", remote_addr);
                                break;
                            }
                            Err(e) => {
                                eprintln!("❌ Error reading from {}: {}", remote_addr, e);
                                break;
                            }
                        }
                    }
                }
            }
        });

        // Handle outgoing messages to client (from server broadcasts)
        let send_cancel = cancel_token.clone();
        let send_task = tokio::spawn(async move {
            loop {
                tokio::select! {
                    _ = send_cancel.cancelled() => break,
                    result = broadcast_rx.recv() => {
                        match result {
                            Ok(message) => {
                                println!("📤 Sending to {}: '{}'", remote_addr, message);
                                if let Err(e) = send.write_all(message.as_bytes()).await {
                                    eprintln!("❌ Error sending to {}: {}", remote_addr, e);
                                    break;
                                }
                                if let Err(e) = send.flush().await {
                                    eprintln!("❌ Error flushing to {}: {}", remote_addr, e);
                                    break;
                                }
                            }
                            Err(tokio::sync::broadcast::error::RecvError::Closed) => {
                                println!("📡 Broadcast channel closed");
                                break;
                            }
                            Err(tokio::sync::broadcast::error::RecvError::Lagged(_)) => {
                                println!("⚠️ Client {} lagged behind, continuing...", remote_addr);
                                continue;
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
                println!("🔄 Receive task finished for {}", remote_addr);
            }
            _ = send_task => {
                println!("🔄 Send task finished for {}", remote_addr);
            }
        }

        cancel_token.cancel();
        println!("✅ Stream closed successfully for {}", remote_addr);
        Ok(())
    }
}

#[tokio::main]
async fn main() -> Result<()> {
    println!("🎯 Starting Bidirectional QUIC Server...");
    
    let server_addr: SocketAddr = "127.0.0.1:4433".parse()?;
    let mut server = QuicServer::new(server_addr)?;
    
    // Handle Ctrl+C gracefully
    let ctrl_c = tokio::signal::ctrl_c();
    
    tokio::select! {
        result = server.run() => {
            match result {
                Ok(_) => println!("✅ Server shut down gracefully"),
                Err(e) => eprintln!("❌ Server error: {}", e),
            }
        }
        _ = ctrl_c => {
            println!("\n🛑 Received Ctrl+C, shutting down server...");
        }
    }
    
    Ok(())
}