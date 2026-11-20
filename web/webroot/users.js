(() => {
  const form = document.getElementById('ssh-key-upload');
  const input = document.getElementById('public-key-files');
  const data = document.getElementById('public-key-data');
  const select = document.getElementById('select-public-key-files');
  const status = document.getElementById('public-key-status');
  if (!form || !input || !data || !select || !status) return;
  select.addEventListener('click', () => input.click());
  input.addEventListener('change', async () => {
    const files = Array.from(input.files || []);
    if (!files.length) return;
    if (files.some(file => !/\.pub$/i.test(file.name))) {
      status.textContent = L.selectPub; return;
    }
    select.disabled = true;
    status.textContent = L.reading;
    try {
      const contents = await Promise.all(files.map(file => file.text()));
      const combined = contents.map(text => text.trim()).filter(Boolean).join('\n');
      if (!combined) throw new Error(L.empty);
      if (new TextEncoder().encode(combined).length > 65536) {
        throw new Error(L.tooLarge);
      }
      data.value = combined;
      status.textContent = L.uploading;
      form.submit();
    } catch (error) {
      status.textContent = error && error.message ? error.message : L.unreadable;
      select.disabled = false;
    }
  });
})();
