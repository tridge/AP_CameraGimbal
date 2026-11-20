(() => {
  const form = document.getElementById('time-sync');
  const browserTime = document.getElementById('browser-time-ms');
  if (!form || !browserTime) return;
  form.addEventListener('submit', () => { browserTime.value = Date.now().toString(); });
})();
