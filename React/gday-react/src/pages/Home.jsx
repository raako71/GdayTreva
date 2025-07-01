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

function Home({ activeProgramData, programCache, message, cycleTimerStatus }) {
  const [countdowns, setCountdowns] = useState({});
  const messageRef = useRef(message);
  const cycleTimerStatusRef = useRef(cycleTimerStatus);

  // Update refs with latest props
  useEffect(() => {
    messageRef.current = message;
    cycleTimerStatusRef.current = cycleTimerStatus;
  }, [activeProgramData, message, cycleTimerStatus]);

  // Update countdowns every second for cycle timer programs
  useEffect(() => {
    const interval = setInterval(() => {
      const currentMessage = messageRef.current;
      const currentCycleTimerStatus = cycleTimerStatusRef.current;

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
          prog.output !== 'null'
        ) {
          // Determine active program IDs from activeProgramData
          if (prog) {
            let nextToggle;
            if (prog.output === 'A' && currentCycleTimerStatus.NextCycleToggleA !== undefined && currentCycleTimerStatus.NextCycleToggleA !== null) {
              nextToggle = currentCycleTimerStatus.NextCycleToggleA;
            } else if (prog.output === 'B' && currentCycleTimerStatus.NextCycleToggleB !== undefined && currentCycleTimerStatus.NextCycleToggleB !== null) {
              nextToggle = currentCycleTimerStatus.NextCycleToggleB;
            }
            if (nextToggle && typeof nextToggle === 'number' && !isNaN(nextToggle)) {
              const secondsLeft = nextToggle - currentMessage.epoch;
              newCountdowns[String(prog.id)] = formatCountdown(secondsLeft);
            } else {
              newCountdowns[String(prog.id)] = 'Awaiting Schedule';
            }
          } else {
            newCountdowns[String(prog.id)] = 'Inactive';
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
  }, [programCache, cycleTimerStatus]);

  // Memoize program tiles to prevent unnecessary re-renders
  const programTiles = useMemo(() => {
    if (!programCache || programCache.length === 0) {
      return <p>No programs available</p>;
    }

    return programCache
      .filter((prog) => prog.enabled)
      .map((prog) => {
        const programId = prog.id.toString().padStart(2, '0');
        const programName = prog.name || `Program${programId}`;

        // Determine trigger display
        const triggerDisplay = prog.trigger === 'Sensor' && prog.sensorCapability
          ? `Capability: ${prog.sensorCapability}`
          : prog.trigger || 'Unknown';

        // Determine state from cycleTimerStatus
        let state = null;
        if (prog.output === 'A' && cycleTimerStatus.outputAState !== undefined) {
          state = cycleTimerStatus.outputAState ? 'ON' : 'OFF';
        } else if (prog.output === 'B' && cycleTimerStatus.outputBState !== undefined) {
          state = cycleTimerStatus.outputBState ? 'ON' : 'OFF';
        }

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
            {prog.output && <p>Output: {prog.output}</p>}
            {state && <p>State: {state}</p>}
            <p>{triggerDisplay}</p>
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
  }, [programCache, activeProgramData, cycleTimerStatus, countdowns]);

  return (
    <div>
      <div className="Title">
        <h1>{message.device_name || 'Home Page'}</h1>
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
    device_name: PropTypes.string,
  }),
  cycleTimerStatus: PropTypes.shape({
    NextCycleToggleA: PropTypes.number,
    NextCycleToggleB: PropTypes.number,
    outputAState: PropTypes.bool,
    outputBState: PropTypes.bool,
  }),
};

Home.defaultProps = {
  activeProgramData: { progs: [], sensors: [] },
  programCache: [],
  message: { epoch: 0, device_name: '' },
  cycleTimerStatus: { NextCycleToggleA: 0, NextCycleToggleB: 0, outputAState: false, outputBState: false },
};

export default Home;