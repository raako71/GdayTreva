import '../styles.css';

function TimeBar({ epoch }) {
  return <div>Epoch: {epoch || 'Not connected'}</div>;
}

export default TimeBar;