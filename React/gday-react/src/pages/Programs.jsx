import { useState, useEffect } from 'react';
import { Link } from 'react-router-dom';
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

  return (
    <div>
      <div className="Title">
        <h1 >Programs</h1>
      </div>
      <div className="Tile-container">
        <table className="Tile">
          <thead>
            <tr>
              <th>Program Name</th>
            </tr>
          </thead>
          <tbody>
            {programs.length === 0 ? (
              <tr>
                <td>No programs found</td>
              </tr>
            ) : (
              programs.map((program) => (
                <tr key={program.id}>
                  <td>
                    <Link
                      to={`/programEditor?programID=${program.id}`}
                    >
                      {program.name || `Program${program.id}`}
                    </Link>
                  </td>
                </tr>
              ))
            )}
          </tbody>
          <Link
            to={`/programEditor`}
          >
            {`New Program`}
          </Link>
        </table>
      </div>
    </div>
  );
}

export default Programs;