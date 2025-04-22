import { useState, useEffect, useRef } from 'react';
import { useLocation, useNavigate } from 'react-router-dom';
import '../styles.css';

function ProgramEditor({ wsRef, isWsReady }) {
  const [programID, setProgramID] = useState('00');
  const [name, setName] = useState('');
  const [enabled, setEnabled] = useState(false);
  const [output, setOutput] = useState('A');
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
        hasFetched.current = false;
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
            const content = JSON.parse(data.content);
            setName(content.name || '');
            setEnabled(content.enabled || false);
            setOutput(content.output || 'A');
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
            navigate('/programs'); // Redirect to /programs
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

    const programContent = { name, enabled, output };
    let sanitizedContent;
    try {
      sanitizedContent = JSON.stringify(programContent);
    } catch (err) {
      setError('Error serializing program data');
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
      {error && <div className="error">{error}</div>}
      {status && <div className="status">{status}</div>}
      <div className="form-group">
        <label htmlFor="name">Name:</label>
        <input
          id="name"
          type="text"
          value={name}
          onChange={(e) => setName(e.target.value)}
          placeholder="Enter program name"
          className="input-field"
        />
      </div>
      <div className="form-group">
        <label>Enabled:</label>
        <label className="toggle-switch">
          <input
            type="checkbox"
            checked={enabled}
            onChange={(e) => setEnabled(e.target.checked)}
          />
          <span className="toggle-slider enabled-slider"></span>
        </label>
      </div>
      <div className="form-group">
        <label>Output:</label>
        <div className="output-toggle">
          <span className="output-option">A</span>
          <label className="toggle-switch">
            <input
              type="checkbox"
              checked={output === 'B'}
              onChange={(e) => setOutput(e.target.checked ? 'B' : 'A')}
            />
            <span className="toggle-slider output-slider"></span>
          </label>
          <span className="output-option">B</span>
        </div>
      </div>
      <button
        className="save-button"
        onClick={saveProgram}
      >
        Save Program
      </button>
    </div>
  );
}

export default ProgramEditor;