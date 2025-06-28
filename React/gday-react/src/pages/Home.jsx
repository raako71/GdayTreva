import { useState, useEffect, useRef, useMemo } from 'react';
import { Link } from 'react-router-dom';
import PropTypes from 'prop-types';
import '../styles.css';

// Utility function to format countdown seconds into a string
const formatCountdown = (secondsLeft) => {
  if (secondsLeft <= 0) return 'Toggling...';
  const hours = Math.floor(secondsLeft / 3600);
  const minutes = Math.floor((secondsLeft % 3600) / 60);
  const seconds = secondsLeft % 60;
  return `${hours.toString().padStart(2, '0')}h ${minutes
    .toString()
    .padStart(2, '0')}m ${seconds.toString().padStart(2, '0')}s`;
};

function Home({ activeProgramData, programCache, message }) {
  const [countdowns, setCountdowns] = useState({});
  const triggerStatusRef = useRef(activeProgramData);
  const messageRef = useRef(message);

  // Update refs with latest props
  useEffect(() => {
    triggerStatusRef.current = activeProgramData;
    messageRef.current = message;
  }, [activeProgramData, message]);

  // Update countdowns every second for cycle timer programs
  useEffect(() => {
    const interval = setInterval(() => {
      const currentTriggerStatus = triggerStatusRef.current;
      const currentMessage = messageRef.current;

      // Safely access progs, defaulting to empty array if undefined
      const progs = currentTriggerStatus?.progs || [];

      // Skip if no programs or epoch is invalid
      if (programCache.length === 0 || currentMessage?.epoch == null) {
        setCountdowns({});
        return;
      }

      const newCountdowns = {};

      programCache.forEach((prog) => {
        if (
          prog.enabled &&
          prog.trigger === 'Cycle Timer' &&
          prog.cycleConfig?.valid &&
          currentTriggerStatus?.progs
        ) {
          // Find corresponding active program to get next_toggle
          const activeProg = progs.find((p) => p.id === prog.id);
          if (activeProg && typeof activeProg.next_toggle === 'number' && !isNaN(activeProg.next_toggle)) {
            const secondsLeft = activeProg.next_toggle - currentMessage.epoch;
            newCountdowns[String(prog.id)] = formatCountdown(secondsLeft);
          }
        }
      });

      setCountdowns((prev) => {
        const hasChanged = Object.keys(newCountdowns).some(
          (id) => newCountdowns[id] !== prev[id]
        ) || Object.keys(prev).some((id) => !newCountdowns[id]);
        return hasChanged ? newCountdowns : prev;
      });
    }, 1000);

    return () => clearInterval(interval);
  }, [programCache]);

  // Memoize program tiles to prevent unnecessary re-renders
  const programTiles = useMemo(() => {
    if (!programCache || programCache.length === 0) {
      return <p>No programs available</p>;
    }

    return programCache
      .filter((prog) => prog.enabled) // Only show enabled programs
      .map((prog) => {
        const programId = prog.id.toString().padStart(2, '0');
        const programName = prog.name || `Program${programId}`;
        
        // Determine trigger display
        const triggerDisplay = prog.trigger === 'Sensor' && prog.sensorType
          ? `${prog.sensorType} ${prog.sensorAddress || ''} ${prog.sensorCapability || ''}`.trim()
          : prog.trigger || 'Unknown';

        // Determine state from activeProgramData
        const activeProg = activeProgramData?.progs?.find((p) => p.id === prog.id);
        const state = activeProg ? (activeProg.state ? 'ON' : 'OFF') : 'Unknown';

        // Find sensor value for sensor-triggered programs
        let sensorValue = 'N/A';
        if (prog.trigger === 'Sensor' && prog.sensorType && prog.sensorAddress && prog.sensorCapability) {
          const sensor = activeProgramData?.sensors?.find(
            (s) =>
              s.type === prog.sensorType &&
              s.address === prog.sensorAddress &&
              s.capability === prog.sensorCapability
          );
          sensorValue = sensor ? sensor.value : 'N/A';
        }

        return (
          <div key={prog.id} className="Tile">
            <h2>
              <Link to={`/programEditor?programID=${programId}`}>
                {programName}
              </Link>
            </h2>
            <p>Output: {prog.output || 'None'}</p>
            <p>State: {state}</p>
            <p>Trigger: {triggerDisplay}</p>
            {prog.trigger === 'Sensor' && (
              <p>Sensor Value: {sensorValue}</p>
            )}
            {prog.trigger === 'Cycle Timer' && prog.cycleConfig?.valid && (
              <p>
                Next Toggle:{' '}
                {countdowns[String(prog.id)] || 'Awaiting Schedule'}
              </p>
            )}
          </div>
        );
      });
  }, [programCache, activeProgramData, countdowns]);

  return (
    <div>
      <div className="Title">
        <h1>Home Page</h1>
      </div>
      <div className="Tile-container">
        {programTiles}
      </div>
    </div>
  );
}

Home.propTypes = {
  activeProgramData: PropTypes.shape({
    type: PropTypes.string,
    epoch: PropTypes.number,
    progs: PropTypes.arrayOf(
      PropTypes.shape({
        id: PropTypes.number.isRequired,
        output: PropTypes.string,
        trigger: PropTypes.string,
        next_toggle: PropTypes.number,
        state: PropTypes.bool,
      })
    ),
    sensors: PropTypes.arrayOf(
      PropTypes.shape({
        type: PropTypes.string,
        address: PropTypes.string,
        capability: PropTypes.string,
        value: PropTypes.string,
      })
    ),
  }),
  programCache: PropTypes.arrayOf(
    PropTypes.shape({
      id: PropTypes.number.isRequired,
      enabled: PropTypes.bool,
      output: PropTypes.string,
      trigger: PropTypes.string,
      sensorType: PropTypes.string,
      sensorAddress: PropTypes.string,
      sensorCapability: PropTypes.string,
      name: PropTypes.string,
      cycleConfig: PropTypes.shape({
        runSeconds: PropTypes.number,
        stopSeconds: PropTypes.number,
        startHigh: PropTypes.bool,
        valid: PropTypes.bool,
      }),
    })
  ),
  message: PropTypes.shape({
    epoch: PropTypes.number,
    offset_minutes: PropTypes.number,
    mem_used: PropTypes.number,
    mem_total: PropTypes.number,
    type: PropTypes.string,
  }),
};

Home.defaultProps = {
  activeProgramData: { progs: [], sensors: [] },
  programCache: [],
  message: { epoch: 0 },
};

export default Home;