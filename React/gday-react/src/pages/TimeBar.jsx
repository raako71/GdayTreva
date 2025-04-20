import '../styles.css';
import { Link } from 'react-router-dom';
import { useState } from 'react';

function TimeBar({ message }) {
  const [isMenuOpen, setIsMenuOpen] = useState(false);

  const formatTime = (epoch) => {
    if (!epoch) return 'N/A';
    const date = new Date(epoch * 1000);
    return date.toLocaleString();
  };

  const toggleMenu = () => {
    setIsMenuOpen(!isMenuOpen);
  };

  const memPercent = message?.mem_total
    ? Math.round(((message.mem_total - message.mem_used) / message.mem_total) * 100)
    : 0;

  return (
    <div className="time-bar">
      <div>Time: {formatTime(message?.epoch)}</div>
      <div>-</div>
      <div>Memory Free: {memPercent}%</div>
      <button
        className="hamburger"
        onClick={toggleMenu}
        aria-label="Toggle navigation menu"
      >
        <svg
          className="hamburger-icon"
          fill="none"
          stroke="currentColor"
          viewBox="0 0 24 24"
          xmlns="http://www.w3.org/2000/svg"
        >
          <path
            strokeLinecap="round"
            strokeLinejoin="round"
            strokeWidth="2"
            d="M4 6h16M4 12h16m-7 6h7"
          />
        </svg>
      </button>
      {isMenuOpen && (
        <div className="menu">
          <Link
            to="/"
            className="menu-item"
            onClick={() => setIsMenuOpen(false)}
          >
            Home
          </Link>
          <Link
            to="/settings"
            className="menu-item"
            onClick={() => setIsMenuOpen(false)}
          >
            Settings
          </Link>
          <Link
            to="/graph"
            className="menu-item"
            onClick={() => setIsMenuOpen(false)}
          >
            Graph
          </Link>
          <Link
            to="/programs"
            className="menu-item"
            onClick={() => setIsMenuOpen(false)}
          >
            Programs
          </Link>
        </div>
      )}
    </div>
  );
}

export default TimeBar;