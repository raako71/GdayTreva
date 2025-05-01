import { Link } from 'react-router-dom';
import PropTypes from 'prop-types';
import '../styles.css';

function Programs({ programs }) {
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
              {programs.length === 0 ? (
                <tr>
                  <td>No programs found</td>
                </tr>
              ) : (
                programs.map((program) => (
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
  programs: PropTypes.arrayOf(
    PropTypes.shape({
      id: PropTypes.string.isRequired,
      name: PropTypes.string,
    })
  ).isRequired,
};

export default Programs;