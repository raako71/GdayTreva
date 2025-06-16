import { useState, useEffect, useRef } from 'react';
import { useLocation, useNavigate } from 'react-router-dom';
import '../styles.css';
import PropTypes from 'prop-types';

function ProgramEditor({ wsRef, isWsReady, programs, sensors }) {
  const [programID, setProgramID] = useState('00');
  const [name, setName] = useState('');
  const [enabled, setEnabled] = useState(false);
  const [isMonitorOnly, setIsMonitorOnly] = useState(false);
  const [output, setOutput] = useState('A');
  const [startDate, setStartDate] = useState('');
  const [startDateEnabled, setStartDateEnabled] = useState(true);
  const [endDate, setEndDate] = useState('');
  const [endDateEnabled, setEndDateEnabled] = useState(true);
  const [startTime, setStartTime] = useState('00:00');
  const [startTimeEnabled, setStartTimeEnabled] = useState(true);
  const [endTime, setEndTime] = useState('23:59');
  const [endTimeEnabled, setEndTimeEnabled] = useState(true);
  const [selectedDays, setSelectedDays] = useState([]);
  const [daysPerWeekEnabled, setDaysPerWeekEnabled] = useState(true);
  const [triggerType, setTriggerType] = useState('Manual');
  const [triggerAddress, setTriggerAddress] = useState('');
  const [triggerCapability, setTriggerCapability] = useState('');
  const [runTime, setRunTime] = useState({ seconds: '', minutes: '', hours: '' });
  const [stopTime, setStopTime] = useState({ seconds: '', minutes: '', hours: '' });
  const [startHigh, setStartHigh] = useState(true);
  const [error, setError] = useState(null);
  const [status, setStatus] = useState('');
  const location = useLocation();
  const navigate = useNavigate();
  const fileInputRef = useRef(null);

  const daysOfWeek = [
    'Monday',
    'Tuesday',
    'Wednesday',
    'Thursday',
    'Friday',
    'Saturday',
    'Sunday',
  ];

  // Define trigger options
  const standardTriggers = [
    { value: 'Manual', label: 'Manual' },
    { value: 'Cycle Timer', label: 'Cycle Timer' },
  ];
  const sensorTriggers = sensors.flatMap((sensor) =>
    (sensor.capabilities || []).map((capability) => ({
      value: `${sensor.type}_${sensor.address}_${capability}`,
      label: `${sensor.type} ${sensor.address} - ${capability}`,
    }))
  );
  const triggerOptions = isMonitorOnly
    ? sensorTriggers
    : [...standardTriggers, ...sensorTriggers];

  useEffect(() => {
    const params = new URLSearchParams(location.search);
    const id = params.get('programID');
    if (id) {
      const parsedID = parseInt(id, 10);
      if (!isNaN(parsedID) && parsedID >= 1 && parsedID <= 10) {
        const formattedID = parsedID.toString().padStart(2, '0');
        setProgramID(formattedID);

        const program = programs.find((p) => p.id === formattedID);
        if (program) {
          setName(program.name || '');
          setEnabled(program.enabled || false);
          setIsMonitorOnly(program.output === 'null');
          setOutput(program.output === 'null' ? 'null' : program.output || 'A');
          setStartDate(program.startDate || '');
          setStartDateEnabled(program.startDateEnabled !== false);
          setEndDate(program.endDate || '');
          setEndDateEnabled(program.endDateEnabled !== false);
          setStartTime(program.startTime || '00:00');
          setStartTimeEnabled(program.startTimeEnabled !== false);
          setEndTime(program.endTime || '23:59');
          setEndTimeEnabled(program.endTimeEnabled !== false);
          setSelectedDays(program.selectedDays || []);
          setDaysPerWeekEnabled(program.daysPerWeekEnabled !== false);
          if (program.triggerType) {
            setTriggerType(program.triggerType);
            setTriggerAddress(program.triggerAddress || '');
            setTriggerCapability(program.triggerCapability || '');
          } else if (program.trigger) {
            if (program.trigger === 'Manual' || program.trigger === 'Cycle Timer') {
              setTriggerType(program.trigger);
              setTriggerAddress('');
              setTriggerCapability('');
            } else {
              const [type, address, capability] = program.trigger.split('_');
              setTriggerType(type || 'Manual');
              setTriggerAddress(address || '');
              setTriggerCapability(capability || '');
            }
          } else {
            setTriggerType('Manual');
            setTriggerAddress('');
            setTriggerCapability('');
          }
          setRunTime({
            seconds: program.runTime?.seconds?.toString() || '',
            minutes: program.runTime?.minutes?.toString() || '',
            hours: program.runTime?.hours?.toString() || '',
          });
          setStopTime({
            seconds: program.stopTime?.seconds?.toString() || '',
            minutes: program.stopTime?.minutes?.toString() || '',
            hours: program.stopTime?.hours?.toString() || '',
          });
          setStartHigh(program.startHigh !== false);
          setStatus(`Loaded program ${formattedID}`);
          setError(null);
        } else {
          setError(`Program with ID ${formattedID} not found`);
        }
      } else {
        setError('Invalid programID in URL (must be 1 to 10)');
      }
    }
  }, [location, programs]);

  useEffect(() => {
    if (!wsRef.current) return;

    const socket = wsRef.current;
    const handleMessage = (event) => {
      try {
        const data = JSON.parse(event.data);
        if (data.type === 'save_program_response') {
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
        console.error(err);
      }
    };

    socket.addEventListener('message', handleMessage);
    return () => {
      socket.removeEventListener('message', handleMessage);
    };
  }, [wsRef, isWsReady, programID, navigate]);

  const toggleDay = (day) => {
    setSelectedDays((prev) =>
      prev.includes(day)
        ? prev.filter((d) => d !== day)
        : [...prev, day]
    );
  };

  const updateTimeField = (field, subfield, value) => {
    const setField = field === 'runTime' ? setRunTime : setStopTime;
    setField((prev) => ({
      ...prev,
      [subfield]: value,
    }));
  };

  const validateProgram = () => {
    if (isMonitorOnly) return null;
    if (startDateEnabled && endDateEnabled && startDate && endDate) {
      const start = new Date(startDate);
      const end = new Date(endDate);
      if (start >= end) {
        return 'Start date and time must be before end date and time';
      }
    }
    if (startTimeEnabled && endTimeEnabled && startTime && endTime) {
      const [startHours, startMinutes] = startTime.split(':').map(Number);
      const [endHours, endMinutes] = endTime.split(':').map(Number);
      const startTotalMinutes = startHours * 60 + startMinutes;
      const endTotalMinutes = endHours * 60 + endMinutes;
      if (startTotalMinutes >= endTotalMinutes) {
        return 'Daily end time must be after daily start time';
      }
    }
    if (daysPerWeekEnabled && selectedDays.length === 0) {
      return 'At least one day must be selected if days per week is enabled';
    }
    if (triggerType === 'Cycle Timer' && !isMonitorOnly) {
      const runSeconds = (parseInt(runTime.hours) || 0) * 3600 + (parseInt(runTime.minutes) || 0) * 60 + (parseInt(runTime.seconds) || 0);
      const stopSeconds = (parseInt(stopTime.hours) || 0) * 3600 + (parseInt(stopTime.minutes) || 0) * 60 + (parseInt(stopTime.seconds) || 0);
      if (runSeconds === 0 || stopSeconds === 0) {
        return 'Run time and stop time must be greater than 0 for Cycle Timer';
      }
    }
    return null;
  };

  const saveProgram = async () => {
    setError(null);
    setStatus('Saving...');

    const validationError = validateProgram();
    if (validationError) {
      setError(validationError);
      setStatus('');
      return;
    }

    const programContent = {
      name,
      enabled,
      output: isMonitorOnly ? 'null' : output,
      startDate: isMonitorOnly ? '' : startDateEnabled ? startDate : '',
      startDateEnabled: isMonitorOnly ? false : startDateEnabled,
      endDate: isMonitorOnly ? '' : endDateEnabled ? endDate : '',
      endDateEnabled: isMonitorOnly ? false : endDateEnabled,
      startTime: isMonitorOnly ? '' : startTimeEnabled ? startTime : '',
      startTimeEnabled: isMonitorOnly ? false : startTimeEnabled,
      endTime: isMonitorOnly ? '' : endTimeEnabled ? endTime : '',
      endTimeEnabled: isMonitorOnly ? false : endTimeEnabled,
      selectedDays: isMonitorOnly ? [] : daysPerWeekEnabled ? selectedDays : [],
      daysPerWeekEnabled: isMonitorOnly ? false : daysPerWeekEnabled,
      trigger: triggerType === 'Manual' || triggerType === 'Cycle Timer' ? triggerType : undefined,
      triggerType: triggerType !== 'Manual' && triggerType !== 'Cycle Timer' ? triggerType : undefined,
      triggerAddress: triggerType !== 'Manual' && triggerType !== 'Cycle Timer' ? triggerAddress : undefined,
      triggerCapability: triggerType !== 'Manual' && triggerType !== 'Cycle Timer' ? triggerCapability : undefined,
      ...(triggerType === 'Cycle Timer' && !isMonitorOnly && {
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
        startHigh,
      }),
    };

    let sanitizedContent;
    try {
      sanitizedContent = JSON.stringify(programContent);
    } catch (err) {
      setError('Error serializing program data');
      setStatus('');
      console.error(err);
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
        programID,
        content: sanitizedContent,
      };
      wsRef.current.send(JSON.stringify(message));
      setStatus(`Program ${programID} save requested`);
    } else {
      setError('WebSocket not connected');
      setStatus('');
    }
  };

  const cancelEdit = () => {
    setError(null);
    setStatus('');
    navigate('/programs');
  };

  const handleImport = () => {
    fileInputRef.current.click();
  };

  const handleFileSelect = (event) => {
    const file = event.target.files[0];
    if (!file) return;

    if (!file.name.endsWith('.json')) {
      setError('Please select a JSON file');
      return;
    }

    const reader = new FileReader();
    reader.onload = (e) => {
      try {
        const content = JSON.parse(e.target.result);
        setName(content.name || '');
        setEnabled(content.enabled || false);
        setIsMonitorOnly(content.output === 'null');
        setOutput(content.output === 'null' ? 'null' : content.output || 'A');
        setStartDate(content.startDate || '');
        setStartDateEnabled(content.startDateEnabled !== false);
        setEndDate(content.endDate || '');
        setEndDateEnabled(content.endDateEnabled !== false);
        setStartTime(content.startTime || '00:00');
        setStartTimeEnabled(content.startTimeEnabled !== false);
        setEndTime(content.endTime || '23:59');
        setEndTimeEnabled(content.endTimeEnabled !== false);
        setSelectedDays(content.selectedDays || []);
        setDaysPerWeekEnabled(content.daysPerWeekEnabled !== false);
        if (content.trigger) {
          setTriggerType(content.trigger);
          setTriggerAddress('');
          setTriggerCapability('');
        } else if (content.triggerType) {
          setTriggerType(content.triggerType);
          setTriggerAddress(content.triggerAddress || '');
          setTriggerCapability(content.triggerCapability || '');
        } else {
          setTriggerType('Manual');
          setTriggerAddress('');
          setTriggerCapability('');
        }
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
        setStartHigh(content.startHigh !== false);
        setStatus('Program imported successfully');
        setError(null);
      } catch (err) {
        setError('Failed to parse JSON file');
        setStatus('');
        console.error(err);
      }
    };
    reader.onerror = () => {
      setError('Error reading file');
      setStatus('');
    };
    reader.readAsText(file);
    event.target.value = '';
  };

  const handleExport = () => {
    const programContent = {
      name,
      enabled,
      output: isMonitorOnly ? 'null' : output,
      startDate: isMonitorOnly ? '' : startDateEnabled ? startDate : '',
      startDateEnabled: isMonitorOnly ? false : startDateEnabled,
      endDate: isMonitorOnly ? '' : endDateEnabled ? endDate : '',
      endDateEnabled: isMonitorOnly ? false : endDateEnabled,
      startTime: isMonitorOnly ? '' : startTimeEnabled ? startTime : '',
      startTimeEnabled: isMonitorOnly ? false : startTimeEnabled,
      endTime: isMonitorOnly ? '' : endTimeEnabled ? endTime : '',
      endTimeEnabled: isMonitorOnly ? false : endTimeEnabled,
      selectedDays: isMonitorOnly ? [] : daysPerWeekEnabled ? selectedDays : [],
      daysPerWeekEnabled: isMonitorOnly ? false : daysPerWeekEnabled,
      trigger: triggerType === 'Manual' || triggerType === 'Cycle Timer' ? triggerType : undefined,
      triggerType: triggerType !== 'Manual' && triggerType !== 'Cycle Timer' ? triggerType : undefined,
      triggerAddress: triggerType !== 'Manual' && triggerType !== 'Cycle Timer' ? triggerAddress : undefined,
      triggerCapability: triggerType !== 'Manual' && triggerType !== 'Cycle Timer' ? triggerCapability : undefined,
      ...(triggerType === 'Cycle Timer' && !isMonitorOnly && {
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
        startHigh,
      }),
    };

    try {
      const json = JSON.stringify(programContent, null, 2);
      const blob = new Blob([json], { type: 'application/json' });
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = `program_${programID || 'new'}.json`;
      a.click();
      URL.revokeObjectURL(url);
      setStatus('Program exported successfully');
      setError(null);
    } catch (err) {
      setError('Failed to export program');
      setStatus('');
      console.error(err);
    }
  };

  const handleTriggerChange = (e) => {
    const value = e.target.value;
    if (value === 'Manual' || value === 'Cycle Timer') {
      setTriggerType(value);
      setTriggerAddress('');
      setTriggerCapability('');
      if (value !== 'Cycle Timer') {
        setRunTime({ seconds: '', minutes: '', hours: '' });
        setStopTime({ seconds: '', minutes: '', hours: '' });
        setStartHigh(true);
      }
    } else {
      const [type, address, capability] = value.split('_');
      setTriggerType(type);
      setTriggerAddress(address || '');
      setTriggerCapability(capability || '');
      setRunTime({ seconds: '', minutes: '', hours: '' });
      setStopTime({ seconds: '', minutes: '', hours: '' });
      setStartHigh(true);
    }
  };

  return (
    <div className="basic-div">
      <div className="Title">
        <h1>Program Editor</h1>
      </div>
      <div className="Title">
        <button className="save-button" onClick={handleImport}>
          Import
        </button>
        <button className="save-button" onClick={handleExport}>
          Export
        </button>
        <input
          type="file"
          ref={fileInputRef}
          style={{ display: 'none' }}
          accept=".json"
          onChange={handleFileSelect}
        />
      </div>
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
        <label>Monitor Only (No Output):</label>
        <label className="toggle-switch">
          <input
            type="checkbox"
            checked={isMonitorOnly}
            onChange={(e) => {
              setIsMonitorOnly(e.target.checked);
              if (e.target.checked) {
                setOutput('null');
                setTriggerType(sensorTriggers[0]?.value.split('_')[0] || 'Manual');
                setTriggerAddress(sensorTriggers[0]?.value.split('_')[1] || '');
                setTriggerCapability(sensorTriggers[0]?.value.split('_')[2] || '');
              } else {
                setOutput('A');
                setTriggerType('Manual');
                setTriggerAddress('');
                setTriggerCapability('');
              }
            }}
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
      {!isMonitorOnly && (
        <>
          <div className="form-group">
            <label>Start Date and Time Enabled:</label>
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
              <label htmlFor="startDate">Start Date and Time:</label>
              <input
                id="startDate"
                type="datetime-local"
                value={startDate}
                onChange={(e) => setStartDate(e.target.value)}
                className="input-field"
              />
              <small className="form-text">When the program begins.</small>
            </div>
          )}
          <div className="form-group">
            <label>Daily Start Time Enabled:</label>
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
              <label htmlFor="startTime">Daily Start Time:</label>
              <input
                id="startTime"
                type="time"
                value={startTime}
                onChange={(e) => setStartTime(e.target.value)}
                className="input-field"
              />
              <small className="form-text">Time when the program starts each day.</small>
            </div>
          )}
          <div className="form-group">
            <label>End Date and Time Enabled:</label>
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
              <label htmlFor="endDate">End Date and Time:</label>
              <input
                id="endDate"
                type="datetime-local"
                value={endDate}
                onChange={(e) => setEndDate(e.target.value)}
                className="input-field"
              />
              <small className="form-text">When the program ends.</small>
            </div>
          )}
          <div className="form-group">
            <label>Daily End Time Enabled:</label>
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
              <label htmlFor="endTime">Daily End Time:</label>
              <input
                id="endTime"
                type="time"
                value={endTime}
                onChange={(e) => setEndTime(e.target.value)}
                className="input-field"
              />
              <small className="form-text">Time when the program ends each day.</small>
            </div>
          )}
          <div className="form-group">
            <label>Days per Week Enabled:</label>
            <label className="toggle-switch">
              <input
                type="checkbox"
                checked={daysPerWeekEnabled}
                onChange={(e) => setDaysPerWeekEnabled(e.target.checked)}
              />
              <span className="toggle-slider enabled-slider"></span>
            </label>
          </div>
          {daysPerWeekEnabled && (
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
              <small className="form-text">
                Select days when the program should run within the date range.
              </small>
            </div>
          )}
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
        </>
      )}
      <div className="form-group">
        <label htmlFor="trigger">Trigger:</label>
        <select
          id="trigger"
          value={triggerType === 'Manual' || triggerType === 'Cycle Timer' ? triggerType : `${triggerType}_${triggerAddress}_${triggerCapability}`}
          onChange={handleTriggerChange}
          className="input-field"
        >
          {triggerOptions.map((option) => (
            <option key={programID + '_' + option.value} value={option.value}>
              {option.label}
            </option>
          ))}
        </select>
      </div>
      <div className="trigger-content">
        {triggerType === 'Cycle Timer' && !isMonitorOnly ? (
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
            <div className="form-group">
              <label>Start High:</label>
              <label className="toggle-switch">
                <input
                  type="checkbox"
                  checked={startHigh}
                  onChange={(e) => setStartHigh(e.target.checked)}
                />
                <span className="toggle-slider enabled-slider"></span>
              </label>
              <small className="form-text">
                Start with output on (High) if checked, off (Low) if unchecked.
              </small>
            </div>
          </div>
        ) : (
          <div className="manual-message">
            {triggerType === 'Manual' && !isMonitorOnly
              ? 'Manually powered on'
              : `Monitoring ${triggerType} ${triggerAddress} ${triggerCapability}`}
          </div>
        )}
      </div>
      <div className="button-group">
        <button className="save-button" onClick={saveProgram}>
          Save Program
        </button>
        <button className="cancel-button" onClick={cancelEdit}>
          Cancel
        </button>
      </div>
    </div>
  );
}

ProgramEditor.propTypes = {
  wsRef: PropTypes.shape({
    current: PropTypes.instanceOf(WebSocket),
  }).isRequired,
  isWsReady: PropTypes.bool.isRequired,
  programs: PropTypes.arrayOf(
    PropTypes.shape({
      id: PropTypes.string.isRequired,
      name: PropTypes.string,
      enabled: PropTypes.bool,
      output: PropTypes.string,
      startDate: PropTypes.string,
      startDateEnabled: PropTypes.bool,
      endDate: PropTypes.string,
      endDateEnabled: PropTypes.bool,
      startTime: PropTypes.string,
      startTimeEnabled: PropTypes.bool,
      endTime: PropTypes.string,
      endTimeEnabled: PropTypes.bool,
      selectedDays: PropTypes.arrayOf(PropTypes.string),
      daysPerWeekEnabled: PropTypes.bool,
      trigger: PropTypes.string,
      triggerType: PropTypes.string,
      triggerAddress: PropTypes.string,
      triggerCapability: PropTypes.string,
      runTime: PropTypes.shape({
        seconds: PropTypes.oneOfType([PropTypes.number, PropTypes.string]),
        minutes: PropTypes.oneOfType([PropTypes.number, PropTypes.string]),
        hours: PropTypes.oneOfType([PropTypes.number, PropTypes.string]),
      }),
      stopTime: PropTypes.shape({
        seconds: PropTypes.oneOfType([PropTypes.number, PropTypes.string]),
        minutes: PropTypes.oneOfType([PropTypes.number, PropTypes.string]),
        hours: PropTypes.oneOfType([PropTypes.number, PropTypes.string]),
      }),
      startHigh: PropTypes.bool,
    })
  ).isRequired,
  sensors: PropTypes.arrayOf(
    PropTypes.shape({
      type: PropTypes.string.isRequired,
      address: PropTypes.string.isRequired,
      active: PropTypes.bool.isRequired,
      capabilities: PropTypes.arrayOf(PropTypes.string),
    })
  ).isRequired,
};

export default ProgramEditor;