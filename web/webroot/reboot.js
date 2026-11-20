(() => {
  const script = document.currentScript;
  const status = document.getElementById('reboot-status');
  const heading = document.getElementById('reboot-heading');
  if (!script || !status || !heading) return;
  const started = Date.now();
  const poll = () => {
    if (Date.now() - started >= 120000) {
      status.textContent = status.dataset.timeout;
      return;
    }
    const check = new XMLHttpRequest();
    check.open('GET', '/upgrade-status?t=' + Date.now());
    check.timeout = 2000;
    const retry = () => {
      status.textContent = status.dataset.wait;
      setTimeout(poll, 1000);
    };
    check.onload = () => {
      const token = check.responseText.trim();
      // A restart changes the server token, or invalidates the browser's
      // session. A response from the old server is not completion.
      if (check.status === 401 || (check.status === 200 &&
          /^[0-9a-f]{64}$/.test(token) && token !== script.dataset.csrf)) {
        heading.textContent = status.dataset.back;
        status.textContent = check.status === 401 ? status.dataset.login : status.dataset.back;
        window.location.replace('/');
        return;
      }
      retry();
    };
    check.onerror = check.ontimeout = retry;
    check.send();
  };
  poll();
})();
