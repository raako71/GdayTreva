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
    <div className="container mx-auto p-4">
      <h1 className="text-2xl font-bold mb-4">Programs</h1>
      <table className="table-auto w-full border-collapse border border-gray-300">
        <thead>
          <tr className="bg-gray-100">
            <th className="border border-gray-300 p-2">Program Name</th>
          </tr>
        </thead>
        <tbody>
          {programs.length === 0 ? (
            <tr>
              <td className="border border-gray-300 p-2 text-center">No programs found</td>
            </tr>
          ) : (
            programs.map((program) => (
              <tr key={program.id} className="hover:bg-gray-50">
                <td className="border border-gray-300 p-2">
                  <Link
                    to={`/programEditor?programID=${program.id}`}
                    className="text-blue-500 hover:underline"
                  >
                    {program.name || `Program${program.id}`}
                  </Link>
                </td>
              </tr>
            ))
          )}
        </tbody>
      </table>
    </div>
  );
}

export default Programs;