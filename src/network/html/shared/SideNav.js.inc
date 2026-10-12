// shared/SideNav.css.inc's menu becomes a row of chips on a narrow screen, which scrolls sideways:
// bring the active one into view there. Beside the page the menu does not scroll, and this does
// nothing. Called after the active link changes.
function revealActiveLink() {
  const link = document.querySelector('.side-nav .cat-link.active');
  if (!link) return;
  const nav = link.closest('nav');
  if (nav.scrollWidth <= nav.clientWidth) return;
  const left = link.offsetLeft - nav.offsetLeft;
  if (left < nav.scrollLeft || left + link.offsetWidth > nav.scrollLeft + nav.clientWidth) {
    nav.scrollLeft = Math.max(0, left - 16);
  }
}
