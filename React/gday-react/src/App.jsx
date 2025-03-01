import { useState, useEffect, useRef } from 'react';
import { Routes, Route } from 'react-router-dom';
import TimeBar from './pages/TimeBar';
import Home from './pages/Home';
import Graph from './pages/Graph';
import './styles.css';

function App() {
  const [epoch, setEpoch] = useState(null);
  const wsRef = useRef(null);

  useEffect(() => {
    let ws;
    const wsServer = 'ws://192.168.1.10/ws'; // Hardcoded default

    const connectWebSocket = () => {
      console.log(`Attempting to connect to ${wsServer}`);
      ws = new WebSocket(wsServer);
      wsRef.current = ws;

      ws.onopen = () => {
        console.log('WebSocket connection opened');
      };

      ws.onmessage = (event) => {
        const data = JSON.parse(event.data);
        if (data.epoch) {
          setEpoch(data.epoch);
          console.log(`Epoch received: ${data.epoch}`);
        }
      };

      ws.onclose = () => {
        console.log('WebSocket connection closed');
        setTimeout(connectWebSocket, 5000);
      };

      ws.onerror = (error) => {
        console.error('WebSocket error:', error);
        ws.close();
      };
    };

    connectWebSocket();
    return () => {
      if (wsRef.current) wsRef.current.close();
    };
  }, []); // Runs once on mount

  return (
    <div className="App">
      <TimeBar epoch={epoch} />
      <Routes>
        <Route path="/" element={<Home />} />
        <Route path="/graph" element={<Graph />} />
      </Routes>
    </div>
  );
}

export default App;