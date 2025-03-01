import { useState, useEffect, useRef } from 'react';
import './styles.css';

function TimeBar() {
  const [data, setData] = useState({ epoch: null, mem_used: null, mem_total: null, lastMessage: Date.now() });
  const [isConnected, setIsConnected] = useState(false);
  const [disconnectTime, setDisconnectTime] = useState(null);
  const [isOverlayOpen, setIsOverlayOpen] = useState(false);
  const [hasConnectedOnce, setHasConnectedOnce] = useState(false);
  const [wsServer, setWsServer] = useState(`ws://${window.location.hostname}/ws`);
  const wsRef = useRef(null);
  const HEARTBEAT_TIMEOUT = 15000;
  const RECONNECT_DELAY = 5000;

  const connectWebSocket = () => {
    if (wsRef.current) wsRef.current.close();
    wsRef.current = new WebSocket(wsServer);

    wsRef.current.onopen = () => {
      console.log('WebSocket connection opened');
      setIsConnected(true);
      setDisconnectTime(null);
      setHasConnectedOnce(true);
    };

    wsRef.current.onmessage = (event) => {
      const newData = JSON.parse(event.data);
      if (newData.epoch) {
        setData({ ...newData, lastMessage: Date.now() });
      }
    };

    wsRef.current.onclose = () => {
      console.log('WebSocket connection closed');
      setIsConnected(false);
      if (hasConnectedOnce) {
        setDisconnectTime(new Date(Date.now() - HEARTBEAT_TIMEOUT));
      }
      setTimeout(connectWebSocket, RECONNECT_DELAY);
    };

    wsRef.current.onerror = (error) => {
      console.error('WebSocket error:', error);
      wsRef.current.close();
    };
  };

  useEffect(() => {
    let heartbeatInterval;

    connectWebSocket();
    heartbeatInterval = setInterval(() => {
      if (wsRef.current.readyState === WebSocket.OPEN && Date.now() - data.lastMessage > HEARTBEAT_TIMEOUT) {
        console.log('Heartbeat timeout: No messages for 15s. Closing connection.');
        wsRef.current.close();
      }
    }, 5000);

    return () => {
      clearInterval(heartbeatInterval);
    };
  }, [wsServer]);

  const formatTime = (date) => {
    if (!date) return 'N/A';
    const year = date.getFullYear();
    const month = String(date.getMonth() + 1).padStart(2, '0');
    const day = String(date.getDate()).padStart(2, '0');
    const hours = String(date.getHours()).padStart(2, '0');
    const minutes = String(date.getMinutes()).padStart(2, '0');
    const seconds = String(date.getSeconds()).padStart(2, '0');
    return `${year}-${month}-${day} ${hours}:${minutes}:${seconds}`;
  };

  const syncTimeAndTimezone = () => {
    const clientTime = new Date();
    const timeStr = formatTime(clientTime);
    const offset = -clientTime.getTimezoneOffset() / 60;
    const offsetStr = offset >= 0 ? `+${offset}` : `${offset}`;
    const message = { command: 'sync_time', time: timeStr, offset: offsetStr };
    if (wsRef.current && wsRef.current.readyState === WebSocket.OPEN) {
      wsRef.current.send(JSON.stringify(message));
      console.log('Sent to server:', message);
      alert(`Sent time: ${timeStr} and offset: ${offsetStr} to server.`);
      setIsOverlayOpen(false);
    } else {
      alert('Cannot sync time: WebSocket is not connected.');
    }
  };

  const getHostname = (url) => {
    try {
      const urlObj = new URL(url.replace('ws://', 'http://'));
      return urlObj.hostname;
    } catch {
      return url;
    }
  };

  const setCustomServer = () => {
    const currentHost = getHostname(wsServer);
    const newServer = prompt('Enter WebSocket server hostname (e.g., gday.local):', currentHost);
    if (newServer) {
      const fullServer = `ws://${newServer}/ws`;
      setWsServer(fullServer);
      if (wsRef.current) wsRef.current.close();
    }
  };

  const serverHost = getHostname(wsServer);
  const memPercent = data.mem_total ? Math.round(((data.mem_total - data.mem_used) / data.mem_total) * 100) : 0;
  const timeStr = data.epoch ? formatTime(new Date(data.epoch * 1000)) : 'Waiting to connect';
  const statusText = isConnected
    ? `${timeStr} (Browser time) ${memPercent}% memory free`
    : disconnectTime
      ? `Lost connection at ${formatTime(disconnectTime)}. ${hasConnectedOnce ? 'Waiting to reconnect' : 'Waiting to connect'}`
      : 'Waiting to connect';
  const timeDiff = data.epoch ? Math.abs(new Date() - new Date(data.epoch * 1000)) / 1000 : 0;

  return (
    <>
      <div id="serverBar" className="server-bar">
        <a href="#" onClick={setCustomServer}>
          Server: {serverHost}
        </a>
      </div>
      <div id="timeBar" className="time-bar">
        <a
          href="#"
          onClick={() => setIsOverlayOpen(true)}
          style={{ color: timeDiff > 5 ? 'red' : 'black' }}
        >
          {statusText}
        </a>
      </div>
      {isOverlayOpen && (
        <div
          id="syncOverlay"
          className="overlay"
          onClick={(e) => e.target.id === 'syncOverlay' && setIsOverlayOpen(false)}
        >
          <div className="overlay-content">
            <div id="serverData">
              <p>Time: <span>{data.epoch ? formatTime(new Date(data.epoch * 1000)) : 'N/A'}</span></p>
              <p>Heap Used (KB): <span>{data.mem_used ? (data.mem_used / 1024).toFixed(2) : 'N/A'}</span></p>
              <p>Heap Available (KB): <span>{data.mem_total ? (data.mem_total / 1024).toFixed(2) : 'N/A'}</span></p>
            </div>
            <h2>Sync Time</h2>
            <p>Click to sync your local time with the server.</p>
            <button onClick={syncTimeAndTimezone}>Sync Local Time</button>
          </div>
        </div>
      )}
    </>
  );
}

export default TimeBar;