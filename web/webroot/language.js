(() => {
  // Keep edits in memory only: passwords and selected files must never be
  // copied into browser storage just to change the interface language.
  const fields = [...document.querySelectorAll('form input, form select, form textarea')]
    .filter(field => field.name !== 'lang' && !['hidden', 'submit', 'button'].includes(field.type));
  const value = field => field.type === 'checkbox' || field.type === 'radio' ? field.checked : field.value;
  const original = fields.map(value);
  const allow = select => !fields.some((field, i) => value(field) !== original[i]) || window.confirm(select.dataset.unsaved);
  const nav = document.getElementById('lang-nav');
  if (nav && nav.form) {
    const previous = nav.value;
    nav.form.querySelectorAll('.lang-apply').forEach(button => { button.hidden = true; });
    nav.addEventListener('change', () => {
      if (!allow(nav)) { nav.value = previous; return; }
      // Parameter tabs use a fragment. The server-supplied path already
      // retains file/directory queries; keep the current tab on reload too.
      const next = nav.form.elements.namedItem('next');
      if (next) next.value = next.value.split('#')[0] + window.location.hash;
      nav.form.submit();
    });
  }
  const login = document.getElementById('lang-login');
  if (login) {
    const previous = login.value;
    login.addEventListener('change', () => {
      if (!allow(login)) { login.value = previous; return; }
      window.location.replace('/login?lang=' + encodeURIComponent(login.value));
    });
  }
})();
