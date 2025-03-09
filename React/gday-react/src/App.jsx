import { useState, useEffect, useRef, useCallback } from 'react';
import { Routes, Route } from 'react-router-dom';
import TimeBar from './pages/TimeBar';
import Home from './pages/Home';
import Graph from './pages/Graph';
import Settings from './pages/Settings';
import './styles.css';

function App() {
  const [message, setMessage] = useState(null);
  const [networkInfo, setNetworkInfo] = useState(null);
  const [connectionStatus, setConnectionStatus] = useState('Attempting to connect...');
  const wsRef = useRef(null);

  const requestNetworkInfo = useCallback(() => {
    if (wsRef.current && wsRef.current.readyState === WebSocket.OPEN) {
      wsRef.current.send(JSON.stringify({ command: 'get_network_info' }));
    }
  }, []);

  useEffect(() => {
    let ws;
    const wsServer = 'ws://gday.local/ws';

    const connectWebSocket = () => {
      console.log(`Attempting to connect to ${wsServer}`);
      setConnectionStatus('Attempting to connect...');
      ws = new WebSocket(wsServer);
      wsRef.current = ws;

      ws.onopen = () => {
        console.log('WebSocket connection opened');
        setConnectionStatus('Connected');
      };

      ws.onmessage = (event) => {
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
          console.error('Failed to parse WebSocket message as JSON:', error);
        }
      };

      ws.onclose = () => {
        console.log('WebSocket connection closed');
        setConnectionStatus('Disconnected');
        setTimeout(connectWebSocket, 5000);
      };

      ws.onerror = (error) => {
        console.error('WebSocket error:', error);
        setConnectionStatus('Connection failed - verify gday.local');
        ws.close();
      };
    };

    connectWebSocket();
    return () => {
      if (wsRef.current) wsRef.current.close();
    };
  }, []);

  return (
    <div className="App">
      <TimeBar message={message} connectionStatus={connectionStatus} />
      <Routes>
        <Route path="/" element={<Home />} />
        <Route path="/graph" element={<Graph />} />
        <Route path="/settings" element={<Settings requestNetworkInfo={requestNetworkInfo} networkInfo={networkInfo} connectionStatus={connectionStatus} />} />
      </Routes>
    </div>
  );
}

export default App;