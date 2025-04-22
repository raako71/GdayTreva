import { useState, useEffect, useRef } from 'react';
import { useLocation, useNavigate } from 'react-router-dom';
import '../styles.css';

function ProgramEditor({ wsRef, isWsReady }) {
  const [programID, setProgramID] = useState('00');
  const [name, setName] = useState('');
  const [enabled, setEnabled] = useState(false);
  const [output, setOutput] = useState('A');
  const [startDate, setStartDate] = useState('');
  const [startDateEnabled, setStartDateEnabled] = useState(true);
  const [endDate, setEndDate] = useState('');
  const [endDateEnabled, setEndDateEnabled] = useState(true);
  const [startTime, setStartTime] = useState('');
  const [startTimeEnabled, setStartTimeEnabled] = useState(true);
  const [endTime, setEndTime] = useState('');
  const [endTimeEnabled, setEndTimeEnabled] = useState(true);
  const [selectedDays, setSelectedDays] = useState([]);
  const [trigger, setTrigger] = useState('Manual');
  const [runTime, setRunTime] = useState({ seconds: '', minutes: '', hours: '' });
  const [stopTime, setStopTime] = useState({ seconds: '', minutes: '', hours: '' });
  const [error, setError] = useState(null);
  const [status, setStatus] = useState('');
  const location = useLocation();
  const navigate = useNavigate();
  const hasFetched = useRef(false);

  // List of days for checkboxes
  const daysOfWeek = [
    'Monday',
    'Tuesday',
    'Wednesday',
    'Thursday',
    'Friday',
    'Saturday',
    'Sunday',
  ];

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
            setStartDate(content.startDate || '');
            setStartDateEnabled(content.startDateEnabled !== false);
            setEndDate(content.endDate || '');
            setEndDateEnabled(content.endDateEnabled !== false);
            setStartTime(content.startTime || '');
            setStartTimeEnabled(content.startTimeEnabled !== false);
            setEndTime(content.endTime || '');
            setEndTimeEnabled(content.endTimeEnabled !== false);
            setSelectedDays(content.selectedDays || []);
            setTrigger(content.trigger || 'Manual');
            setRunTime({
              seconds: content.runTime?.seconds?.toString() || '',
              minutes: content.runTime?.minutes?.toString() || '',
              hours: content.runTime?.hours?.toString() || '',
            });
            setStopTime({
              seconds: content.stopTime?.seconds?.toString() || '',
              minutes: content.stopTime?.minutes?.toString() || '',
              hours: content.stopTime?.hours?.toString() || '',
            });
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
            navigate('/programs');
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

  // Toggle day selection
  const toggleDay = (day) => {
    setSelectedDays((prev) =>
      prev.includes(day)
        ? prev.filter((d) => d !== day)
        : [...prev, day]
    );
  };

  // Update runTime or stopTime fields
  const updateTimeField = (field, subfield, value) => {
    const setField = field === 'runTime' ? setRunTime : setStopTime;
    setField((prev) => ({
      ...prev,
      [subfield]: value,
    }));
  };

  // Sanitize and save the program via WebSocket
  const saveProgram = async () => {
    setError(null);
    setStatus('Saving...');

    const programContent = {
      name,
      enabled,
      output,
      startDate: startDateEnabled ? startDate : '',
      startDateEnabled,
      endDate: endDateEnabled ? endDate : '',
      endDateEnabled,
      startTime: startTimeEnabled ? startTime : '',
      startTimeEnabled,
      endTime: endTimeEnabled ? endTime : '',
      endTimeEnabled,
      selectedDays,
      trigger,
      ...(trigger === 'Cycle Timer' && {
        runTime: {
          seconds: parseInt(runTime.seconds) || 0,
          minutes: parseInt(runTime.minutes) || 0,
          hours: parseInt(runTime.hours) || 0,
        },
        stopTime: {
          seconds: parseInt(stopTime.seconds) || 0,
          minutes: parseInt(stopTime.minutes) || 0,
          hours: parseInt(stopTime.hours) || 0,
        },
      }),
    };
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
        <label>Start Date Enabled:</label>
        <label className="toggle-switch">
          <input
            type="checkbox"
            checked={startDateEnabled}
            onChange={(e) => setStartDateEnabled(e.target.checked)}
          />
          <span className="toggle-slider enabled-slider"></span>
        </label>
      </div>
      {startDateEnabled && (
        <div className="form-group">
          <label htmlFor="startDate">Start Date:</label>
          <input
            id="startDate"
            type="date"
            value={startDate}
            onChange={(e) => setStartDate(e.target.value)}
            className="input-field"
          />
        </div>
      )}
      <div className="form-group">
        <label>Start Time Enabled:</label>
        <label className="toggle-switch">
          <input
            type="checkbox"
            checked={startTimeEnabled}
            onChange={(e) => setStartTimeEnabled(e.target.checked)}
          />
          <span className="toggle-slider enabled-slider"></span>
        </label>
      </div>
      {startTimeEnabled && (
        <div className="form-group">
          <label htmlFor="startTime">Start Time:</label>
          <input
            id="startTime"
            type="time"
            value={startTime}
            onChange={(e) => setStartTime(e.target.value)}
            className="input-field"
          />
        </div>
      )}
      <div className="form-group">
        <label>End Date Enabled:</label>
        <label className="toggle-switch">
          <input
            type="checkbox"
            checked={endDateEnabled}
            onChange={(e) => setEndDateEnabled(e.target.checked)}
          />
          <span className="toggle-slider enabled-slider"></span>
        </label>
      </div>
      {endDateEnabled && (
        <div className="form-group">
          <label htmlFor="endDate">End Date:</label>
          <input
            id="endDate"
            type="date"
            value={endDate}
            onChange={(e) => setEndDate(e.target.value)}
            className="input-field"
          />
        </div>
      )}
      <div className="form-group">
        <label>End Time Enabled:</label>
        <label className="toggle-switch">
          <input
            type="checkbox"
            checked={endTimeEnabled}
            onChange={(e) => setEndTimeEnabled(e.target.checked)}
          />
          <span className="toggle-slider enabled-slider"></span>
        </label>
      </div>
      {endTimeEnabled && (
        <div className="form-group">
          <label htmlFor="endTime">End Time:</label>
          <input
            id="endTime"
            type="time"
            value={endTime}
            onChange={(e) => setEndTime(e.target.value)}
            className="input-field"
          />
        </div>
      )}
      <div className="form-group days-group">
        <label>Days per Week:</label>
        <div className="checkbox-group">
          {daysOfWeek.map((day) => (
            <label key={day} className="checkbox-label">
              <input
                type="checkbox"
                checked={selectedDays.includes(day)}
                onChange={() => toggleDay(day)}
              />
              {day}
            </label>
          ))}
        </div>
      </div>
      <div className="form-group">
        <label htmlFor="trigger">Trigger:</label>
        <select
          id="trigger"
          value={trigger}
          onChange={(e) => setTrigger(e.target.value)}
          className="input-field"
        >
          <option value="Manual">Manual</option>
          <option value="Cycle Timer">Cycle Timer</option>
        </select>
      </div>
      <div className="trigger-content">
        {trigger === 'Cycle Timer' ? (
          <div className="time-fields">
            <div className="form-group">
              <label>Run Time:</label>
              <div className="time-input-group">
                <div className="time-input">
                  <input
                    type="number"
                    min="0"
                    value={runTime.hours}
                    onChange={(e) => updateTimeField('runTime', 'hours', e.target.value)}
                    placeholder="0"
                    className="input-field time-subfield"
                  />
                  <span>Hours</span>
                </div>
                <div className="time-input">
                  <input
                    type="number"
                    min="0"
                    value={runTime.minutes}
                    onChange={(e) => updateTimeField('runTime', 'minutes', e.target.value)}
                    placeholder="0"
                    className="input-field time-subfield"
                  />
                  <span>Minutes</span>
                </div>
                <div className="time-input">
                  <input
                    type="number"
                    min="0"
                    value={runTime.seconds}
                    onChange={(e) => updateTimeField('runTime', 'seconds', e.target.value)}
                    placeholder="0"
                    className="input-field time-subfield"
                  />
                  <span>Seconds</span>
                </div>
              </div>
            </div>
            <div className="form-group">
              <label>Stop Time:</label>
              <div className="time-input-group">
                <div className="time-input">
                  <input
                    type="number"
                    min="0"
                    value={stopTime.hours}
                    onChange={(e) => updateTimeField('stopTime', 'hours', e.target.value)}
                    placeholder="0"
                    className="input-field time-subfield"
                  />
                  <span>Hours</span>
                </div>
                <div className="time-input">
                  <input
                    type="number"
                    min="0"
                    value={stopTime.minutes}
                    onChange={(e) => updateTimeField('stopTime', 'minutes', e.target.value)}
                    placeholder="0"
                    className="input-field time-subfield"
                  />
                  <span>Minutes</span>
                </div>
                <div className="time-input">
                  <input
                    type="number"
                    min="0"
                    value={stopTime.seconds}
                    onChange={(e) => updateTimeField('stopTime', 'seconds', e.target.value)}
                    placeholder="0"
                    className="input-field time-subfield"
                  />
                  <span>Seconds</span>
                </div>
              </div>
            </div>
          </div>
        ) : (
          <div className="manual-message">Manually powered on</div>
        )}
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