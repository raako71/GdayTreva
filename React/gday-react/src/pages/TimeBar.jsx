import '../styles.css';
import { Link } from 'react-router-dom';
import { useState, useRef } from 'react';
import PropTypes from 'prop-types';

function TimeBar({ message, wsRef }) {
  const [isMenuOpen, setIsMenuOpen] = useState(false);
  const [isOverlayOpen, setIsOverlayOpen] = useState(false);
  const [offsetError, setOffsetError] = useState(null);
  const [isMemoryTooltipOpen, setIsMemoryTooltipOpen] = useState(false);
  const timeoutRef = useRef(null);
  const menuRef = useRef(null);
  const buttonRef = useRef(null);
  const memoryRef = useRef(null);

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
    const dayNames = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
    const day = dayNames[date.getUTCDay()];
    const dateStr = String(date.getUTCDate()).padStart(2, '0');
    const month = String(date.getUTCMonth() + 1).padStart(2, '0');
    const year = date.getUTCFullYear();
    const hours = String(date.getUTCHours()).padStart(2, '0');
    const minutes = String(date.getUTCMinutes()).padStart(2, '0');
    const seconds = String(date.getUTCSeconds()).padStart(2, '0');

    const offsetHours = Math.floor(Math.abs(offsetMinutes) / 60);
    const offsetMins = Math.abs(offsetMinutes) % 60;
    const offsetSign = offsetMinutes >= 0 ? '+' : '-';
    const offsetStr = `${offsetSign}${String(offsetHours).padStart(2, '0')}:${String(offsetMins).padStart(2, '0')}`;

    return `${day} ${dateStr}/${month}/${year}, ${hours}:${minutes}:${seconds} (${offsetStr})`;
  };

  // Format uptime from milliseconds
  const formatUptime = (millis) => {
    if (millis == null || isNaN(millis) || millis < 0) {
      return 'N/A';
    }
    const totalSeconds = Math.floor(millis / 1000);
    const days = Math.floor(totalSeconds / (24 * 3600));
    const hours = Math.floor((totalSeconds % (24 * 3600)) / 3600);
    const minutes = Math.floor((totalSeconds % 3600) / 60);
    const seconds = totalSeconds % 60;

    const parts = [];
    if (days > 0) parts.push(`${days}d`);
    if (hours > 0 || parts.length > 0) parts.push(`${hours}h`);
    if (minutes > 0 || parts.length > 0) parts.push(`${minutes}m`);
    parts.push(`${seconds}s`);

    return parts.join(' ');
  };

  // Format offset for overlay
  const formatOffset = (minutes) => {
    if (minutes == null || isNaN(minutes)) return 'N/A';
    const hours = Math.floor(Math.abs(minutes) / 60);
    const mins = Math.abs(minutes) % 60;
    const sign = minutes >= 0 ? '+' : '-';
    return `${sign}${String(hours).padStart(2, '0')}:${String(mins).padStart(2, '0')}`;
  };

  // Format memory in KB
  const formatMemory = (bytes) => {
    if (bytes == null || isNaN(bytes)) return 'N/A';
    return `${Math.round(bytes / 1024)} KB`;
  };

  const toggleMenu = () => {
    setIsMenuOpen((prev) => !prev);
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

  const closeMenu = () => {
    setIsMenuOpen(false);
    if (timeoutRef.current) {
      clearTimeout(timeoutRef.current);
      timeoutRef.current = null;
    }
  };

  const handleBackdropClick = (e) => {
    if (!menuRef.current?.contains(e.target) && !buttonRef.current?.contains(e.target)) {
      closeMenu();
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

  const memFree = message?.mem_total && message?.mem_used
    ? message.mem_total - message.mem_used
    : null;

  const browserOffset = -new Date().getTimezoneOffset();
  const isOffsetMismatch = message?.offset_minutes != null && message.offset_minutes !== browserOffset;

  return (
    <div className="time-bar">
      <Link to="/">Home</Link> - 
      <a className="time-display" onClick={toggleOverlay} aria-label="Adjust time offset">
        Device time:{' '}
        <span className={isOffsetMismatch ? 'offset-mismatch' : ''}>
          {formatTime(message?.epoch, message?.offset_minutes)}
        </span>
      </a>
      <div>-</div>
      <div
        ref={memoryRef}
        className="memory-display"
        onMouseEnter={() => setIsMemoryTooltipOpen(true)}
        onMouseLeave={() => setIsMemoryTooltipOpen(false)}
        aria-label="Memory usage details"
      >
        Memory Free: {memPercent}%
        {isMemoryTooltipOpen && (
          <div className="memory-tooltip">
            <p>Free: {formatMemory(memFree)}</p>
            <p>Used: {formatMemory(message?.mem_used)}</p>
            <p>Total: {formatMemory(message?.mem_total)}</p>
          </div>
        )}
      </div>
      <div>-</div>
      <div>Uptime: {formatUptime(message?.millis)}</div>
      <button
        ref={buttonRef}
        className="hamburger"
        onMouseEnter={toggleMenu}
        onMouseLeave={closeMenuWithDelay}
        onClick={toggleMenu}
        onFocus={toggleMenu}
        onBlur={closeMenuWithDelay}
        aria-label="Toggle navigation menu"
        aria-expanded={isMenuOpen}
        aria-controls="navigation-menu"
      >
        <svg
          className="hamburger-icon"
          fill="none"
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
          className="menu-backdrop"
          onClick={handleBackdropClick}
          role="presentation"
        >
          <div
            ref={menuRef}
            id="navigation-menu"
            className="menu"
            onMouseEnter={cancelCloseMenu}
            onMouseLeave={closeMenuWithDelay}
            onFocus={cancelCloseMenu}
            onBlur={closeMenuWithDelay}
            role="menu"
          >
            <Link
              to="/"
              className="menu-item"
              onClick={closeMenu}
              role="menuitem"
              tabIndex={0}
            >
              Home
            </Link>
            <Link
              to="/settings"
              className="menu-item"
              onClick={closeMenu}
              role="menuitem"
              tabIndex={0}
            >
              Settings
            </Link>
            <Link
              to="/graph"
              className="menu-item"
              onClick={closeMenu}
              role="menuitem"
              tabIndex={0}
            >
              Graph
            </Link>
            <Link
              to="/programs"
              className="menu-item"
              onClick={closeMenu}
              role="menuitem"
              tabIndex={0}
            >
              Programs
            </Link>
          </div>
        </div>
      )}
      {isOverlayOpen && (
        <div className="overlay-backdrop" onClick={toggleOverlay}>
          <div className="overlay" onClick={(e) => e.stopPropagation()}>
            <h2 className="overlay-header">Time Offset Settings</h2>
            <p className={isOffsetMismatch ? 'offset-mismatch' : ''}>ESP32 Time Offset: {formatOffset(message?.offset_minutes)}</p>
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
                className="save-button"
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

TimeBar.propTypes = {
  message: PropTypes.shape({
    epoch: PropTypes.number,
    offset_minutes: PropTypes.number,
    mem_total: PropTypes.number,
    mem_used: PropTypes.number,
    millis: PropTypes.number,
  }),
  wsRef: PropTypes.shape({
    current: PropTypes.instanceOf(WebSocket),
  }),
};

export default TimeBar;