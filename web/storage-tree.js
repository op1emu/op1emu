// Wunderbaum adapter for the USB file browser. The Worker and operation
// serialization stay in storage-ui.js; this module only renders directory data.
import { Wunderbaum } from "wunderbaum";
import "wunderbaum/dist/wunderbaum.css";

const join = (path, name) => path === "/" ? `/${name}` : `${path}/${name}`;
const pad = (value) => String(value).padStart(2, "0");
const formatSize = (size) => size < 1024 ? `${size} B` : size < 1024 * 1024
  ? `${(size / 1024).toFixed(1)} KiB` : `${(size / 1024 / 1024).toFixed(1)} MiB`;

function formatModified(date, time) {
  if (!Number.isInteger(date) || !Number.isInteger(time) || !date) return "";
  const year = 1980 + (date >> 9), month = (date >> 5) & 15, day = date & 31;
  const hour = time >> 11, minute = (time >> 5) & 63;
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59) return "";
  return `${year}-${pad(month)}-${pad(day)} ${pad(hour)}:${pad(minute)}`;
}

function childNodes(entries, parentPath) {
  return [...entries].sort((a, b) => Number(b.directory) - Number(a.directory) ||
    a.name.localeCompare(b.name, undefined, { sensitivity: "base" })).map((entry) => {
    const path = join(parentPath, entry.name);
    return {
      title: entry.name,  // Wunderbaum escapes titles; never render FAT names as HTML.
      key: path,
      path,
      size: entry.size,
      directory: entry.directory,
      modified: formatModified(entry.modifiedDate, entry.modifiedTime),
      lazy: entry.directory,
      classes: entry.directory ? "storage-directory" : "storage-file",
    };
  });
}

export function createStorageTree({ element, isInteractive, onDirectory, onContext, onLazyLoad, onError }) {
  const iconMap = {
    ...Wunderbaum.iconMaps.bootstrap,
    expanderExpanded: "storage-expander expanded",
    expanderCollapsed: "storage-expander",
    expanderLazy: "storage-expander",
    folder: "storage-folder",
    folderOpen: "storage-folder open",
    folderLazy: "storage-folder",
    doc: "storage-document",
  };
  const tree = new Wunderbaum({
    id: "usb-storage",
    element,
    source: [],
    columns: [
      { id: "*", title: "Name", width: "*" },
      { id: "size", title: "Size", width: "72px" },
      { id: "modified", title: "Modified", width: "128px" },
    ],
    header: true,
    rowHeightPx: 26,
    iconMap,
    tooltip: true,
    selectMode: "single",
    navigationModeOption: "row",  // Finder selects whole rows, never individual cells.
    debugLevel: 1,
    activate: ({ node }) => {
      if (node.data.directory) onDirectory(node.data.path);
    },
    beforeActivate: () => isInteractive() ? undefined : false,
    beforeExpand: () => isInteractive() ? undefined : false,
    click: ({ node, info, event }) => {
      if (!isInteractive()) return false;
      // Select a directory and load/expand it with one click. Arrow keys still
      // use the tree's native navigation; the expander only toggles the branch.
      if (node.data.directory && info.region === "title") {
        void node.setActive(true, { event });
        node.setFocus();
        void node.setExpanded(!node.isExpanded());
        return false;
      }
    },
    lazyLoad: async ({ node }) => childNodes(await onLazyLoad(node.data.path), node.data.path),
    render: ({ node, nodeElem, renderColInfosById }) => {
      const row = nodeElem.parentElement;
      row.dataset.storagePath = node.data.path;
      row.classList.toggle("storage-drop-target", element.dataset.dropPath === node.data.path);
      const size = renderColInfosById.size?.elem;
      if (size) {
        size.textContent = node.data.directory ? "" : formatSize(node.data.size);
        size.classList.add("storage-size");
      }
      const modified = renderColInfosById.modified?.elem;
      if (modified) {
        modified.textContent = node.data.modified;
        modified.classList.add("storage-modified");
      }
      // A quiet overflow affordance provides the same menu on touch devices.
      let button = nodeElem.querySelector(":scope > .storage-row-actions");
      if (!button) {
        button = document.createElement("button");
        button.type = "button";
        button.className = "storage-row-actions";
        button.textContent = "⋯";
        button.tabIndex = -1;
        nodeElem.append(button);
      }
      button.setAttribute("aria-label", `Actions for ${node.title}`);
    },
    error: ({ error }) => onError(error),
  });

  const rowFor = (node) => [...element.querySelectorAll("[data-storage-path]")]
    .find((row) => row.dataset.storagePath === node?.data.path);
  const describe = (node) => ({
    path: node.data.path, name: node.title, size: node.data.size, directory: node.data.directory,
  });
  function showContext(node, position) {
    if (!node || !isInteractive()) return;
    node.setFocus();
    onContext(describe(node), position);
  }

  element.addEventListener("contextmenu", (event) => {
    const row = event.target.closest("[data-storage-path]");
    if (!row || !element.contains(row)) {
      if (event.target.closest(".wb-header") || !isInteractive()) return;
      event.preventDefault();
      event.stopPropagation();
      tree.setFocus();
      onContext({ path: "/", name: "/", size: 0, directory: true }, { x: event.clientX, y: event.clientY });
      return;
    }
    event.preventDefault();
    event.stopPropagation();
    showContext(tree.findKey(row.dataset.storagePath), { x: event.clientX, y: event.clientY });
  });
  element.addEventListener("click", (event) => {
    const button = event.target.closest(".storage-row-actions");
    if (!button) return;
    event.preventDefault();
    event.stopPropagation();
    const row = button.closest("[data-storage-path]");
    const rect = button.getBoundingClientRect();
    showContext(tree.findKey(row.dataset.storagePath), { x: rect.left, y: rect.bottom + 3 });
  }, true);
  element.addEventListener("keydown", (event) => {
    if (event.key !== "ContextMenu" && !(event.shiftKey && event.key === "F10")) return;
    const node = tree.getFocusNode() ?? tree.getActiveNode();
    const row = rowFor(node);
    if (!row) return;
    event.preventDefault();
    event.stopPropagation();
    const rect = row.getBoundingClientRect();
    showContext(node, { x: rect.left + 28, y: rect.bottom + 3 });
  });

  return {
    clear: () => tree.load([]),
    loadRoot: (entries) => tree.load(childNodes(entries, "/")),
    refresh: async (path, entries) => {
      if (path === "/") {
        await tree.load(childNodes(entries, path));
        return true;
      }
      const node = tree.findKey(path);
      if (!node) return false;
      const wasExpanded = node.expanded;
      node.removeChildren();
      await node.load(childNodes(entries, path));
      if (wasExpanded) await node.setExpanded(true, { noEvents: true });
      return true;
    },
    setEnabled: (enabled) => {
      tree.setEnabled(enabled);
      element.setAttribute("aria-disabled", String(!enabled));
      for (const button of element.querySelectorAll("button")) button.disabled = !enabled;
    },
  };
}
