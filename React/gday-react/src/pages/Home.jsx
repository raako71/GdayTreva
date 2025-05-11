import { useState, useEffect, useMemo } from 'react';
import { Link } from 'react-router-dom';
import PropTypes from 'prop-types';
import '../styles.css';

function Home({ triggerStatus, programs, message}) {
  const [countdowns, setCountdowns] = useState({});

  // Update countdowns every second for Cycle Timer triggers
  useEffect(() => {
    if (!triggerStatus?.progs) return;

    const interval = setInterval(() => {
      const newCountdowns = {};
      triggerStatus.progs.forEach((prog) => {
        if (prog.trigger === 'Cycle Timer' && prog.next_toggle) {
          const secondsLeft = prog.next_toggle - message?.epoch;
          if (secondsLeft > 0) {
            const hours = Math.floor(secondsLeft / 3600);
            const minutes = Math.floor((secondsLeft % 3600) / 60);
            const seconds = secondsLeft % 60;
            newCountdowns[prog.id] = `${hours.toString().padStart(2, '0')}h ${minutes
              .toString()
              .padStart(2, '0')}m ${seconds.toString().padStart(2, '0')}s`;
          } else {
            newCountdowns[prog.id] = 'Toggling...';
          }
        }
      });
      setCountdowns((prev) => {
        // Only update if countdowns have changed to minimize re-renders
        const hasChanged = Object.keys(newCountdowns).some(
          (id) => newCountdowns[id] !== prev[id]
        );
        return hasChanged ? newCountdowns : prev;
      });
    }, 1000);

    return () => clearInterval(interval);
  }, [triggerStatus, message]);

  // Memoize the program tiles to prevent unnecessary re-renders
  const programTiles = useMemo(() => {
    if (!triggerStatus?.progs || triggerStatus.progs.length === 0) {
      return <p>No active programs</p>;
    }

    return triggerStatus.progs.map((prog) => {
      const programId = prog.id.toString().padStart(2, '0');
      const program = programs.find((p) => p.id === programId);
      return (
        <div key={prog.id} className="Tile">
          <h2>
            <Link to={`/programEditor?programID=${programId}`}>
              {program?.name || `Program${programId}`}
            </Link>
          </h2>
          <p>Output: {prog.output}</p>
          <p>State: {prog.state === undefined ? 'Unknown' : prog.state ? 'ON' : 'OFF'}</p>
          <p>Trigger: {prog.trigger}</p>
          {prog.trigger === 'Cycle Timer' && countdowns[prog.id] && (
            <p>Next Toggle: {countdowns[prog.id]}</p>
          )}
        </div>
      );
    });
  }, [triggerStatus, programs, countdowns]);

  return (
    <div>
      <div className="Title">
        <h1>Home Page</h1>
      </div>
      <div className="Tile-container">{programTiles}</div>
    </div>
  );
}

Home.propTypes = {
  triggerStatus: PropTypes.shape({
    epoch: PropTypes.number,
    progs: PropTypes.arrayOf(
      PropTypes.shape({
        id: PropTypes.number.isRequired,
        output: PropTypes.string.isRequired,
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
  ).isRequired,
};

export default Home;