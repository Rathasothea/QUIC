function checkProtocol() {
    const protocolInfo = document.getElementById('protocol-info');
    
    // Check if HTTP/3 is being used
    if (navigator.connection) {
        protocolInfo.innerHTML = `
            <h3>Connection Information:</h3>
            <p><strong>Effective Type:</strong> ${navigator.connection.effectiveType}</p>
            <p><strong>Protocol:</strong> ${location.protocol}</p>
            <p><strong>Host:</strong> ${location.host}</p>
        `;
    } else {
        protocolInfo.innerHTML = `
            <h3>Connection Information:</h3>
            <p><strong>Protocol:</strong> ${location.protocol}</p>
            <p><strong>Host:</strong> ${location.host}</p>
            <p><em>Your browser supports HTTP/3 if this page loaded successfully over HTTPS!</em></p>
        `;
    }
}