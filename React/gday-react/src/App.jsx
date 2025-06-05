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
  const [message, setMessage] = useState({ epoch: 0, offset_minutes: 0, mem_used: 0, mem_total: 0 });
  const [networkInfo, setNetworkInfo] = useState(null);
  const [connectionStatus, setConnectionStatus] = useState('Attempting to connect...');
  const [isWsReady, setIsWsReady] = useState(false);
  const wsRef = useRef(null);
  const [wsServer, setWsServer] = useState(null);
  const [triggerStatus, setTriggerStatus] = useState(null);
  const [programs, setPrograms] = useState([]);

  // Dummy sensor data
  const sensors = [
    { id: 'temp1', name: 'Temperature XX' },
    { id: 'humid1', name: 'Humidity XX' },
    { id: 'volt1', name: 'Voltage XX' },
  ];

  const requestNetworkInfo = useCallback(() => {
    if (wsRef.current && wsRef.current.readyState === WebSocket.OPEN) {
      wsRef.current.send(JSON.stringify({ command: 'get_network_info' }));
    } else {
      console.warn('WebSocket not connected, cannot request network info');
    }
  }, []);

  const requestPrograms = useCallback(() => {
    if (wsRef.current && wsRef.current.readyState === WebSocket.OPEN) {
      for (let i = 1; i <= 10; i++) {
        const id = i.toString().padStart(2, '0');
        wsRef.current.send(JSON.stringify({ command: 'get_program', programID: id }));
      }
    } else {
      console.warn('WebSocket not connected, cannot request programs');
    }
  }, []);

  useEffect(() => {
    const fetchConfig = async () => {
      try {
        const response = await fetch(`http://${window.location.host}/config.json`);
        if (!response.ok) {
          setWsServer(`ws://gday.local/ws`);
          throw new Error('no config file, default hostname being used');
        }
        const config = await response.json();
        const hostname = config.mdns_hostname;
        if (!hostname) {
          throw new Error('mdns_hostname not found in config.json');
        }
        setWsServer(`ws://${hostname}.local/ws`);
      } catch (error) {
        console.error('Error fetching config, using default.', error);
        setWsServer(`ws://gday.local/ws`);
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
          setIsWsReady(true);
          retryDelay = 1000;
          requestPrograms();
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
              case 'save_program_response':
                console.log('Save program response:', data);
                if (data.success && data.programID) {
                  wsRef.current.send(JSON.stringify({ command: 'get_program', programID: data.programID }));
                }
                break;
              case 'get_program_response':
                if (data.success && data.content) {
                  const content = JSON.parse(data.content);
                  setPrograms((prev) => {
                    if (prev.some((p) => p.id === data.programID)) {
                      return prev.map((p) =>
                        p.id === data.programID ? { ...p, ...content, id: data.programID } : p
                      );
                    }
                    return [
                      ...prev,
                      { id: data.programID, ...content },
                    ];
                  });
                }
                break;
              case 'time_offset':
                setMessage((prev) => ({ ...prev, offset_minutes: data.offset_minutes }));
                break;
              case 'trigger_status':
                //console.log('Received trigger_status:', data);
                setTriggerStatus(data);
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
          setIsWsReady(false);
          reconnectTimeout = setTimeout(() => {
            connectWebSocket();
            retryDelay = Math.min(retryDelay * 2, 30000);
          }, retryDelay);
        }
      };

      ws.onerror = (error) => {
        if (isMounted) {
          console.error('WebSocket error:', error);
          setConnectionStatus('Connection failed - verify gday.local');
          setIsWsReady(false);
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
  }, [wsServer, requestPrograms]);

  return (
    <div className="App">
      <TimeBar message={message} wsRef={wsRef} />
      <Routes>
        <Route path="/" element={<Home message={message} wsRef={wsRef} isWsReady={isWsReady} triggerStatus={triggerStatus} programs={programs} />} />
        <Route path="/graph" element={<Graph />} />
        <Route path="/settings" element={<Settings requestNetworkInfo={requestNetworkInfo} networkInfo={networkInfo} connectionStatus={connectionStatus} />} />
        <Route path="/programs" element={<Programs wsRef={wsRef} isWsReady={isWsReady} triggerStatus={triggerStatus} programs={programs} />} />
        <Route path="/programEditor" element={<ProgramEditor wsRef={wsRef} isWsReady={isWsReady} programs={programs} sensors={sensors}/>}
/>
      </Routes>
    </div>
  );
}

export default App;