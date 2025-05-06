import { useState, useEffect } from 'react';
import { Link } from 'react-router-dom';
import PropTypes from 'prop-types';
import '../styles.css';

function Home({ triggerStatus, programs }) {
  const [countdowns, setCountdowns] = useState({});

  // Update countdowns every second for Cycle triggers
  useEffect(() => {
    const interval = setInterval(() => {
      if (!triggerStatus?.progs) return;

      const newCountdowns = {};
      triggerStatus.progs.forEach((prog) => {
        if (prog.trigger === 'Cycle' && prog.next_toggle && triggerStatus.epoch) {
          const secondsLeft = prog.next_toggle - triggerStatus.epoch;
          if (secondsLeft > 0) {
            const minutes = Math.floor(secondsLeft / 60);
            const seconds = secondsLeft % 60;
            newCountdowns[prog.id] = `${minutes}m ${seconds}s`;
          } else {
            newCountdowns[prog.id] = 'Toggling...';
          }
        }
      });
      setCountdowns(newCountdowns);
    }, 1000);

    return () => clearInterval(interval);
  }, [triggerStatus]);

  return (
    <div>
      <div className="Title">
        <h1>Home Page</h1>
      </div>
      <div className="Tile-container">
        {!triggerStatus?.progs || triggerStatus.progs.length === 0 ? (
          <p>No active programs</p>
        ) : (
          triggerStatus.progs.map((prog) => {
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
                <p>Trigger: {prog.trigger}</p>
                {prog.trigger === 'Cycle' && countdowns[prog.id] && (
                  <p>Next Toggle: {countdowns[prog.id]}</p>
                )}
              </div>
            );
          })
        )}
      </div>
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
        state: PropTypes.bool, // Optional, as it's not in the example yet
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