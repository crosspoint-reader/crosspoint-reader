/* The shared navigation bar (shared/NavLinks.html.inc). The including page
   defines --font-color, --accent-color and --accent-hover-color. */
.nav-links {
  margin: 20px 0;
  display: flex;
  flex-wrap: wrap;
  gap: 10px;
}
.nav-links a {
  padding: 10px 20px;
  color: var(--font-color);
  text-decoration: none;
  border-radius: 4px;
}
.nav-links a.active {
  background-color: var(--accent-color);
  color: white;
}
.nav-links a:not(.active):hover {
  background-color: var(--accent-hover-color);
  color: white;
}
@media (max-width: 600px) {
  .nav-links a {
    padding: 8px 12px;
    font-size: 0.9em;
  }
}
