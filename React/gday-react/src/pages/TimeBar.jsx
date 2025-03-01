import '../styles.css';

function TimeBar({ message }) {
  const formatTime = (epoch) => {
    if (!epoch) return 'N/A';
    const date = new Date(epoch * 1000);
    return date.toLocaleString();
  };

  const memPercent = message?.mem_total
    ? Math.round(((message.mem_total - message.mem_used) / message.mem_total) * 100)
    : 0;

  return (
    <div className="time-bar">
      <div>Time: {formatTime(message?.epoch)}</div>
      <div>-</div>
      <div>Memory Free: {memPercent}%</div>
    </div>
  );
}

export default TimeBar;