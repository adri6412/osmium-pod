// Osmium Pod web installer: page logic. Formats and USB live in lib.js.

import { parseBootloader, readZip, usbBoot, USB_FILTER } from "./lib.js";

const $ = (id) => document.getElementById(id);

const hasUsb = "usb" in navigator;
const hasFolders = "showDirectoryPicker" in window;

let manifest = null;
const cache = new Map();

async function fetchBytes(url) {
  if (!cache.has(url)) {
    cache.set(url, fetch(url).then(async (r) => {
      if (!r.ok) throw new Error(`cannot download ${url} (${r.status})`);
      return new Uint8Array(await r.arrayBuffer());
    }));
  }
  try {
    return await cache.get(url);
  } catch (e) {
    cache.delete(url);
    throw e;
  }
}

function setStatus(id, text, kind = "") {
  const el = $(id);
  el.textContent = text;
  el.className = `status ${kind}`;
}

// ---------------------------------------------------------------- release

async function loadManifest() {
  try {
    const r = await fetch("firmware/manifest.json", { cache: "no-cache" });
    if (!r.ok) throw new Error();
    manifest = await r.json();
  } catch {
    manifest = null;
  }

  if (!manifest?.package || !manifest?.bootloader) {
    $("release").textContent =
      "No release with the web installer has been published yet.";
    return;
  }

  const date = manifest.published ? ` · ${manifest.published.slice(0, 10)}` : "";
  $("release").innerHTML = "";
  const a = document.createElement("a");
  a.href = manifest.url || "#";
  a.textContent = `Release ${manifest.version}`;
  $("release").append(a, date);

  for (const id of ["zip-link", "zip-link-top"]) $(id).href = manifest.package;
  $("sd-button").disabled = !hasFolders;
  $("usb-button").disabled = !hasUsb;
}

// ---------------------------------------------------------------- step 1

async function writeFile(root, path, data) {
  const parts = path.split("/");
  const name = parts.pop();
  let dir = root;
  for (const p of parts) dir = await dir.getDirectoryHandle(p, { create: true });
  const fh = await dir.getFileHandle(name, { create: true });
  const w = await fh.createWritable();
  await w.write(data);
  await w.close();
}

async function prepareCard() {
  let root;
  try {
    root = await window.showDirectoryPicker({ id: "osmium-sd", mode: "readwrite" });
  } catch {
    return; /* cancelled */
  }

  const ok = confirm(
    `Copy Osmium Pod ${manifest.version} to "${root.name}"?\n\n` +
    "This must be the root of the player's microSD card. Files in .rockbox " +
    "are overwritten; settings, database and music are kept.");
  if (!ok) return;

  const btn = $("sd-button");
  const bar = $("sd-progress");
  btn.disabled = true;
  bar.hidden = false;
  bar.value = 0;
  try {
    setStatus("sd-status", "Downloading the release…");
    const [zip, bl] = await Promise.all([
      fetchBytes(manifest.package),
      fetchBytes(manifest.bootloader),
    ]);
    parseBootloader(bl); /* refuse a broken download before writing anything */

    const entries = readZip(zip);
    const total = entries.reduce((n, e) => n + e.size, 0) + bl.length;
    let done = 0;
    for (const e of entries) {
      setStatus("sd-status", `Writing ${e.name}`);
      await writeFile(root, e.name, await e.data());
      done += e.size;
      bar.value = done / total;
    }
    await writeFile(root, "bootloader.m3k", bl);
    bar.value = 1;
    setStatus("sd-status",
      `Done: ${entries.length + 1} files written. Eject the card safely.`, "ok");
  } catch (e) {
    setStatus("sd-status", `Failed: ${e.message}`, "error");
  } finally {
    btn.disabled = false;
  }
}

// ---------------------------------------------------------------- step 2

async function bootPlayer() {
  const log = $("usb-log");
  const say = (line) => {
    log.hidden = false;
    log.textContent += `${line}\n`;
  };
  log.textContent = "";

  let dev;
  try {
    dev = await navigator.usb.requestDevice({ filters: [USB_FILTER] });
  } catch {
    return; /* no device picked */
  }

  const btn = $("usb-button");
  btn.disabled = true;
  try {
    const file = $("bl-file").files[0];
    const data = file
      ? new Uint8Array(await file.arrayBuffer())
      : await fetchBytes(manifest.bootloader);
    const boot = parseBootloader(data);
    say(`Bootloader ${boot.version}${file ? ` (${file.name})` : ""}`);

    await usbBoot(dev, boot, say);
    say("Started.");
    setStatus("usb-status",
      "The recovery menu should now be on the player's screen. Go on with step 3.", "ok");
  } catch (e) {
    say(`Error: ${e.message}`);
    setStatus("usb-status",
      "Loading failed. Unplug the player, enter USB boot mode again and retry.", "error");
    try { await dev.close(); } catch { /* ignore */ }
  } finally {
    btn.disabled = false;
  }
}

// ---------------------------------------------------------------- step 3

function setupChecklist() {
  for (const box of document.querySelectorAll("[data-check]")) {
    const key = `osmium-check-${box.dataset.check}`;
    try { box.checked = localStorage.getItem(key) === "1"; } catch { /* no storage */ }
    box.addEventListener("change", () => {
      try { localStorage.setItem(key, box.checked ? "1" : "0"); } catch { /* ignore */ }
    });
  }
}

// ---------------------------------------------------------------- init

$("browser-warning").hidden = hasUsb && hasFolders;
$("sd-button").addEventListener("click", prepareCard);
$("usb-button").addEventListener("click", bootPlayer);
setupChecklist();
loadManifest();
