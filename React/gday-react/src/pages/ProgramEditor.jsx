import { useState, useEffect, useRef } from 'react';
import { useLocation, useNavigate } from 'react-router-dom';
import '../styles.css';

function ProgramEditor({ wsRef, isWsReady }) {
  const [programID, setProgramID] = useState('00');
  const [programContent, setProgramContent] = useState('');
  const [error, setError] = useState(null);
  const [status, setStatus] = useState('');
  const location = useLocation();
  const navigate = useNavigate();
  const hasFetched = useRef(false);

  // Parse URL for programID on mount
  useEffect(() => {
    const params = new URLSearchParams(location.search);
    const id = params.get('programID');
    if (id) {
      const parsedID = parseInt(id, 10);
      if (!isNaN(parsedID) && parsedID >= 1 && parsedID <= 10) {
        const formattedID = parsedID.toString().padStart(2, '0');
        setProgramID(formattedID);
        hasFetched.current = false; // Reset to allow fetch for new URL ID
      } else {
        setError('Invalid programID in URL (must be 1 to 10)');
      }
    }
  }, [location]);

  // Fetch program when WebSocket is ready and programID is set
  useEffect(() => {
    if (!isWsReady || !programID || programID === '00' || hasFetched.current) return;

    wsRef.current.send(JSON.stringify({ command: 'get_program', programID }));
    setStatus(`Requesting program ${programID}`);
    hasFetched.current = true;
  }, [isWsReady, programID, wsRef]);

  // Handle WebSocket messages
  useEffect(() => {
    if (!wsRef.current) return;

    const handleMessage = (event) => {
      try {
        const data = JSON.parse(event.data);
        if (data.type === 'get_program_response') {
          if (data.success) {
            setProgramContent(JSON.stringify(JSON.parse(data.content), null, 2));
            setStatus(`Loaded program ${data.programID}`);
            setError(null);
          } else {
            setError(`Failed to load program ${data.programID}: ${data.message}`);
            setStatus('');
          }
        } else if (data.type === 'save_program_response') {
          if (data.success) {
            const assignedID = data.programID || programID;
            setProgramID(assignedID);
            setStatus(`Program ${assignedID} saved successfully`);
            setError(null);
            navigate(`/programEditor?programID=${assignedID}`);
          } else {
            setError(`Failed to save program: ${data.message}`);
            setStatus('');
          }
        }
      } catch (err) {
        setError('Error processing server response');
      }
    };

    wsRef.current.addEventListener('message', handleMessage);
    return () => {
      wsRef.current.removeEventListener('message', handleMessage);
    };
  }, [wsRef, isWsReady, programID, navigate]);

  // Sanitize and save the program via WebSocket
  const saveProgram = async () => {
    setError(null);
    setStatus('Saving...');

    let sanitizedContent;
    try {
      sanitizedContent = JSON.parse(programContent);
      sanitizedContent = JSON.stringify(sanitizedContent);
    } catch (err) {
      setError('Invalid JSON format');
      setStatus('');
      return;
    }

    const maxSize = 4096;
    if (sanitizedContent.length > maxSize) {
      setError(`Program size exceeds ${maxSize} bytes`);
      setStatus('');
      return;
    }

    if (wsRef.current && wsRef.current.readyState === WebSocket.OPEN) {
      const message = {
        command: 'save_program',
        programID: programID,
        content: sanitizedContent,
      };
      wsRef.current.send(JSON.stringify(message));
      setStatus(`Program ${programID} save requested`);
    } else {
      setError('WebSocket not connected');
      setStatus('');
    }
  };

  return (
    <div>
      <h1 className="Title">Program Editor</h1>
      {error && <div>{error}</div>}
      {status && <div>{status}</div>}
      <textarea
        className="w-full h-64 p-2 border rounded mb-4"
        value={programContent}
        onChange={(e) => setProgramContent(e.target.value)}
        placeholder="Enter your program (JSON format)"
      />
      <button
        className="bg-blue-500 hover:bg-blue-700 text-white font-bold py-2 px-4 rounded"
        onClick={saveProgram}
      >
        Save Program
      </button>
    </div>
  );
}

export default ProgramEditor;