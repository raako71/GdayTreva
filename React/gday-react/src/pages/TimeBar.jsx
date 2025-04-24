import '../styles.css';
import { Link } from 'react-router-dom';
import { useState, useRef } from 'react';

function TimeBar({ message, wsRef }) {
  const [isMenuOpen, setIsMenuOpen] = useState(false);
  const [isOverlayOpen, setIsOverlayOpen] = useState(false);
  const [offsetError, setOffsetError] = useState(null);
  const timeoutRef = useRef(null);

  // Format time with ESP32's offset
  const formatTime = (epoch, offsetMinutes) => {
    if (!epoch || isNaN(epoch) || epoch === 0 || offsetMinutes == null || isNaN(offsetMinutes)) {
      return 'Waiting for time data...';
    }
    const adjustedEpochMs = (epoch + offsetMinutes * 60) * 1000;
    const date = new Date(adjustedEpochMs);
    if (isNaN(date.getTime())) {
      console.warn('Invalid date calculated:', { epoch, offsetMinutes, adjustedEpochMs });
      return 'Invalid time';
    }
    const day = String(date.getUTCDate()).padStart(2, '0');
    const month = String(date.getUTCMonth() + 1).padStart(2, '0');
    const year = date.getUTCFullYear();
    const hours = String(date.getUTCHours()).padStart(2, '0');
    const minutes = String(date.getUTCMinutes()).padStart(2, '0');
    const seconds = String(date.getUTCSeconds()).padStart(2, '0');

    const offsetHours = Math.floor(Math.abs(offsetMinutes) / 60);
    const offsetMins = Math.abs(offsetMinutes) % 60;
    const offsetSign = offsetMinutes >= 0 ? '+' : '-';
    const offsetStr = `${offsetSign}${String(offsetHours).padStart(2, '0')}:${String(offsetMins).padStart(2, '0')}`;

    return `${day}/${month}/${year}, ${hours}:${minutes}:${seconds} (${offsetStr})`;
  };

  // Format offset for overlay
  const formatOffset = (minutes) => {
    if (minutes == null || isNaN(minutes)) return 'N/A';
    const hours = Math.floor(Math.abs(minutes) / 60);
    const mins = Math.abs(minutes) % 60;
    const sign = minutes >= 0 ? '+' : '-';
    return `${sign}${String(hours).padStart(2, '0')}:${String(mins).padStart(2, '0')}`;
  };

  const toggleMenu = () => {
    setIsMenuOpen(true);
    if (isOverlayOpen) setIsOverlayOpen(false);
    if (timeoutRef.current) {
      clearTimeout(timeoutRef.current);
      timeoutRef.current = null;
    }
  };

  const closeMenuWithDelay = () => {
    timeoutRef.current = setTimeout(() => {
      setIsMenuOpen(false);
    }, 500);
  };

  const cancelCloseMenu = () => {
    if (timeoutRef.current) {
      clearTimeout(timeoutRef.current);
      timeoutRef.current = null;
    }
  };

  const toggleOverlay = () => {
    setIsOverlayOpen(!isOverlayOpen);
    setOffsetError(null);
    if (isMenuOpen) setIsMenuOpen(false);
  };

  const updateOffset = () => {
    if (!wsRef?.current || wsRef.current.readyState !== WebSocket.OPEN) {
      setOffsetError('WebSocket not connected');
      console.error('WebSocket not connected for set_time_offset');
      return;
    }
    const browserOffset = -new Date().getTimezoneOffset();
    const message = {
      command: 'set_time_offset',
      offset_minutes: browserOffset,
    };
    try {
      console.log('Sending set_time_offset:', message);
      wsRef.current.send(JSON.stringify(message));
      setIsOverlayOpen(false);
    } catch (error) {
      setOffsetError('Failed to update offset');
      console.error('Error sending set_time_offset:', error);
    }
  };

  const memPercent = message?.mem_total
    ? Math.round(((message.mem_total - message.mem_used) / message.mem_total) * 100)
    : 0;

  const browserOffset = -new Date().getTimezoneOffset();
  const isOffsetMismatch = message?.offset_minutes != null && message.offset_minutes !== browserOffset;

  return (
    <div className="time-bar">
      <button
        className="time-display"
        onClick={toggleOverlay}
        aria-label="Adjust time offset"
      >
        Time:{' '}
        <span className={isOffsetMismatch ? 'offset-mismatch' : ''}>
          {formatTime(message?.epoch, message?.offset_minutes)}
        </span>
      </button>
      <div>-</div>
      <div>Memory Free: {memPercent}%</div>
      <button
        className="hamburger"
        onMouseEnter={toggleMenu}
        onMouseLeave={closeMenuWithDelay}
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
        <div
          className="menu"
          onMouseEnter={cancelCloseMenu}
          onMouseLeave={closeMenuWithDelay}
        >
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
      {isOverlayOpen && (
        <div className="overlay-backdrop" onClick={toggleOverlay}>
          <div className="overlay" onClick={(e) => e.stopPropagation()}>
            <h2 className="overlay-header">Time Offset Settings</h2>
            <p>ESP32 Time Offset: {formatOffset(message?.offset_minutes)}</p>
            <p>Local Browser Offset: {formatOffset(browserOffset)}</p>
            <p>Update ESP32 offset to local browser time?</p>
            {offsetError && <p className="overlay-error">{offsetError}</p>}
            <div className="overlay-buttons">
              <button
                className="save-button"
                onClick={updateOffset}
                disabled={!wsRef?.current || wsRef.current.readyState !== WebSocket.OPEN}
              >
                Update
              </button>
              <button
                className="cancel-button"
                onClick={toggleOverlay}
              >
                Cancel
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}

export default TimeBar;