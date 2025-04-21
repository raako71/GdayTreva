import { useState, useEffect } from 'react';
import { useLocation } from 'react-router-dom';
import '../styles.css';

function ProgramEditor({ wsRef }) {
  const [programID, setProgramID] = useState('00');
  const [programContent, setProgramContent] = useState('');
  const [error, setError] = useState(null);
  const [status, setStatus] = useState('');
  const location = useLocation();

  // Parse URL for programID on mount
  useEffect(() => {
    const params = new URLSearchParams(location.search);
    const id = params.get('programID');
    if (id) {
      const parsedID = parseInt(id, 10);
      if (!isNaN(parsedID) && parsedID >= 1 && parsedID <= 10) {
        const formattedID = parsedID.toString().padStart(2, '0');
        setProgramID(formattedID);
      } else {
        setError('Invalid programID in URL (must be 1 to 10)');
      }
    }
  }, [location]);

  // Fetch program when WebSocket is ready
  useEffect(() => {
    console.log('ProgramEditor: fetchProgram useEffect triggered, wsRef:', wsRef, 'programID:', programID);
    if (!wsRef.current || !programID || programID === '00') {
      console.log('ProgramEditor: Skipping fetchProgram, invalid wsRef or programID');
      return;
    }
  
    const fetchProgram = () => {
      if (wsRef.current.readyState === WebSocket.OPEN) {
        console.log('ProgramEditor: Sending get_program request for ID:', programID);
        wsRef.current.send(JSON.stringify({ command: 'get_program', programID }));
        setStatus(`Requesting program ${programID}`);
      } else {
        console.log('ProgramEditor: WebSocket not open, readyState:', wsRef.current.readyState);
      }
    };
  
    fetchProgram();
    if (wsRef.current.readyState !== WebSocket.OPEN) {
      const handleOpen = () => {
        console.log('ProgramEditor: WebSocket opened, sending get_program');
        fetchProgram();
      };
      wsRef.current.addEventListener('open', handleOpen);
      return () => {
        console.log('ProgramEditor: Cleaning up open listener');
        wsRef.current.removeEventListener('open', handleOpen);
      };
    }
  }, [wsRef, programID]);

  // Sanitize and save the program via WebSocket
  const saveProgram = async () => {
    setError(null);
    setStatus('Saving...');

    // Sanitize input
    let sanitizedContent;
    try {
      sanitizedContent = JSON.parse(programContent);
      sanitizedContent = JSON.stringify(sanitizedContent); // Normalize
    } catch (err) {
      setError('Invalid JSON format');
      setStatus('');
      return;
    }

    // Check size (max 4KB)
    const maxSize = 4096;
    if (sanitizedContent.length > maxSize) {
      setError(`Program size exceeds ${maxSize} bytes`);
      setStatus('');
      return;
    }

    // Send program via WebSocket
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
          } else {
            setError(`Failed to save program: ${data.message}`);
            setStatus('');
          }
        }
      } catch (err) {
        console.error('Failed to parse WebSocket message:', err);
        setError('Error processing server response');
      }
    };

    wsRef.current.addEventListener('message', handleMessage);
    return () => {
      wsRef.current.removeEventListener('message', handleMessage);
    };
  }, [wsRef, programID]);

  return (
    <div className="container mx-auto p-4">
      <h1 className="text-2xl font-bold mb-4">Program Editor</h1>
      {error && <div className="text-red-500 mb-4">{error}</div>}
      {status && <div className="text-green-500 mb-4">{status}</div>}
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