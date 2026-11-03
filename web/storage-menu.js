// Small native context menu for storage rows. Positioning and focus stay here;
// storage-ui.js owns the operations invoked by the menu.
export function createStorageMenu({ element, onAction }) {
  const items = [...element.querySelectorAll("[data-action]")];
  let target = null;
  let returnFocus = null;

  const visibleItems = () => items.filter((item) => !item.hidden);

  function close(restoreFocus = false) {
    if (element.hidden) return;
    element.hidden = true;
    target = null;
    const origin = returnFocus;
    returnFocus = null;
    if (restoreFocus && origin?.isConnected) origin.focus();
  }

  function open(nextTarget, { x, y }) {
    const origin = document.activeElement;
    close(false);
    target = Object.freeze({ ...nextTarget });
    returnFocus = origin;
    for (const item of items) {
      const action = item.dataset.action;
      item.hidden = target.directory ? action !== "upload" : action === "upload";
    }
    element.hidden = false;
    // Measure before clamping so long menus never extend beyond the viewport.
    element.style.left = "0px";
    element.style.top = "0px";
    const rect = element.getBoundingClientRect();
    const left = Math.max(8, Math.min(x, window.innerWidth - rect.width - 8));
    const top = Math.max(8, Math.min(y, window.innerHeight - rect.height - 8));
    element.style.left = `${left}px`;
    element.style.top = `${top}px`;
    visibleItems()[0]?.focus();
  }

  element.addEventListener("click", (event) => {
    const item = event.target.closest("[data-action]");
    if (!item || item.hidden || !target) return;
    event.preventDefault();
    event.stopPropagation();
    const action = item.dataset.action;
    const selected = target;
    close(true);
    onAction(action, selected);
  });

  element.addEventListener("keydown", (event) => {
    const visible = visibleItems();
    const at = visible.indexOf(document.activeElement);
    let next = null;
    if (event.key === "ArrowDown") next = visible[(at + 1 + visible.length) % visible.length];
    else if (event.key === "ArrowUp") next = visible[(at - 1 + visible.length) % visible.length];
    else if (event.key === "Home") next = visible[0];
    else if (event.key === "End") next = visible.at(-1);
    else if (event.key === "Escape") {
      event.preventDefault();
      event.stopPropagation();
      close(true);
      return;
    } else if (event.key === "Tab") {
      close(true);
      return;
    } else return;
    event.preventDefault();
    event.stopPropagation();
    next?.focus();
  });

  element.addEventListener("contextmenu", (event) => event.preventDefault());
  document.addEventListener("pointerdown", (event) => {
    if (!element.hidden && !element.contains(event.target)) close(false);
  }, true);
  window.addEventListener("blur", () => close(false));
  window.addEventListener("resize", () => close(false));
  return { open, close };
}
