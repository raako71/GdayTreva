import { useState, useEffect } from 'react';
import PropTypes from 'prop-types';
import '../styles.css';

function Settings({ requestNetworkInfo, networkInfo, connectionStatus }) {
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
          <button
            onClick={() => {
              console.log('Manual refresh - requesting network info');
              requestNetworkInfo();
            }}
          >
            Refresh Network Info
          </button>
          {isWaitingForData && connectionStatus !== 'Connected' ? (
            <div>Waiting for WebSocket connection ({connectionStatus})</div>
          ) : networkInfo ? (
            <>
              {networkInfo.wifi_ip !== 'N/A' && (
                <>
                  <div>WiFi IP: {networkInfo.wifi_ip}</div>
                  <div>
                    WiFi RSSI:{' '}
                    {networkInfo.wifi_rssi !== 0 ? `${networkInfo.wifi_rssi} dBm` : 'N/A'}
                  </div>
                </>
              )}
              {networkInfo.eth_ip !== 'N/A' && <div>ETH IP: {networkInfo.eth_ip}</div>}
              <a href="#" onClick={() => setShowAdvanced(!showAdvanced)}>
                {showAdvanced ? 'Hide Advanced' : 'Show Advanced'}
              </a>
              {showAdvanced && (
                <>
                  <div>WiFi MAC: {networkInfo.wifi_mac || 'N/A'}</div>
                  <div>WiFi Gateway: {networkInfo.wifi_gateway || 'N/A'}</div>
                  <div>WiFi Subnet: {networkInfo.wifi_subnet || 'N/A'}</div>
                  <div>WiFi DNS: {networkInfo.wifi_dns || 'N/A'}</div>
                  <div>ETH MAC: {networkInfo.eth_mac || 'N/A'}</div>
                  <div>ETH Gateway: {networkInfo.eth_gateway || 'N/A'}</div>
                  <div>ETH Subnet: {networkInfo.eth_subnet || 'N/A'}</div>
                  <div>ETH DNS: {networkInfo.eth_dns || 'N/A'}</div>
                  <div>mDNS Hostname: {networkInfo.mdns_hostname || 'N/A'}</div>
                </>
              )}
            </>
          ) : (
            <div>Loading network info...</div>
          )}
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
    eth_mac: PropTypes.string,
    eth_gateway: PropTypes.string,
    eth_subnet: PropTypes.string,
    eth_dns: PropTypes.string,
    mdns_hostname: PropTypes.string,
  }),
};

export default Settings;