/* The shared navigation bar (shared/NavLinks.html.inc) inside the page header
   (shared/PageHeader.html.inc). Needs shared/Theme.css.inc. On a narrow screen the links stay on
   one line and scroll sideways rather than stacking into rows. */
.nav-links {
  display: flex;
  flex: 1;
  min-width: 0;
  gap: 2px;
  overflow-x: auto;
  scrollbar-width: none;
}
.nav-links::-webkit-scrollbar { display: none; }
.nav-links a {
  padding: 7px 10px;
  border-radius: var(--radius-sm);
  color: var(--text-muted);
  text-decoration: none;
  font-weight: 500;
  font-size: 0.92rem;
  white-space: nowrap;
}
.nav-links a:hover { background: var(--surface-2); color: var(--text); }
.nav-links a.active { background: var(--accent-soft); color: var(--accent-text); }
@media (max-width: 760px) {
  .app-header { position: static; }
  .app-header-inner { flex-wrap: wrap; gap: 0; padding-top: 10px; }
  /* The name and the colour mode switch on the first line, the links below. */
  .brand { flex: 1; }
  .nav-links { order: 3; flex-basis: 100%; margin: 0 -16px; padding: 6px 16px 8px; }
}
