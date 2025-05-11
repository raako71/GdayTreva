import { useState, useEffect, useRef } from 'react';
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

function Home({ triggerStatus, programs, message }) {
  const [countdowns, setCountdowns] = useState({});
  const triggerStatusRef = useRef(triggerStatus);
  const messageRef = useRef(message);

  // Update refs with latest props
  useEffect(() => {
    triggerStatusRef.current = triggerStatus;
    messageRef.current = message;
  }, [triggerStatus, message]);

  // Update countdowns every second
  useEffect(() => {
    const interval = setInterval(() => {
      const currentTriggerStatus = triggerStatusRef.current;
      const currentMessage = messageRef.current;

      const progs = currentTriggerStatus?.progs || (currentTriggerStatus?.type === 'trigger_status' ? currentTriggerStatus.progs : null);

      if (!progs || progs.length === 0 || currentMessage?.epoch == null) {
        return;
      }

      const newCountdowns = {};

      progs.forEach((prog) => {
        if (prog.trigger === 'Cycle Timer' && typeof prog.next_toggle === 'number' && !isNaN(prog.next_toggle)) {
          const secondsLeft = prog.next_toggle - currentMessage.epoch;
          newCountdowns[String(prog.id)] = formatCountdown(secondsLeft);
        }
      });

      setCountdowns((prev) => {
        const hasChanged = Object.keys(newCountdowns).some(
          (id) => newCountdowns[id] !== prev[id]
        );
        return hasChanged ? newCountdowns : prev;
      });
    }, 1000);

    return () => clearInterval(interval);
  }, []);

  // Basic UI
  return (
    <div>
      <div className="Title">
        <h1>Home Page</h1>
      </div>
      <div className="Tile-container">
        {triggerStatus?.progs && triggerStatus.progs.length > 0 ? (
          triggerStatus.progs.map((prog) => {
            const programId = prog.id.toString().padStart(2, '0');
            const program = programs.find((p) => p.id === programId) || { name: `Program${programId}` };
            return (
              <div key={prog.id} className="Tile">
                <h2>
                  <Link to={`/programEditor?programID=${programId}`}>
                    {program.name}
                  </Link>
                </h2>
                <p>Output: {prog.output}</p>
                <p>State: {prog.state === undefined ? 'Unknown' : prog.state ? 'ON' : 'OFF'}</p>
                <p>Trigger: {prog.trigger}</p>
                {prog.trigger === 'Cycle Timer' && (
                  <p>
                    Next Toggle:{' '}
                    {countdowns[String(prog.id)] || 'Awaiting Schedule'}
                  </p>
                )}
              </div>
            );
          })
        ) : (
          <p>No active programs</p>
        )}
      </div>
    </div>
  );
}

Home.propTypes = {
  triggerStatus: PropTypes.shape({
    type: PropTypes.string,
    epoch: PropTypes.number,
    progs: PropTypes.arrayOf(
      PropTypes.shape({
        id: PropTypes.number.isRequired,
        output: PropTypes.string,
        trigger: PropTypes.string.isRequired,
        next_toggle: PropTypes.number,
        state: PropTypes.bool,
      })
    ),
  }),
  programs: PropTypes.arrayOf(
    PropTypes.shape({
      id: PropTypes.string.isRequired,
      name: PropTypes.string,
    })
  ),
  message: PropTypes.shape({
    epoch: PropTypes.number.isRequired,
    offset_minutes: PropTypes.number,
    mem_used: PropTypes.number,
    mem_total: PropTypes.number,
    type: PropTypes.string,
  }),
};

Home.defaultProps = {
  triggerStatus: { progs: [] },
  programs: [],
  message: { epoch: 0 },
};

export default Home;