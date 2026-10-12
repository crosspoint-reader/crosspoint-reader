<script>
  // Applies the colour mode chosen with the header's switch (shared/PageHeader.html.inc) before
  // the page is drawn, so it never shows the other mode first. Kept per browser; no choice, or
  // storage that cannot be read, means Auto: the system's own preference.
  try {
    const mode = localStorage.getItem('theme');
    if (mode === 'light' || mode === 'dark') document.documentElement.dataset.theme = mode;
  } catch (e) { /* private window or blocked storage: Auto */ }
</script>
