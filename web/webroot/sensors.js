(() => {
  const script = document.currentScript;
  const lidar = document.getElementById('lidar');
  const lidarToggle = document.getElementById('lidar-toggle');
  const minimum = document.getElementById('thermal-min');
  const maximum = document.getElementById('thermal-max');
  const cpu = document.getElementById('cpu-temperature');
  const status = document.getElementById('sensor-status');
  if (!script || !lidar || !lidarToggle || !minimum || !maximum || !cpu || !status) return;
  let issued = 0;
  let applied = 0;
  const temperature = (value, x, y) => value === null ? L.unavailable : value.toFixed(2) + L.temperatureAt + x + ', ' + y + ')';
  const refresh = async () => {
    const requestId = ++issued;
    try {
      const response = await fetch('/sensors.json', {cache: 'no-store'});
      if (!response.ok) throw new Error('HTTP ' + response.status);
      const data = await response.json();
      if (requestId < applied) return;
      applied = requestId;
      if (data.lidar_enabled === false) lidar.textContent = L.disabled;
      else lidar.textContent = data.lidar_m === null ? L.unavailable : (data.lidar_m === 0 ? L.noReturn : data.lidar_m.toFixed(1) + ' m');
      lidarToggle.textContent = data.lidar_enabled ? L.disable : L.enable;
      lidarToggle.dataset.action = data.lidar_enabled ? 'disable' : 'enable';
      lidarToggle.disabled = data.lidar_enabled === null;
      minimum.textContent = temperature(data.minimum_c, data.minimum_x, data.minimum_y);
      maximum.textContent = temperature(data.maximum_c, data.maximum_x, data.maximum_y);
      cpu.textContent = data.cpu_c === null ? L.unavailable : data.cpu_c.toFixed(1) + ' °C';
      status.textContent = L.updated + new Date().toLocaleTimeString() + L.twicePerSecond;
    } catch (error) {
      if (requestId < applied) return;
      applied = requestId;
      lidar.textContent = minimum.textContent = maximum.textContent = cpu.textContent = L.unavailable;
      lidarToggle.disabled = true;
      status.textContent = L.sensorFailed + (error && error.message ? error.message : L.unknownError);
    }
  };
  lidarToggle.addEventListener('click', async () => {
    const action = lidarToggle.dataset.action;
    if (action !== 'enable' && action !== 'disable') return;
    lidarToggle.disabled = true;
    const body = new URLSearchParams({csrf: script.dataset.csrf, action});
    try {
      const response = await fetch('/sensors/lidar', {method: 'POST', headers: {'Content-Type': 'application/x-www-form-urlencoded'}, body});
      if (!response.ok) throw new Error((await response.text()).trim() || ('HTTP ' + response.status));
      lidar.textContent = action === 'enable' ? L.waitingRange : L.disabled;
      lidarToggle.textContent = action === 'enable' ? L.disable : L.enable;
      lidarToggle.dataset.action = action === 'enable' ? 'disable' : 'enable';
      status.textContent = action === 'enable' ? L.lidarEnabled : L.lidarDisabled;
    } catch (error) {
      status.textContent = L.lidarControlFailed + (error && error.message ? error.message : L.unknownError);
    } finally {
      lidarToggle.disabled = false;
      window.setTimeout(refresh, 100);
    }
  });
  refresh();
  window.setInterval(refresh, 500);
})();
