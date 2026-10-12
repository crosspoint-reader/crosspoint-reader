<!-- The navigation bar every page shows. A page includes this where its bar
     goes, and shared/NavLinks.css.inc with its styles; a new page is added here
     once instead of to every page. No tag names in this comment: the minifier
     would take one for a real tag. -->
<nav class="nav-links">
  <a href="/files">File Manager</a>
  <a href="/settings">Settings</a>
  <a href="/fonts">Font Manager</a>
  <a href="/dictionary">Dictionaries</a>
  <a href="/stats">Reading Stats</a>
  <a href="/plugins">Plugins</a>
  <a href="/systeminfo">System Info</a>
  <a href="/diagnostics">Diagnostics</a>
</nav>
<script>
  // Highlight the page being shown. Runs inline, so before the first paint; the
  // start page at / matches no link.
  for (const link of document.currentScript.previousElementSibling.querySelectorAll('a')) {
    if (link.getAttribute('href') === location.pathname) link.classList.add('active');
  }
</script>
