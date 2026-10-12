/* A menu of panels beside the page's content (Settings, Font Manager); a strip of chips above it
   on a narrow screen. Markup:
     .side-layout > aside.side-nav > nav > (.cat-group heading, .cat-list > a.cat-link)*, then the panels.
   A link may end in a .cat-count. */
.side-layout {
  display: grid;
  grid-template-columns: 236px minmax(0, 1fr);
  gap: 32px;
  align-items: start;
}
.side-nav { position: sticky; top: 76px; }
.cat-group { font-size: 0.72rem; font-weight: 600; letter-spacing: 0.06em; text-transform: uppercase;
  color: var(--text-muted); margin: 16px 10px 4px; }
.cat-group:first-child { margin-top: 0; }
.cat-list { display: flex; flex-direction: column; gap: 1px; }
.cat-link {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
  padding: 7px 10px;
  border-radius: var(--radius-sm);
  color: var(--text);
  text-decoration: none;
}
.cat-link:hover { background: var(--surface-2); }
.cat-link.active { background: var(--accent-soft); color: var(--accent-text); font-weight: 600; }
.cat-count { flex: none; font-size: 0.8rem; font-weight: 500; color: var(--text-muted); }
.cat-count:empty { display: none; }
.cat-count.attention { min-width: 20px; padding: 0 6px; border-radius: 999px; text-align: center;
  background: var(--accent); color: var(--on-accent); }

@media (max-width: 860px) {
  .side-layout { grid-template-columns: minmax(0, 1fr); gap: 8px; }
  .side-nav {
    top: 0;
    z-index: 10;
    background: var(--bg);
    margin: 0 -16px;
    padding: 8px 16px 10px;
    border-bottom: 1px solid var(--border);
  }
  .cat-group { display: none; }
  .side-nav nav { display: flex; gap: 6px; overflow-x: auto; scrollbar-width: none; min-width: 0; }
  .side-nav nav::-webkit-scrollbar { display: none; }
  .cat-list { flex: none; flex-direction: row; gap: 6px; }
  .cat-link { white-space: nowrap; border: 1px solid var(--border); border-radius: 999px; padding: 5px 12px;
    background: var(--surface); }
}
