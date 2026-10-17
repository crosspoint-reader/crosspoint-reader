// --- dialogs: closing on a click beside one (shared by the File Manager and its Details dialog) ---
// A click on the dimmed backdrop around a dialog closes it. The browser reports a click on the
// element that holds both the press and the release, though, so selecting the text of an input in
// the dialog and letting go of the button beside it reported a click on the backdrop: the dialog
// closed and took what was typed with it. Only a click that also began on the backdrop closes it
// now. The press is remembered per backdrop and forgotten at every click, so a dialog opened again
// does not close on a press from before.
//
// Ported from crosspoint-reader PR #3931 (Crestwave), with the per-backdrop press and its reset
// from CrossInk (uxjulia/CrossInk) commit 753e555e (Julia Nguyen), MIT.
function closeOnBackdropClick(overlay, close) {
  let pressedOnBackdrop = false;
  overlay.addEventListener('mousedown', (e) => {
    pressedOnBackdrop = e.target === overlay;
  });
  overlay.addEventListener('click', (e) => {
    const onBackdrop = pressedOnBackdrop && e.target === overlay;
    pressedOnBackdrop = false;
    if (onBackdrop) close();
  });
}
