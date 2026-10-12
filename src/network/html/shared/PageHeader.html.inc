<!-- The bar across the top of every page: the reader's name, which leads back to the start page,
     the navigation bar and the colour mode switch. Styled by shared/Theme.css.inc and
     shared/NavLinks.css.inc. -->
<header class="app-header">
  <div class="app-header-inner">
    <a class="brand" href="/">📚 %%CROSSPOINT%%</a>
    <!--#include file="shared/NavLinks.html.inc" -->
    <button type="button" class="theme-toggle"></button>
    <script>
      // Auto, Light, Dark in turn. Auto follows the system and stores nothing. The choice is
      // applied early by shared/ThemeInit.html.inc; here it is changed, and followed when another
      // tab changes it.
      (() => {
        const button = document.currentScript.previousElementSibling;
        const MODES = ['auto', 'light', 'dark'];
        const NAMES = { auto: 'Automatic (as the system)', light: 'Light', dark: 'Dark' };
        const ICONS = {
          auto: '<circle cx="12" cy="12" r="8"/><path d="M12 4a8 8 0 0 1 0 16z" fill="currentColor"/>',
          light: '<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M2 12h2M20 12h2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4' +
            'M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/>',
          dark: '<path d="M20 14.5A8 8 0 0 1 9.5 4a8 8 0 1 0 10.5 10.5z"/>'
        };
        const stored = () => {
          try { return localStorage.getItem('theme') || 'auto'; } catch (e) { return 'auto'; }
        };
        const apply = (chosen) => {
          if (chosen === 'auto') delete document.documentElement.dataset.theme;
          else document.documentElement.dataset.theme = chosen;
          button.innerHTML = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" ' +
            'stroke-linecap="round" aria-hidden="true">' + ICONS[chosen] + '</svg>';
          button.title = 'Colour mode: ' + NAMES[chosen];
          button.setAttribute('aria-label', button.title);
        };
        // Tracked here rather than re-read, so the switch still cycles where storage is blocked.
        let mode = stored();
        apply(mode);
        button.addEventListener('click', () => {
          mode = MODES[(MODES.indexOf(mode) + 1) % MODES.length];
          try {
            if (mode === 'auto') localStorage.removeItem('theme');
            else localStorage.setItem('theme', mode);
          } catch (e) { /* not kept, but still applied to this page */ }
          apply(mode);
        });
        window.addEventListener('storage', (e) => {
          if (e.key === 'theme') apply(mode = stored());
        });
      })();
    </script>
  </div>
</header>
