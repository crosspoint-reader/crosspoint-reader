# Controls

Open **Settings → Controls** to configure buttons and touch input. The available
options depend on the device.

- **Home button gestures** opens the existing gesture assignments.
- **Power button** contains **Short click** and **Double click**.
- **Front buttons** contains remapping, orientation, Back behavior, and the two
  long-press settings at the bottom.
- **Side buttons** contains the reader layout, orientation, and page-turn long press.
- **Taps and gestures** contains **Touch controls while reading**, the page/menu
  gestures, and the four **top-corner shortcuts** (**Top left tap**, **Top left
  long press**, **Top right tap**, **Top right long press**).

**Top-corner shortcuts** run while reading an EPUB, TXT, or Markdown book (XTC
books do not use them). Each corner gesture is assigned independently; the
top-left corner starts **Disabled**, which keeps the normal tap behavior and
word selection there, while top-right tap runs **Toggle Bookmark** and
top-right long press runs **Bookmarks**. Each target is a square whose side is
15% of the screen's shorter dimension. A bound tap is removed from the
page-turn tap zone under it, so it runs its action instead of turning the page;
a bound long press replaces word selection in that corner. Set that gesture to
**Disabled** to give the spot back to its normal meaning. Tap and long press
are independent: a corner with a disabled tap behaves like any other tap on
the page, and a corner with a disabled long press still starts word selection.
Tap and long press can be bound to **Toggle Bookmark**, **Bookmarks**,
**Select Chapter**, **Reader Menu**, **Look Up**, **Links and Footnotes**,
**Night Mode**, **Go to %**, or **CrossPoint Sync**. An action the device
cannot perform (for example footnotes on a page without them, or a lookup with
no dictionary) falls back to the normal zone behavior. The shortcuts follow
**Touch controls while reading**.

**Page turn long press** is shared by Front buttons and Side buttons. Changing it
in either group changes the same setting.

**Double click** offers the same actions as **Short click**, plus **Frontlight**
to toggle the light on devices that have one. It is not offered where the power
button is also Confirm. It defaults to Frontlight on X4 Pro and EEGO A4 and to
Ignore elsewhere. With a double-click action enabled, a single
click waits up to half a second before its action runs. A double click runs only
its assigned action. Set Double click to **Ignore** to remove this wait.
Reader actions such as page turn and footnotes apply while reading. Sleep,
screen refresh, Confirm, and the frontlight retain their usual behavior.

On X4 Pro, set **Home button gestures → Tap → Browse Files** to return from
a reading page to the folder containing the current book. You can also assign
Browse Files to Double Tap or Long Press. These assignments apply while reading;
the default Tap action remains Home. Set Double Tap to Ignore if you want a
single tap to act without waiting for a possible second tap.
