import { useState, useEffect } from 'react';
import PropTypes from 'prop-types';
import '../styles.css';

function Settings({ requestNetworkInfo, networkInfo, connectionStatus, sensors, refreshSensors }) {
  const [isWaitingForData, setIsWaitingForData] = useState(false);
  const [showAdvanced, setShowAdvanced] = useState(false);
  const [hasRequested, setHasRequested] = useState(false);

  useEffect(() => {
    console.log('Settings page mounted');
    setIsWaitingForData(true); // Set flag on load
  }, []);

  useEffect(() => {
    if (connectionStatus === 'Connected' && isWaitingForData && !hasRequested) {
      console.log('Connection established - requesting network info');
      requestNetworkInfo();
      setHasRequested(true); // Prevent re-request
    } else if (connectionStatus !== 'Connected' && hasRequested) {
      setHasRequested(false); // Reset on disconnect
    }
  }, [connectionStatus, isWaitingForData, hasRequested, requestNetworkInfo]);

  return (
    <div>
      <div className="Title">
        <h1>Settings</h1>
      </div>
      <div className="Tile-container">
        <div className="Tile">
          <h2>Network Settings</h2>
          
          {isWaitingForData && connectionStatus !== 'Connected' ? (
            <div>Waiting for WebSocket connection ({connectionStatus})</div>
          ) : networkInfo ? (
            <>
              {networkInfo.wifi_ip && (
                <>
                  <div>WiFi IP: {networkInfo.wifi_ip}</div>
                  <div>
                    WiFi RSSI:{' '}
                    {networkInfo.wifi_rssi !== 0 ? `${networkInfo.wifi_rssi} dBm` : 'N/A'}
                  </div>
                </>
              )}
              {networkInfo.eth_ip && <div>ETH IP: {networkInfo.eth_ip}</div>}
              <div>mDNS Hostname: {networkInfo.mdns_hostname || 'N/A'}</div>
              <a href="#" onClick={() => setShowAdvanced(!showAdvanced)}>
                {showAdvanced ? 'Hide Advanced' : 'Show Advanced'}
              </a>
              {showAdvanced && (
                <>
                <p>
                  {networkInfo.wifi_ip && (
                  <>
                  WiFi Gateway: {networkInfo.wifi_gateway || 'N/A'}<br/>
                  WiFi DNS: {networkInfo.wifi_dns || 'N/A'}<br/>
                  WiFi DNS2: {networkInfo.wifi_dns_2 || 'N/A'}<br/>
                  WiFi Subnet: {networkInfo.wifi_subnet || 'N/A'}<br/>
                  WiFi MAC: {networkInfo.wifi_mac || 'N/A'}<br/>
                  </>)}
                  {networkInfo.eth_gateway && (
                  <>
                  ETH Gateway: {networkInfo.eth_gateway || 'N/A'}<br/>
                  ETH DNS: {networkInfo.eth_dns || 'N/A'}<br/>
                  ETH DNS2: {networkInfo.eth_dns_2 || 'N/A'}<br/>
                  ETH Subnet: {networkInfo.eth_subnet || 'N/A'}<br/>
                  ETH MAC: {networkInfo.eth_mac || 'N/A'}<br/>
                  </>)}
                   
                  </p>
                </>
              )}
            </>
          ) : (
            <div>Loading network info...</div>
          )}
          {!showAdvanced && (<p></p>)}
          <div className='center'>
          <button
          className="save-button"
            onClick={() => {
              console.log('Manual refresh - requesting network info');
              requestNetworkInfo();
            }}
          >
            Refresh Network Info
          </button>
          </div>
        </div>
        <div className="Tile">
          <h2>Discovered Sensors</h2>
          {sensors.length > 0 ? (
            <ul>
              {sensors.map((sensor, index) => (
                <li key={`${sensor.type}_${sensor.address}_${index}`}>
                  {sensor.type} {sensor.address}
                </li>
              ))}
            </ul>
          ) : (
            <div>No sensors detected</div>
          )}<div className='center'>
          <button
          className="save-button"
            onClick={() => {
              console.log('Requesting sensor scan');
              refreshSensors();
            }}
          >
            Rescan for sensors
          </button>
          </div>
        </div>
      </div>
    </div>
  );
}

Settings.propTypes = {
  requestNetworkInfo: PropTypes.func.isRequired,
  connectionStatus: PropTypes.string.isRequired,
  networkInfo: PropTypes.shape({
    wifi_ip: PropTypes.string,
    wifi_rssi: PropTypes.number,
    eth_ip: PropTypes.string,
    wifi_mac: PropTypes.string,
    wifi_gateway: PropTypes.string,
    wifi_subnet: PropTypes.string,
    wifi_dns: PropTypes.string,
    wifi_dns_2: PropTypes.string,
    eth_mac: PropTypes.string,
    eth_gateway: PropTypes.string,
    eth_subnet: PropTypes.string,
    eth_dns: PropTypes.string,
    eth_dns_2: PropTypes.string,
    mdns_hostname: PropTypes.string,
  }),
  sensors: PropTypes.arrayOf(
    PropTypes.shape({
      type: PropTypes.string.isRequired,
      address: PropTypes.string.isRequired,
      active: PropTypes.bool.isRequired,
    })
  ).isRequired,
  refreshSensors: PropTypes.func,
};

export default Settings;