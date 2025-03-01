import { Routes, Route } from 'react-router-dom'; // Remove BrowserRouter import
import TimeBar from './TimeBar';
import Home from './pages/Home';
import Graph from './pages/Graph';
import './styles.css';

function App() {
  return (
    <div className="App">
      <TimeBar />
      <Routes>
        <Route path="/" element={<Home />} />
        <Route path="/graph" element={<Graph />} />
      </Routes>
    </div>
  );
}

export default App;