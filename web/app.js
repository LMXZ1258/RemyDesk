"use strict";

const state = {
  path: "",
  desktop: null,
  selectedSsid: "",
  ipv4Mode: "auto",
  selectedIcon: "",
  desktopLayout: {},
  layoutSaveTimer: null,
  dragPath: "",
  epubReader: null,
};
const $ = (selector) => document.querySelector(selector);
const $$ = (selector) => Array.from(document.querySelectorAll(selector));
const desktopLayoutKey = "remydesk.desktop-layout.v1";

function toast(message, duration = 2800) {
  const element = $("#toast");
  element.textContent = message;
  element.classList.add("show");
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => element.classList.remove("show"), duration);
}

async function copyText(text, sourceElement = null) {
  if (navigator.clipboard && window.isSecureContext) {
    await navigator.clipboard.writeText(text);
    return;
  }

  const fallback = sourceElement || document.createElement("textarea");
  const temporary = !sourceElement;
  if (temporary) {
    fallback.value = text;
    fallback.setAttribute("readonly", "");
    fallback.style.position = "fixed";
    fallback.style.opacity = "0";
    document.body.appendChild(fallback);
  }

  const selectionStart = fallback.selectionStart;
  const selectionEnd = fallback.selectionEnd;
  fallback.focus();
  fallback.select();
  const copied = document.execCommand("copy");
  if (temporary) fallback.remove();
  else if (selectionStart !== null && selectionEnd !== null) fallback.setSelectionRange(selectionStart, selectionEnd);
  if (!copied) throw new Error("浏览器未允许复制，请手动选择便签内容复制");
}

async function api(path, options = {}) {
  const response = await fetch(path, options);
  const data = await response.json().catch(() => ({ error: `HTTP ${response.status}` }));
  if (!response.ok || data.ok === false) throw new Error(data.error || `HTTP ${response.status}`);
  return data;
}

function jsonPost(path, body = {}) {
  return api(path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
}

function formatSize(value) {
  if (!value) return "-";
  const units = ["B", "KB", "MB", "GB", "TB"];
  let size = Number(value), index = 0;
  while (size >= 1024 && index < units.length - 1) {
    size /= 1024;
    index += 1;
  }
  return `${size >= 10 || index === 0 ? size.toFixed(0) : size.toFixed(1)} ${units[index]}`;
}

function extensionOf(name) {
  const dot = name.lastIndexOf(".");
  return dot > 0 ? name.slice(dot).toLowerCase() : "";
}

function fileVisual(entry) {
  if (entry.type === "dir") return { icon: "folder", kind: "文件夹", className: "folder" };
  const extension = extensionOf(entry.name);
  if ([".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".svg"].includes(extension)) {
    return { icon: "image", kind: "图片", className: "image", thumbnail: true };
  }
  if ([".mp4", ".m4v", ".webm", ".mov"].includes(extension)) {
    return { icon: "film", kind: "视频", className: "video" };
  }
  if ([".mp3", ".wav", ".ogg"].includes(extension)) {
    return { icon: "music-2", kind: "音频", className: "audio" };
  }
  if ([".md", ".markdown"].includes(extension)) {
    return { icon: "notebook-tabs", kind: "Markdown", className: "markdown" };
  }
  if ([".txt", ".log"].includes(extension)) {
    return { icon: "file-text", kind: "文本", className: "text" };
  }
  if (extension === ".pdf") return { icon: "file-type-2", kind: "PDF", className: "pdf" };
  if (extension === ".epub") return { icon: "book-open", kind: "EPUB 电子书", className: "epub" };
  if ([".zip", ".7z", ".rar", ".tar", ".gz"].includes(extension)) {
    return { icon: "archive", kind: "压缩包", className: "archive" };
  }
  if ([".js", ".ts", ".json", ".html", ".css", ".cpp", ".c", ".h", ".py", ".go"].includes(extension)) {
    return { icon: "file-code-2", kind: "代码", className: "code" };
  }
  return { icon: "file", kind: extension ? extension.slice(1).toUpperCase() : "文件", className: "file" };
}

function previewUrl(path) {
  return `/preview?path=${encodeURIComponent(path)}`;
}

function downloadUrl(path) {
  return `/file?path=${encodeURIComponent(path)}`;
}

function localDesktopLayout() {
  try {
    const value = JSON.parse(localStorage.getItem(desktopLayoutKey) || "{}");
    return value && typeof value === "object" ? value : {};
  } catch {
    return {};
  }
}

async function loadDesktopLayout() {
  const local = localDesktopLayout();
  try {
    const data = await api("/api/desktop/layout");
    state.desktopLayout = data.layout && typeof data.layout === "object" ? data.layout : {};
    if (!Object.keys(state.desktopLayout).length && Object.keys(local).length) {
      state.desktopLayout = local;
      await persistDesktopLayout();
    }
  } catch {
    state.desktopLayout = local;
  }
}

async function persistDesktopLayout() {
  localStorage.setItem(desktopLayoutKey, JSON.stringify(state.desktopLayout));
  await jsonPost("/api/desktop/layout", { layout: state.desktopLayout });
}

function queueDesktopLayoutSave() {
  clearTimeout(state.layoutSaveTimer);
  state.layoutSaveTimer = setTimeout(() => {
    persistDesktopLayout().catch((error) => toast(`图标位置保存失败：${error.message}`));
  }, 120);
}

function saveDesktopPosition(path, x, y, icon, list) {
  const maxX = Math.max(1, list.clientWidth - icon.offsetWidth - 4);
  const maxY = Math.max(1, list.clientHeight - icon.offsetHeight - 4);
  state.desktopLayout[path] = {
    rx: Math.max(0, Math.min(1, x / maxX)),
    ry: Math.max(0, Math.min(1, y / maxY)),
  };
  queueDesktopLayoutSave();
}

function removeDesktopPosition(path) {
  if (state.desktopLayout[path]) {
    delete state.desktopLayout[path];
    queueDesktopLayoutSave();
  }
}

function defaultDesktopPosition(index, width) {
  const cellWidth = 112;
  const cellHeight = 122;
  const columns = Math.max(1, Math.floor(Math.max(width - 28, cellWidth) / cellWidth));
  return {
    x: 18 + (index % columns) * cellWidth,
    y: 22 + Math.floor(index / columns) * cellHeight,
  };
}

function clampDesktopPosition(icon, list, x, y) {
  return {
    x: Math.max(4, Math.min(x, Math.max(4, list.clientWidth - icon.offsetWidth - 4))),
    y: Math.max(4, Math.min(y, Math.max(4, list.clientHeight - icon.offsetHeight - 4))),
  };
}

function selectDesktopIcon(icon, entry) {
  $$(".desktop-icon.selected").forEach((item) => item.classList.remove("selected"));
  icon.classList.add("selected");
  state.selectedIcon = entry.path;
}

function attachDesktopDrag(icon, entry, list) {
  let drag = null;
  let folderTarget = null;

  function findFolderTarget(clientX, clientY) {
    return document.elementsFromPoint(clientX, clientY)
      .map((element) => element.closest?.(".desktop-icon"))
      .find((candidate) => candidate && candidate !== icon && candidate.dataset.type === "dir") || null;
  }

  function setFolderTarget(target) {
    if (folderTarget === target) return;
    folderTarget?.classList.remove("drop-target");
    folderTarget = target;
    folderTarget?.classList.add("drop-target");
  }

  icon.addEventListener("pointerdown", (event) => {
    if (event.button !== 0 || event.target.closest(".desktop-icon-delete")) return;
    selectDesktopIcon(icon, entry);
    drag = {
      pointerId: event.pointerId,
      startX: event.clientX,
      startY: event.clientY,
      left: Number.parseFloat(icon.style.left) || 0,
      top: Number.parseFloat(icon.style.top) || 0,
      moved: false,
    };
    icon.setPointerCapture(event.pointerId);
  });
  icon.addEventListener("pointermove", (event) => {
    if (!drag || drag.pointerId !== event.pointerId) return;
    const dx = event.clientX - drag.startX;
    const dy = event.clientY - drag.startY;
    if (Math.hypot(dx, dy) > 4) drag.moved = true;
    if (!drag.moved) return;
    const position = clampDesktopPosition(icon, list, drag.left + dx, drag.top + dy);
    icon.style.left = `${position.x}px`;
    icon.style.top = `${position.y}px`;
    icon.classList.add("dragging");
    setFolderTarget(findFolderTarget(event.clientX, event.clientY));
  });
  icon.addEventListener("pointerup", async (event) => {
    if (!drag || drag.pointerId !== event.pointerId) return;
    const completedDrag = drag;
    const destination = folderTarget?.dataset.path || "";
    setFolderTarget(null);
    if (drag.moved) {
      icon.dataset.justDragged = "1";
      setTimeout(() => delete icon.dataset.justDragged, 0);
    }
    icon.classList.remove("dragging");
    drag = null;
    if (!completedDrag.moved) return;
    if (destination) {
      try {
        await moveDroppedEntry(entry.path, destination);
      } catch (error) {
        icon.style.left = `${completedDrag.left}px`;
        icon.style.top = `${completedDrag.top}px`;
        toast(error.message);
      }
      return;
    }
    saveDesktopPosition(entry.path, Number.parseFloat(icon.style.left), Number.parseFloat(icon.style.top), icon, list);
  });
  icon.addEventListener("pointercancel", () => {
    setFolderTarget(null);
    if (drag?.moved) {
      icon.style.left = `${drag.left}px`;
      icon.style.top = `${drag.top}px`;
    }
    icon.classList.remove("dragging");
    drag = null;
  });
}

function hasFileDrop(dataTransfer) {
  return Array.from(dataTransfer?.types || []).includes("Files");
}

function hasInternalDrop(dataTransfer) {
  return Array.from(dataTransfer?.types || []).includes("application/x-remydesk-path");
}

function clearDropTargets() {
  $$(".drop-target").forEach((element) => element.classList.remove("drop-target"));
  document.body.classList.remove("parent-drop-target");
}

function attachDragSource(element, entry) {
  element.draggable = true;
  element.addEventListener("dragstart", (event) => {
    const transfer = event.dataTransfer;
    if (!transfer) return;
    state.dragPath = entry.path;
    transfer.effectAllowed = entry.type === "file" ? "copyMove" : "move";
    transfer.setData("application/x-remydesk-path", entry.path);
    transfer.setData("text/plain", entry.path);
    if (entry.type === "file") {
      const url = new URL(downloadUrl(entry.path), location.href).href;
      const safeName = entry.name.replace(/:/g, "_");
      transfer.setData("text/uri-list", url);
      transfer.setData("DownloadURL", `application/octet-stream:${safeName}:${url}`);
    }
  });
  element.addEventListener("dragend", () => {
    state.dragPath = "";
    clearDropTargets();
  });
}

async function uploadDroppedFiles(files, destination) {
  let completed = 0;
  for (const file of files) {
    if (!file || !file.name) continue;
    await api(`/api/upload?path=${encodeURIComponent(destination)}&name=${encodeURIComponent(file.name)}`, {
      method: "POST",
      body: file,
    });
    completed += 1;
  }
  if (completed) toast(`${completed} 个文件已上传到 /${destination}`);
  await loadFiles();
}

async function moveDroppedEntry(source, destination) {
  if (!source) return;
  const currentParent = source.split("/").slice(0, -1).join("/");
  if (currentParent === destination) return;
  await jsonPost("/api/move", { source, destination });
  removeDesktopPosition(source);
  toast(`已移动到 /${destination}`);
  await loadFiles();
}

function attachDropTarget(element, destinationPath) {
  const destination = typeof destinationPath === "function" ? destinationPath : () => destinationPath;
  element.addEventListener("dragover", (event) => {
    if (!hasFileDrop(event.dataTransfer) && !hasInternalDrop(event.dataTransfer)) return;
    event.preventDefault();
    event.stopPropagation();
    // An internal RemyDesk drag may also expose browser file/URL flavours.
    // Always prefer our explicit path MIME type so it remains a move instead
    // of being mistaken for an external upload/copy.
    event.dataTransfer.dropEffect = hasInternalDrop(event.dataTransfer) ? "move" : "copy";
    clearDropTargets();
    element.classList.add("drop-target");
  });
  element.addEventListener("dragleave", (event) => {
    if (!element.contains(event.relatedTarget)) element.classList.remove("drop-target");
  });
  element.addEventListener("drop", async (event) => {
    if (!hasFileDrop(event.dataTransfer) && !hasInternalDrop(event.dataTransfer)) return;
    event.preventDefault();
    event.stopPropagation();
    clearDropTargets();
    try {
      const target = destination();
      if (hasInternalDrop(event.dataTransfer)) {
        const source = event.dataTransfer.getData("application/x-remydesk-path") || state.dragPath;
        await moveDroppedEntry(source, target);
      } else if (hasFileDrop(event.dataTransfer) && event.dataTransfer.files.length) {
        await uploadDroppedFiles(Array.from(event.dataTransfer.files), target);
      }
    } catch (error) {
      toast(error.message);
    }
  });
}

function attachParentOutsideDrop(surface) {
  function isOutsideSurface(event) {
    return state.path && hasInternalDrop(event.dataTransfer) && !surface.contains(event.target);
  }

  function clearParentTarget() {
    document.body.classList.remove("parent-drop-target");
  }

  document.addEventListener("dragover", (event) => {
    if (!isOutsideSurface(event)) return;
    event.preventDefault();
    event.dataTransfer.dropEffect = "move";
    clearDropTargets();
    document.body.classList.add("parent-drop-target");
  });
  document.addEventListener("drop", async (event) => {
    if (!isOutsideSurface(event)) return;
    event.preventDefault();
    clearParentTarget();
    const source = event.dataTransfer.getData("application/x-remydesk-path") || state.dragPath;
    const parent = state.path.split("/").slice(0, -1).join("/");
    try {
      await moveDroppedEntry(source, parent);
    } catch (error) {
      toast(error.message);
    }
  });
  document.addEventListener("dragend", clearParentTarget);
}

function openEntry(entry) {
  if (entry.type === "dir") loadFiles(entry.path);
  else openPreview(entry);
}

async function deleteEntry(entry) {
  if (!confirm(`删除 ${entry.name}？`)) return;
  try {
    await jsonPost("/api/delete", { path: entry.path });
    removeDesktopPosition(entry.path);
    await loadFiles();
  } catch (error) {
    toast(error.message);
  }
}

function renderDesktopEntries(entries) {
  const list = $("#fileList");
  const layout = state.desktopLayout;
  list.className = "file-list desktop-file-list";
  $("#fileSurface").classList.add("desktop-mode");
  $("#fileHead").hidden = true;
  $("#desktopHint").hidden = false;

  requestAnimationFrame(() => {
    const width = list.clientWidth;
    const rows = Math.ceil(Math.max(entries.length, 1) / Math.max(1, Math.floor(width / 112)));
    list.style.minHeight = `${Math.max(420, rows * 122 + 36)}px`;

    entries.forEach((entry, index) => {
      const visual = fileVisual(entry);
      const icon = document.createElement("div");
      icon.className = `desktop-icon type-${visual.className}`;
      icon.tabIndex = 0;
      icon.dataset.path = entry.path;
      icon.dataset.type = entry.type;
      icon.setAttribute("role", "button");
      icon.setAttribute("aria-label", `${visual.kind} ${entry.name}`);
      // Desktop icons use the pointer-based positioning/move implementation.
      // Suppress native image/URL dragging from descendants so grabbing the
      // upper thumbnail behaves exactly like grabbing the label below it.
      icon.addEventListener("dragstart", (event) => event.preventDefault());

      const visualNode = document.createElement("div");
      visualNode.className = "desktop-icon-visual";
      if (visual.thumbnail) {
        const image = document.createElement("img");
        image.src = previewUrl(entry.path);
        image.alt = "";
        image.loading = "lazy";
        image.draggable = false;
        image.addEventListener("error", () => {
          visualNode.replaceChildren();
          visualNode.innerHTML = `<i data-lucide="${visual.icon}"></i>`;
          lucide.createIcons({ nodes: [visualNode] });
        }, { once: true });
        visualNode.append(image);
      } else {
        visualNode.innerHTML = `<i data-lucide="${visual.icon}"></i>`;
      }

      const label = document.createElement("span");
      label.className = "desktop-icon-label";
      label.textContent = entry.name;

      const remove = document.createElement("button");
      remove.className = "desktop-icon-delete";
      remove.title = `删除 ${entry.name}`;
      remove.setAttribute("aria-label", `删除 ${entry.name}`);
      remove.innerHTML = `<i data-lucide="x"></i>`;
      remove.addEventListener("click", (event) => {
        event.stopPropagation();
        deleteEntry(entry);
      });

      icon.append(visualNode, label, remove);
      list.append(icon);

      const fallback = defaultDesktopPosition(index, width);
      const saved = layout[entry.path];
      const maxX = Math.max(1, list.clientWidth - icon.offsetWidth - 4);
      const maxY = Math.max(1, list.clientHeight - icon.offsetHeight - 4);
      const savedX = Number.isFinite(saved?.rx) ? saved.rx * maxX : saved?.x;
      const savedY = Number.isFinite(saved?.ry) ? saved.ry * maxY : saved?.y;
      const position = clampDesktopPosition(icon, list, savedX ?? fallback.x, savedY ?? fallback.y);
      icon.style.left = `${position.x}px`;
      icon.style.top = `${position.y}px`;

      attachDesktopDrag(icon, entry, list);
      if (entry.type === "dir") attachDropTarget(icon, entry.path);
      icon.addEventListener("click", () => {
        if (icon.dataset.justDragged) return;
        selectDesktopIcon(icon, entry);
      });
      icon.addEventListener("dblclick", () => {
        if (!icon.dataset.justDragged) openEntry(entry);
      });
      icon.addEventListener("keydown", (event) => {
        if (event.key === "Enter") openEntry(entry);
        if (event.key === "Delete") deleteEntry(entry);
      });
    });
    lucide.createIcons();
  });
}

function renderListEntries(entries) {
  const list = $("#fileList");
  list.className = "file-list";
  $("#fileSurface").classList.remove("desktop-mode");
  $("#fileHead").hidden = false;
  $("#desktopHint").hidden = true;
  list.style.minHeight = "";

  entries.forEach((entry) => {
    const visual = fileVisual(entry);
    const row = document.createElement("button");
    row.className = "file-row";
    row.innerHTML = `<span class="file-name"><i data-lucide="${visual.icon}"></i><span></span></span><span class="file-size">${entry.type === "dir" ? "-" : formatSize(entry.size)}</span><span class="delete-button" title="删除"><i data-lucide="trash-2"></i></span>`;
    row.querySelector(".file-name span").textContent = entry.name;
    attachDragSource(row, entry);
    if (entry.type === "dir") attachDropTarget(row, entry.path);
    row.addEventListener("click", (event) => {
      if (!event.target.closest(".delete-button")) openEntry(entry);
    });
    row.querySelector(".delete-button").addEventListener("click", (event) => {
      event.stopPropagation();
      deleteEntry(entry);
    });
    list.append(row);
  });
  lucide.createIcons();
}

async function loadFiles(path = state.path) {
  state.path = path;
  $("#breadcrumb").textContent = `/${path}`;
  const backButton = $("#backButton");
  backButton.disabled = !path;
  backButton.title = path ? "返回上级" : "已经位于根目录";
  if (!path) document.body.classList.remove("parent-drop-target");
  try {
    const data = await api(`/api/files?path=${encodeURIComponent(path)}`);
    const list = $("#fileList");
    list.replaceChildren();
    $("#emptyState").hidden = data.entries.length !== 0;
    if (!path) renderDesktopEntries(data.entries);
    else renderListEntries(data.entries);
  } catch (error) {
    toast(error.message);
  }
}

function appendMarkdownParagraph(container, text) {
  const paragraph = document.createElement("p");
  paragraph.textContent = text;
  container.append(paragraph);
}

function markdownTableCells(line) {
  let value = line.trim();
  if (value.startsWith("|")) value = value.slice(1);
  if (value.endsWith("|")) value = value.slice(0, -1);
  return value.split("|").map((cell) => cell.trim());
}

function isMarkdownTableSeparator(line) {
  const cells = markdownTableCells(line);
  return cells.length > 0 && cells.every((cell) => /^:?-{3,}:?$/.test(cell));
}

function renderMarkdown(source) {
  const article = document.createElement("article");
  article.className = "markdown-preview";
  const lines = source.replace(/\r\n?/g, "\n").split("\n");
  let codeBlock = null;
  let list = null;

  const endList = () => { list = null; };
  for (let index = 0; index < lines.length; index += 1) {
    const line = lines[index];
    if (/^```/.test(line)) {
      endList();
      if (codeBlock) {
        article.append(codeBlock);
        codeBlock = null;
      } else {
        codeBlock = document.createElement("pre");
        codeBlock.append(document.createElement("code"));
      }
      continue;
    }
    if (codeBlock) {
      codeBlock.firstChild.textContent += `${line}\n`;
      continue;
    }
    if (line.includes("|") && index + 1 < lines.length && isMarkdownTableSeparator(lines[index + 1])) {
      endList();
      const wrapper = document.createElement("div");
      wrapper.className = "markdown-table-wrap";
      const table = document.createElement("table");
      const head = document.createElement("thead");
      const headRow = document.createElement("tr");
      markdownTableCells(line).forEach((cell) => {
        const element = document.createElement("th");
        element.textContent = cell;
        headRow.append(element);
      });
      head.append(headRow);
      table.append(head);
      const tableBody = document.createElement("tbody");
      index += 2;
      while (index < lines.length && lines[index].includes("|") && lines[index].trim()) {
        const row = document.createElement("tr");
        markdownTableCells(lines[index]).forEach((cell) => {
          const element = document.createElement("td");
          element.textContent = cell;
          row.append(element);
        });
        tableBody.append(row);
        index += 1;
      }
      index -= 1;
      table.append(tableBody);
      wrapper.append(table);
      article.append(wrapper);
      continue;
    }
    const heading = line.match(/^(#{1,6})\s+(.+)$/);
    if (heading) {
      endList();
      const element = document.createElement(`h${heading[1].length}`);
      element.textContent = heading[2];
      article.append(element);
      continue;
    }
    const quote = line.match(/^>\s?(.*)$/);
    if (quote) {
      endList();
      const element = document.createElement("blockquote");
      element.textContent = quote[1];
      article.append(element);
      continue;
    }
    const bullet = line.match(/^[-*+]\s+(.+)$/);
    const ordered = line.match(/^\d+\.\s+(.+)$/);
    if (bullet || ordered) {
      const type = ordered ? "OL" : "UL";
      if (!list || list.tagName !== type) {
        list = document.createElement(type.toLowerCase());
        article.append(list);
      }
      const item = document.createElement("li");
      item.textContent = (bullet || ordered)[1];
      list.append(item);
      continue;
    }
    endList();
    if (/^([-*_])\1\1+$/.test(line.trim())) {
      article.append(document.createElement("hr"));
    } else if (line.trim()) {
      appendMarkdownParagraph(article, line);
    }
  }
  if (codeBlock) article.append(codeBlock);
  return article;
}

function destroyEpubReader() {
  const reader = state.epubReader;
  state.epubReader = null;
  if (!reader) return;
  try { reader.rendition?.destroy(); } catch (_) {}
  try { reader.book?.destroy(); } catch (_) {}
}

function appendEpubToc(options, items, depth = 0) {
  items.forEach((item) => {
    const option = document.createElement("option");
    option.value = item.href;
    option.textContent = `${"　".repeat(depth)}${item.label?.trim() || "未命名章节"}`;
    options.append(option);
    if (item.subitems?.length) appendEpubToc(options, item.subitems, depth + 1);
  });
}

async function openEpubReader(entry, source, body) {
  if (typeof window.ePub !== "function") throw new Error("EPUB 阅读组件未加载，请刷新页面后重试");

  const shell = document.createElement("div");
  shell.className = "epub-reader";
  shell.innerHTML = `
    <div class="epub-toolbar">
      <button type="button" class="icon-button epub-previous" title="上一页" aria-label="上一页"><i data-lucide="chevron-left"></i></button>
      <select class="epub-toc" aria-label="章节目录"><option value="">目录加载中…</option></select>
      <button type="button" class="command-button epub-spread" title="切换单栏或双栏"><i data-lucide="columns-2"></i><span>双栏</span></button>
      <span class="epub-progress">正在打开…</span>
      <button type="button" class="icon-button epub-next" title="下一页" aria-label="下一页"><i data-lucide="chevron-right"></i></button>
    </div>
    <div class="epub-viewer" tabindex="0"></div>`;
  body.append(shell);
  lucide.createIcons({ nodes: [shell] });

  const viewer = shell.querySelector(".epub-viewer");
  const toc = shell.querySelector(".epub-toc");
  const progress = shell.querySelector(".epub-progress");
  const spreadButton = shell.querySelector(".epub-spread");
  let useSpread = window.matchMedia("(min-width: 900px)").matches;
  let currentLocation = null;
  const positionKey = `remydesk.epub.position.v1:${entry.path}`;
  progress.textContent = "正在读取电子书…";
  const response = await fetch(source);
  if (!response.ok) throw new Error(`无法读取 EPUB：HTTP ${response.status}`);
  const bookData = await response.arrayBuffer();
  const book = window.ePub(bookData);
  const rendition = book.renderTo(viewer, {
    width: "100%",
    height: "100%",
    flow: "paginated",
    spread: useSpread ? "always" : "none",
    minSpreadWidth: 0,
  });
  state.epubReader = { book, rendition };

  rendition.themes.default({
    body: {
      "font-family": "system-ui, -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif !important",
      "line-height": "1.75 !important",
      "padding": "0 1.1rem !important",
    },
    img: { "max-width": "100% !important", "height": "auto !important" },
  });

  function updateSpreadButton() {
    spreadButton.classList.toggle("active", useSpread);
    spreadButton.querySelector("span").textContent = useSpread ? "双栏" : "单栏";
  }

  function updateProgress(location) {
    currentLocation = location;
    const displayed = location?.start?.displayed;
    let label = displayed?.total ? `本章 ${displayed.page}/${displayed.total}` : "阅读中";
    try {
      if (book.locations.length() && location?.start?.cfi) {
        const percent = Math.round(book.locations.percentageFromCfi(location.start.cfi) * 100);
        label += ` · ${Math.max(0, Math.min(100, percent))}%`;
      }
    } catch (_) {}
    progress.textContent = label;
    if (location?.start?.cfi) {
      try { localStorage.setItem(positionKey, location.start.cfi); } catch (_) {}
    }
  }

  rendition.on("relocated", updateProgress);
  rendition.on("keyup", (event) => {
    if (event.key === "ArrowLeft" || event.key === "PageUp") rendition.prev();
    if (event.key === "ArrowRight" || event.key === "PageDown" || event.key === " ") rendition.next();
  });
  shell.querySelector(".epub-previous").addEventListener("click", () => rendition.prev());
  shell.querySelector(".epub-next").addEventListener("click", () => rendition.next());
  spreadButton.addEventListener("click", async () => {
    useSpread = !useSpread;
    rendition.spread(useSpread ? "always" : "none", 0);
    updateSpreadButton();
    if (currentLocation?.start?.cfi) await rendition.display(currentLocation.start.cfi);
  });
  toc.addEventListener("change", () => {
    if (toc.value) rendition.display(toc.value);
  });
  shell.addEventListener("keydown", (event) => {
    if (event.key === "ArrowLeft" || event.key === "PageUp") rendition.prev();
    if (event.key === "ArrowRight" || event.key === "PageDown" || event.key === " ") rendition.next();
  });

  updateSpreadButton();
  const navigation = await book.loaded.navigation;
  toc.replaceChildren();
  const placeholder = document.createElement("option");
  placeholder.value = "";
  placeholder.textContent = "选择章节";
  toc.append(placeholder);
  appendEpubToc(toc, navigation.toc || []);

  let savedPosition = "";
  try { savedPosition = localStorage.getItem(positionKey) || ""; } catch (_) {}
  await rendition.display(savedPosition || undefined);
  book.locations.generate(1200).then(() => {
    if (currentLocation) updateProgress(currentLocation);
  }).catch(() => {});
}

async function openPreview(entry) {
  const dialog = $("#previewDialog");
  const body = $("#previewBody");
  const visual = fileVisual(entry);
  const extension = extensionOf(entry.name);
  const source = previewUrl(entry.path);
  destroyEpubReader();
  dialog.classList.toggle("epub-reader-open", extension === ".epub");
  body.replaceChildren();
  body.className = `preview-body preview-${visual.className}`;
  $("#previewTitle").textContent = entry.name;
  $("#previewMeta").textContent = `${visual.kind} · ${formatSize(entry.size)}`;
  $("#previewDownload").href = downloadUrl(entry.path);
  $("#previewDownload").download = entry.name;

  try {
    if ([".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".svg"].includes(extension)) {
      const image = document.createElement("img");
      image.className = "preview-media-image";
      image.src = source;
      image.alt = entry.name;
      body.append(image);
    } else if ([".mp4", ".m4v", ".webm", ".mov"].includes(extension)) {
      const video = document.createElement("video");
      video.className = "preview-video-player";
      video.src = source;
      video.controls = true;
      video.autoplay = true;
      video.playsInline = true;
      video.preload = "metadata";
      body.append(video);
    } else if ([".mp3", ".wav", ".ogg"].includes(extension)) {
      const audio = document.createElement("audio");
      audio.src = source;
      audio.controls = true;
      audio.autoplay = true;
      body.append(audio);
    } else if ([".txt", ".log", ".md", ".markdown"].includes(extension)) {
      if (entry.size > 5 * 1024 * 1024) throw new Error("文本文件超过 5 MB，请下载后查看");
      const response = await fetch(source);
      if (!response.ok) throw new Error(`无法读取文件：HTTP ${response.status}`);
      const text = await response.text();
      if ([".md", ".markdown"].includes(extension)) {
        body.append(renderMarkdown(text));
      } else {
        const pre = document.createElement("pre");
        pre.className = "text-preview";
        pre.textContent = text;
        body.append(pre);
      }
    } else if (extension === ".pdf") {
      const frame = document.createElement("iframe");
      frame.className = "preview-frame";
      frame.src = source;
      frame.title = entry.name;
      body.append(frame);
    } else if (extension === ".epub") {
      if (!dialog.open) dialog.showModal();
      await openEpubReader(entry, source, body);
    } else {
      const unsupported = document.createElement("div");
      unsupported.className = "preview-unsupported";
      unsupported.innerHTML = `<i data-lucide="${visual.icon}"></i><strong></strong><span>此格式暂不支持页面内预览，可以下载后打开。</span>`;
      unsupported.querySelector("strong").textContent = entry.name;
      body.append(unsupported);
    }
  } catch (error) {
    const message = document.createElement("div");
    message.className = "preview-error";
    message.textContent = error.message;
    body.append(message);
  }
  if (!dialog.open) dialog.showModal();
  lucide.createIcons();
}

async function loadNetwork() {
  try {
    const data = await api("/api/network/status");
    $("#networkBadge").textContent = data.current_ssid || (data.hotspot_active ? data.hotspot_ssid : "未连接");
    $("#currentSsid").textContent = data.current_ssid || "未连接";
    $("#hotspotState").textContent = data.hotspot_active ? `${data.hotspot_ssid} 已打开` : "关闭";
    $("#hotspotAddress").textContent = data.hotspot_ip ? `管理地址 ${data.hotspot_ip}:8010` : "";
    $("#autoHotspot").checked = data.auto_hotspot;
    $("#toggleHotspot span").textContent = data.hotspot_active ? "关闭热点" : "打开热点";
    $("#toggleHotspot").dataset.active = data.hotspot_active ? "1" : "0";
    const select = $("#ipv4Connection");
    const selected = select.value;
    select.replaceChildren(...data.connections.map((item) => {
      const option = document.createElement("option");
      option.value = item.uuid;
      option.textContent = `${item.name}${item.active ? "（当前）" : ""}`;
      return option;
    }));
    if (data.connections.some((item) => item.uuid === selected)) select.value = selected;
    else if (data.connections.length) {
      select.value = (data.connections.find((item) => item.active) || data.connections[0]).uuid;
    }
    if (select.value) await loadIpv4();
    return data;
  } catch (error) {
    toast(error.message);
    return null;
  }
}

async function scanWifi(deep) {
  $("#wifiList").innerHTML = `<div class="empty-state">正在扫描</div>`;
  try {
    const data = await api(`/api/network/scan?deep=${deep ? "1" : "0"}`);
    const list = $("#wifiList");
    list.replaceChildren();
    if (!data.networks.length) list.innerHTML = `<div class="empty-state">未发现网络</div>`;
    data.networks.forEach((network) => {
      const item = document.createElement("div");
      item.className = "wifi-item";
      item.innerHTML = `<span><strong></strong><br><small>${network.security || "开放网络"}${network.known ? " · 已保存" : ""}${network.in_use ? " · 当前连接" : ""}</small></span><span class="wifi-signal">${network.signal}%</span><button type="button" class="command-button wifi-connect-button"><span>${network.in_use ? "已连接" : "连接"}</span></button>`;
      item.querySelector("strong").textContent = network.ssid;
      const connectButton = item.querySelector(".wifi-connect-button");
      connectButton.disabled = network.in_use;
      connectButton.addEventListener("click", () => connectWifi(network, connectButton));
      list.append(item);
    });
  } catch (error) {
    $("#wifiList").innerHTML = "";
    toast(error.message);
  }
}

function setWifiConnectFeedback(message, type = "") {
  const status = $("#wifiConnectStatus");
  status.textContent = message;
  status.className = `wifi-connect-status${type ? ` ${type}` : ""}`;
}

function setWifiButtonsBusy(activeButton, busy) {
  $$(".wifi-connect-button").forEach((button) => {
    button.disabled = busy || button.textContent.trim() === "已连接";
  });
  if (activeButton) {
    activeButton.classList.toggle("connecting", busy);
    activeButton.querySelector("span").textContent = busy ? "连接中…" : "连接";
  }
}

async function connectWifi(network, activeButton = null) {
  state.selectedSsid = network.ssid;
  state.selectedWifiButton = activeButton;
  if (network.known || !network.security) {
    setWifiButtonsBusy(activeButton, true);
    setWifiConnectFeedback(`正在连接 ${network.ssid}，热点可能会暂时断开…`, "connecting");
    try {
      await jsonPost("/api/network/connect", { ssid: network.ssid, known: network.known });
      toast(`已提交连接：${network.ssid}`, 4500);
      pollConnection(network.ssid);
    } catch (error) {
      setWifiButtonsBusy(activeButton, false);
      setWifiConnectFeedback(`连接请求失败：${error.message}`, "error");
      toast(error.message);
    }
    return;
  }
  $("#passwordTitle").textContent = `连接 ${network.ssid}`;
  $("#wifiPassword").value = "";
  $("#passwordDialog").showModal();
}

async function pollConnection(ssid) {
  const stageText = {
    queued: "连接请求已接收",
    "leaving-hotspot": "正在关闭热点",
    associating: "正在连接路由器",
    "manual-recovery-window": "自动连接失败，可在设备网络菜单中手动连接",
    "restoring-hotspot": "正在恢复设置热点",
  };
  for (let count = 0; count < 75; count += 1) {
    await new Promise((resolve) => setTimeout(resolve, 1000));
    const data = await loadNetwork();
    if (!data) continue;
    if (data.connect_state.running) {
      const detail = stageText[data.connect_state.stage] || "正在处理网络切换";
      setWifiConnectFeedback(`${detail}：${ssid}…`, "connecting");
      continue;
    }
    if (data.current_ssid === ssid) {
      setWifiConnectFeedback(`已成功连接 ${ssid}。`, "success");
      toast(`已连接 ${ssid}`);
      await scanWifi(false);
    } else if (data.connect_state.error) {
      setWifiButtonsBusy(state.selectedWifiButton, false);
      setWifiConnectFeedback(`连接失败：${data.connect_state.error}`, "error");
      toast(data.connect_state.error);
    }
    return;
  }
  setWifiButtonsBusy(state.selectedWifiButton, false);
  setWifiConnectFeedback(`连接状态确认超时。若热点已断开，请切换到 ${ssid} 后访问设备的新 IP。`, "error");
}

async function loadIpv4() {
  const key = $("#ipv4Connection").value;
  if (!key) return;
  try {
    const data = await api(`/api/network/ipv4?connection=${encodeURIComponent(key)}`);
    setIpv4Mode(data.mode || "auto");
    const address = (data.address || "").split(",")[0].trim();
    const [ip, prefix] = address.split("/");
    $("#ipv4Address").value = ip || "";
    $("#ipv4Netmask").value = prefixToNetmask(prefix);
    $("#ipv4Gateway").value = data.gateway || "";
    $("#ipv4Dns").value = data.dns || "";
  } catch (error) {
    toast(error.message);
  }
}

function setIpv4Mode(mode) {
  state.ipv4Mode = mode;
  $$("#ipv4Mode button").forEach((button) => button.classList.toggle("active", button.dataset.mode === mode));
  $("#manualIpv4").hidden = mode !== "manual";
  $("#ipv4ModeHint").textContent = mode === "manual"
    ? "填写固定 IP 地址和子网掩码。网关与 DNS 可在高级设置中选填。"
    : "由路由器自动分配 IP 地址，无需填写其他内容。";
  $("#saveIpv4 span").textContent = mode === "manual" ? "应用静态地址" : "应用动态地址";
  $("#ipv4SaveStatus").textContent = "";
  $("#ipv4SaveStatus").className = "ipv4-save-status";
}

function prefixToNetmask(prefix) {
  const bits = Number(prefix);
  if (!Number.isInteger(bits) || bits < 0 || bits > 32) return "";
  return [0, 8, 16, 24].map((offset) => {
    const count = Math.max(0, Math.min(8, bits - offset));
    return count ? 256 - (2 ** (8 - count)) : 0;
  }).join(".");
}

function netmaskToPrefix(netmask) {
  const parts = netmask.trim().split(".");
  if (parts.length !== 4) throw new Error("请输入正确的子网掩码，例如 255.255.255.0");
  const binary = parts.map((part) => {
    const value = Number(part);
    if (!Number.isInteger(value) || value < 0 || value > 255) throw new Error("子网掩码格式不正确");
    return value.toString(2).padStart(8, "0");
  }).join("");
  if (!/^1*0*$/.test(binary)) throw new Error("子网掩码必须由连续的 1 和 0 组成");
  return binary.indexOf("0") === -1 ? 32 : binary.indexOf("0");
}

function validIpv4Address(value) {
  const parts = value.trim().split(".");
  return parts.length === 4 && parts.every((part) => /^\d{1,3}$/.test(part) && Number(part) <= 255);
}

async function loadDesktop({ remount = false } = {}) {
  try {
    state.desktop = await api("/api/desktop/status");
    $("#desktopDot").classList.toggle("active", state.desktop.running);
    $("#desktopPower span").textContent = state.desktop.running ? "停止" : "启动";
    $("#desktopStatusText").textContent = state.desktop.running ? "运行中" : "已停止";
    if (state.desktop.running) mountDesktop(remount);
  } catch (error) {
    toast(error.message);
  }
}

function mountDesktop(remount = false) {
  if (!state.desktop || !state.desktop.running) return;
  const stage = $("#desktopStage");
  if (remount) stage.replaceChildren();
  if (stage.querySelector("iframe")) return;
  $("#desktopStatusText").textContent = "正在连接…";
  const frame = document.createElement("iframe");
  frame.allow = "autoplay; fullscreen; clipboard-read; clipboard-write";
  frame.src = state.desktop.url;
  frame.title = "RemyDesk 局域网桌面";
  frame.addEventListener("load", () => { $("#desktopStatusText").textContent = "已连接"; });
  stage.replaceChildren(frame);
}

function closeDialog(event) {
  const dialog = event.target.closest("dialog");
  if (dialog.id === "previewDialog") {
    destroyEpubReader();
    dialog.classList.remove("epub-reader-open");
    setPreviewFullscreenLayout(false);
    updatePreviewFullscreenButton();
    $("#previewBody").replaceChildren();
  }
  dialog.close();
}

$("#refreshButton").addEventListener("click", () => {
  loadFiles();
  loadNetwork();
  loadDesktop({ remount: $("#desktopDialog").open });
});
$("#backButton").addEventListener("click", () => loadFiles(state.path.split("/").slice(0, -1).join("/")));
$("#mkdirButton").addEventListener("click", async () => {
  const name = prompt("文件夹名称");
  if (!name) return;
  try {
    await jsonPost("/api/mkdir", { path: state.path, name });
    await loadFiles();
  } catch (error) {
    toast(error.message);
  }
});
$("#uploadButton").addEventListener("click", () => $("#fileInput").click());
$("#fileInput").addEventListener("change", async (event) => {
  try {
    await uploadDroppedFiles(Array.from(event.target.files), state.path);
  } catch (error) {
    toast(error.message);
  }
  event.target.value = "";
});

$("#wifiButton").addEventListener("click", async () => {
  $("#wifiDialog").showModal();
  await loadNetwork();
  await scanWifi(false);
});
$("#scanWifi").addEventListener("click", () => scanWifi(false));
$("#deepScanWifi").addEventListener("click", () => scanWifi(true));
$("#autoHotspot").addEventListener("change", async (event) => {
  try {
    await jsonPost("/api/network/settings", { auto_hotspot: event.target.checked });
    toast(event.target.checked
      ? "已切换为开机启动热点"
      : "已关闭开机热点，下次开机将自动连接已保存的 Wi-Fi", 5000);
  } catch (error) {
    event.target.checked = !event.target.checked;
    toast(error.message);
  }
});
$("#toggleHotspot").addEventListener("click", async (event) => {
  try {
    await jsonPost(event.currentTarget.dataset.active === "1" ? "/api/network/hotspot/stop" : "/api/network/hotspot/start");
    await loadNetwork();
  } catch (error) {
    toast(error.message);
  }
});
$("#confirmWifi").addEventListener("click", async () => {
  const button = $("#confirmWifi");
  try {
    button.disabled = true;
    button.querySelector("span").textContent = "连接中…";
    setWifiButtonsBusy(state.selectedWifiButton, true);
    setWifiConnectFeedback(`正在连接 ${state.selectedSsid}，热点可能会暂时断开…`, "connecting");
    await jsonPost("/api/network/connect", {
      ssid: state.selectedSsid,
      password: $("#wifiPassword").value,
      known: false,
    });
    $("#passwordDialog").close();
    toast(`已提交连接：${state.selectedSsid}`, 4500);
    pollConnection(state.selectedSsid);
  } catch (error) {
    setWifiButtonsBusy(state.selectedWifiButton, false);
    setWifiConnectFeedback(`连接请求失败：${error.message}`, "error");
    toast(error.message);
  } finally {
    button.disabled = false;
    button.querySelector("span").textContent = "连接";
  }
});
$("#ipv4Connection").addEventListener("change", loadIpv4);
$$("#ipv4Mode button").forEach((button) => button.addEventListener("click", () => setIpv4Mode(button.dataset.mode)));
$("#saveIpv4").addEventListener("click", async () => {
  const button = $("#saveIpv4");
  const buttonLabel = button.querySelector("span");
  const status = $("#ipv4SaveStatus");
  const normalLabel = state.ipv4Mode === "manual" ? "应用静态地址" : "应用动态地址";
  let requestedIp = "";
  try {
    let address = "";
    if (state.ipv4Mode === "manual") {
      const ip = $("#ipv4Address").value.trim();
      if (!validIpv4Address(ip)) throw new Error("请输入正确的 IPv4 地址");
      requestedIp = ip;
      address = `${ip}/${netmaskToPrefix($("#ipv4Netmask").value)}`;
    }
    button.disabled = true;
    button.classList.add("loading");
    buttonLabel.textContent = "正在应用…";
    status.className = "ipv4-save-status applying";
    status.textContent = "正在保存设置并重新连接网络，请稍候…";
    toast("正在应用 IPv4 设置…", 6000);
    await new Promise((resolve) => requestAnimationFrame(() => resolve()));
    await jsonPost("/api/network/ipv4", {
      connection: $("#ipv4Connection").value,
      mode: state.ipv4Mode,
      address,
      gateway: $("#ipv4Gateway").value,
      dns: $("#ipv4Dns").value,
    });
    status.className = "ipv4-save-status success";
    if (state.ipv4Mode === "manual") {
      status.textContent = `静态地址已保存。若连接断开，请访问 http://${requestedIp}:8010`;
      toast("静态 IPv4 已保存，网络正在重新连接", 5000);
    } else {
      status.textContent = "动态地址已保存，正在从路由器重新获取 IP。";
      toast("动态 IPv4 已保存，网络正在重新连接", 5000);
    }
  } catch (error) {
    const connectionInterrupted = error instanceof TypeError || /fetch|network|failed/i.test(error.message || "");
    if (connectionInterrupted && requestedIp) {
      status.className = "ipv4-save-status warning";
      status.textContent = `旧地址连接已中断，设置可能已经生效。请访问 http://${requestedIp}:8010`;
      toast("网络正在切换，请使用新的静态 IP 重新访问", 6500);
    } else {
      status.className = "ipv4-save-status error";
      status.textContent = error.message;
      toast(error.message, 5000);
    }
  } finally {
    button.disabled = false;
    button.classList.remove("loading");
    buttonLabel.textContent = normalLabel;
  }
});

$("#noteButton").addEventListener("click", async () => {
  try {
    $("#noteText").value = (await api("/api/note")).text || "";
    $("#noteDialog").showModal();
  } catch (error) {
    toast(error.message);
  }
});
$("#copyNote").addEventListener("click", async () => {
  const button = $("#copyNote");
  const label = button.querySelector("span");
  const normalLabel = label.textContent;
  try {
    await copyText($("#noteText").value, $("#noteText"));
    label.textContent = "已复制";
    button.classList.add("success");
    toast("便签内容已复制到剪贴板");
    setTimeout(() => {
      label.textContent = normalLabel;
      button.classList.remove("success");
    }, 1800);
  } catch (error) {
    toast(error.message);
  }
});
$("#saveNote").addEventListener("click", async () => {
  try {
    await jsonPost("/api/note", { text: $("#noteText").value });
    toast("便签已保存");
  } catch (error) {
    toast(error.message);
  }
});

$("#desktopButton").addEventListener("click", async () => {
  $("#desktopDialog").showModal();
  await loadDesktop({ remount: true });
});
$("#desktopPower").addEventListener("click", async () => {
  try {
    state.desktop = await jsonPost("/api/desktop/switch", {
      enabled: !(state.desktop && state.desktop.running),
    });
    $("#desktopStage").innerHTML = state.desktop.running
      ? ""
      : `<div class="desktop-placeholder">桌面投屏默认关闭</div>`;
    await loadDesktop({ remount: true });
  } catch (error) {
    toast(error.message);
  }
});
function currentFullscreenElement() {
  return document.fullscreenElement || document.webkitFullscreenElement || null;
}

function setDesktopFullscreenLayout(active) {
  $("#desktopDialog").classList.toggle("desktop-local-fullscreen", active);
  document.documentElement.classList.toggle("desktop-fullscreen-active", active);
}

function updatePageFullscreenButton() {
  const button = $("#desktopFullscreen");
  const active = $("#desktopDialog").classList.contains("desktop-local-fullscreen");
  button.title = active ? "退出本地页面全屏" : "本地页面全屏";
  button.setAttribute("aria-label", button.title);
  button.innerHTML = `<i data-lucide="${active ? "minimize" : "maximize"}"></i>`;
  lucide.createIcons({ nodes: [button] });
}

$("#desktopFullscreen").addEventListener("click", async () => {
  const dialog = $("#desktopDialog");
  const stage = $("#desktopStage");
  try {
    if (dialog.classList.contains("desktop-local-fullscreen")) {
      const fullscreenElement = currentFullscreenElement();
      if (fullscreenElement && document.exitFullscreen) await document.exitFullscreen();
      else if (fullscreenElement && document.webkitExitFullscreen) await document.webkitExitFullscreen();
      setDesktopFullscreenLayout(false);
    } else {
      setDesktopFullscreenLayout(true);
      if (stage.requestFullscreen) await stage.requestFullscreen();
      else if (stage.webkitRequestFullscreen) await stage.webkitRequestFullscreen();
      else throw new Error("当前浏览器不支持页面全屏");
    }
    updatePageFullscreenButton();
  } catch (error) {
    setDesktopFullscreenLayout(false);
    updatePageFullscreenButton();
    toast(`本地页面全屏失败：${error.message}`);
  }
});
function handleDesktopFullscreenChange() {
  setDesktopFullscreenLayout(currentFullscreenElement() === $("#desktopStage"));
  updatePageFullscreenButton();
}
document.addEventListener("fullscreenchange", handleDesktopFullscreenChange);
document.addEventListener("webkitfullscreenchange", handleDesktopFullscreenChange);

function setPreviewFullscreenLayout(active) {
  $("#previewDialog").classList.toggle("preview-local-fullscreen", active);
  document.documentElement.classList.toggle("preview-fullscreen-active", active);
}

function updatePreviewFullscreenButton() {
  const button = $("#previewFullscreen");
  const active = $("#previewDialog").classList.contains("preview-local-fullscreen");
  button.title = active ? "退出预览内容全屏" : "预览内容全屏";
  button.setAttribute("aria-label", button.title);
  button.innerHTML = `<i data-lucide="${active ? "minimize" : "maximize"}"></i>`;
  lucide.createIcons({ nodes: [button] });
}

$("#previewFullscreen").addEventListener("click", async () => {
  const dialog = $("#previewDialog");
  const body = $("#previewBody");
  try {
    if (dialog.classList.contains("preview-local-fullscreen")) {
      const fullscreenElement = currentFullscreenElement();
      if (fullscreenElement && document.exitFullscreen) await document.exitFullscreen();
      else if (fullscreenElement && document.webkitExitFullscreen) await document.webkitExitFullscreen();
      setPreviewFullscreenLayout(false);
    } else {
      setPreviewFullscreenLayout(true);
      if (body.requestFullscreen) await body.requestFullscreen();
      else if (body.webkitRequestFullscreen) await body.webkitRequestFullscreen();
      else throw new Error("当前浏览器不支持页面全屏");
    }
    updatePreviewFullscreenButton();
  } catch (error) {
    setPreviewFullscreenLayout(false);
    updatePreviewFullscreenButton();
    toast(`预览内容全屏失败：${error.message}`);
  }
});

function handlePreviewFullscreenChange() {
  setPreviewFullscreenLayout(currentFullscreenElement() === $("#previewBody"));
  updatePreviewFullscreenButton();
}
document.addEventListener("fullscreenchange", handlePreviewFullscreenChange);
document.addEventListener("webkitfullscreenchange", handlePreviewFullscreenChange);
$("#shutdownButton").addEventListener("click", async () => {
  if (!confirm("确认关闭设备？")) return;
  try {
    await jsonPost("/api/system/shutdown");
    toast("设备正在关机");
  } catch (error) {
    toast(error.message);
  }
});
$$(".close-dialog").forEach((button) => button.addEventListener("click", closeDialog));

document.addEventListener("click", (event) => {
  if (!event.target.closest(".desktop-icon")) {
    $$(".desktop-icon.selected").forEach((item) => item.classList.remove("selected"));
    state.selectedIcon = "";
  }
});

lucide.createIcons();
attachDropTarget($("#fileSurface"), () => state.path);
attachParentOutsideDrop($("#fileSurface"));

async function initialize() {
  await loadDesktopLayout();
  await Promise.all([loadFiles(), loadNetwork(), loadDesktop()]);
}

initialize();
