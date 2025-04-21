import { useState, useEffect } from 'react';
import '../styles.css';

function Programs({ wsRef, isWsReady }) {
  const [programs, setPrograms] = useState([]);

  useEffect(() => {
    if (!isWsReady || !wsRef.current) return;
  
    for (let i = 1; i <= 10; i++) {
      const id = i.toString().padStart(2, '0');
      wsRef.current.send(JSON.stringify({ command: 'get_program', programID: id }));
    }
  
    const handleMessage = (event) => {
      try {
        const data = JSON.parse(event.data);
        if (data.type === 'get_program_response' && data.success && data.content) {
          const content = JSON.parse(data.content);
          setPrograms((prev) => {
            // Avoid duplicates by checking if id already exists
            if (prev.some((p) => p.id === data.programID)) return prev;
            return [
              ...prev,
              {
                id: data.programID,
                name: 'name' in content ? content.name : null,
              },
            ];
          });
        }
      } catch (err) {
        console.error('Programs: Failed to parse WebSocket message:', err);
      }
    };
  
    wsRef.current.addEventListener('message', handleMessage);
    return () => {
      wsRef.current.removeEventListener('message', handleMessage);
    };
  }, [isWsReady, wsRef]);

  useEffect(() => {
    console.log('Programs: Current programs array:', programs);
  }, [programs]);

  return (
    <div className="container mx-auto p-4">
      <h1 className="text-2xl font-bold mb-4">Programs</h1>
    </div>
  );
}

export default Programs;