import { useState, useEffect, useRef, useCallback } from 'react';
import { Routes, Route } from 'react-router-dom';
import TimeBar from './pages/TimeBar';
import Home from './pages/Home';
import Graph from './pages/Graph';
import Settings from './pages/Settings';
import Programs from './pages/Programs';
import ProgramEditor from './pages/ProgramEditor';

import './styles.css';

function App() {
  const [message, setMessage] = useState(null);
  const [networkInfo, setNetworkInfo] = useState(null);
  const [connectionStatus, setConnectionStatus] = useState('Attempting to connect...');
  const [lastError, setLastError] = useState(null);
  const wsRef = useRef(null);
  const [wsServer, setWsServer] = useState(null);

  const requestNetworkInfo = useCallback(() => {
    if (wsRef.current && wsRef.current.readyState === WebSocket.OPEN) {
      wsRef.current.send(JSON.stringify({ command: 'get_network_info' }));
    } else {
      console.warn('WebSocket not connected, cannot request network info');
    }
  }, []);

  useEffect(() => {
    const fetchConfig = async () => {
      try {
        // Use window.location.host to dynamically get the ESP32's address
        const response = await fetch(`http://${window.location.host}/config.json`);
        if (!response.ok) {
          throw new Error('Failed to fetch config.json');
        }
        const config = await response.json();
        const hostname = config.mdns_hostname;
        if (!hostname) {
          throw new Error('mdns_hostname not found in config.json');
        }
        // Set WebSocket URL (e.g., ws://gday.local/ws)
        setWsServer(`ws://${hostname}.local/ws`);
      } catch (error) {
        console.error('Error fetching config:', error);
        // Fallback to IP address
        setWsServer(`ws://${window.location.hostname}/ws`);
      }
    };

    fetchConfig();

    return () => {
      if (wsRef.current) wsRef.current.close();
    };
  }, []);

  useEffect(() => {
    if (!wsServer) return;

    let isMounted = true;
    let reconnectTimeout;
    let retryDelay = 1000;

    const connectWebSocket = () => {
      console.log(`Attempting to connect to ${wsServer}`);
      setConnectionStatus('Attempting to connect...');
      const ws = new WebSocket(wsServer);
      wsRef.current = ws;

      ws.onopen = () => {
        if (isMounted) {
          console.log('WebSocket connection opened');
          setConnectionStatus('Connected');
          setLastError(null);
          retryDelay = 1000; // Reset retry delay
        }
      };

      ws.onmessage = (event) => {
        if (isMounted) {
          try {
            const data = JSON.parse(event.data);
            if (!data.type) {
              console.warn('Message missing type:', data);
              return;
            }
            switch (data.type) {
              case 'time':
                setMessage(data);
                break;
              case 'network_info':
                setNetworkInfo(data);
                break;
              default:
                console.warn('Unknown message type:', data.type);
            }
          } catch (error) {
            console.error('Failed to parse WebSocket message:', event.data, error);
          }
        }
      };

      ws.onclose = () => {
        if (isMounted) {
          console.log('WebSocket connection closed');
          setConnectionStatus('Disconnected');
          reconnectTimeout = setTimeout(() => {
            connectWebSocket();
            retryDelay = Math.min(retryDelay * 2, 30000); // Max 30s
          }, retryDelay);
        }
      };

      ws.onerror = (error) => {
        if (isMounted) {
          console.error('WebSocket error:', error);
          setConnectionStatus('Connection failed - verify gday.local');
          setLastError('Connection failed - verify gday.local');
          ws.close();
        }
      };
    };

    connectWebSocket();

    return () => {
      isMounted = false;
      clearTimeout(reconnectTimeout);
      if (wsRef.current) wsRef.current.close();
    };
  }, [wsServer]);

  return (
    <div className="App">
      <TimeBar message={message} connectionStatus={connectionStatus} lastError={lastError} />
      <Routes>
        <Route path="/" element={<Home />} />
        <Route path="/graph" element={<Graph />} />
        <Route path="/settings" element={<Settings requestNetworkInfo={requestNetworkInfo} networkInfo={networkInfo} connectionStatus={connectionStatus} />} />
        <Route path="/programs" element={<Programs />} />
        <Route path="/programEditor" element={<ProgramEditor />} />
      </Routes>
    </div>
  );
}

export default App;