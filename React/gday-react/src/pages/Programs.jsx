import { Link } from 'react-router-dom';
import PropTypes from 'prop-types';
import '../styles.css';

function Programs({ programCache }) {
  return (
    <div>
      <div className="Title">
        <h1>Programs</h1>
      </div>
      <div className="Tile-container">
        <div className="Tile">
          <table>
            <thead>
              <tr>
                <th>Program Name</th>
              </tr>
            </thead>
            <tbody>
              {programCache.length === 0 ? (
                <tr>
                  <td>No programs found</td>
                </tr>
              ) : (
                programCache.map((program) => (
                  <tr key={program.id}>
                    <td>
                      <Link to={`/programEditor?programID=${program.id}`}>
                        {program.name || `Program${program.id}`}
                      </Link>
                    </td>
                  </tr>
                ))
              )}
            </tbody>
          </table>
          <hr />
          <div className="new-program-link">
            <Link to="/programEditor">New Program</Link>
          </div>
        </div>
      </div>
    </div>
  );
}

Programs.propTypes = {
  programCache: PropTypes.arrayOf(
    PropTypes.shape({
      id: PropTypes.number.isRequired,
      name: PropTypes.string
    })
  ).isRequired,
};

export default Programs;