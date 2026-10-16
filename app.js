// Osmium Pod web installer: page logic. Formats and USB live in lib.js.

import {
  buildPackage, parseBootloader, readZip, usbBoot,
  PACKAGE_ALLOW_NO_BACKUP, USB_FILTER,
} from "./lib.js?v=d9e7ae50b3";

const $ = (id) => document.getElementById(id);

const hasUsb = "usb" in navigator;
const hasFolders = "showDirectoryPicker" in window;

let manifest = null;
const cache = new Map();

async function fetchBytes(url) {
  if (!cache.has(url)) {
    // the files keep their names from one release to the next and Pages
    // lets browsers keep them for 10 minutes: always ask the server, or a
    // fresh release would install the previous one
    cache.set(url, fetch(url, { cache: "no-cache" }).then(async (r) => {
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

function logger(id) {
  const log = $(id);
  log.textContent = "";
  return (line) => {
    log.hidden = false;
    log.textContent += `${line}\n`;
  };
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

  for (const id of ["zip-link", "zip-link-top", "zip-link-restore"])
    $(id).href = manifest.package;
  $("install-button").disabled = !hasUsb || !manifest.installer;
  $("rescue-button").disabled = !hasUsb;
  $("sd-button").disabled = !hasFolders;
  if (!manifest.installer)
    setStatus("install-status",
      "This release has no installer: use the other ways below.", "error");
}

async function pickDevice() {
  try {
    return await navigator.usb.requestDevice({ filters: [USB_FILTER] });
  } catch {
    return null; /* no device picked */
  }
}

// ---------------------------------------------------------------- install

async function installPlayer() {
  const noBackup = $("no-backup").checked;
  const ok = confirm(
    `Install Osmium Pod ${manifest.version} on the player?\n\n` +
    "The FiiO firmware is replaced. " +
    (noBackup ? "No backup of it will be kept."
              : "If it is still there, a backup goes to the card in the player."));
  if (!ok) return;

  const dev = await pickDevice();
  if (!dev) return;

  const say = logger("install-log");
  const btn = $("install-button");
  const bar = $("install-progress");
  btn.disabled = true;
  bar.hidden = false;
  bar.value = 0;
  try {
    setStatus("install-status", "Downloading the release…");
    const [zip, bl, inst] = await Promise.all([
      fetchBytes(manifest.package),
      fetchBytes(manifest.bootloader),
      fetchBytes(manifest.installer),
    ]);
    say(`Bootloader ${parseBootloader(bl).version}`); /* refuses a broken download */
    const installer = parseBootloader(inst);
    say(`Installer ${installer.version}`);

    setStatus("install-status", "Preparing the package…");
    const files = [];
    for (const e of readZip(zip))
      if (e.name.startsWith(".rockbox/"))
        files.push({ name: e.name, data: await e.data() });
    const pkg = buildPackage(bl, files, noBackup ? PACKAGE_ALLOW_NO_BACKUP : 0);
    say(`${files.length} files`);

    setStatus("install-status", "Sending to the player…");
    await usbBoot(dev, installer, say, pkg, (f) => { bar.value = f; });
    say("Started.");
    setStatus("install-status",
      "The player is installing: follow its screen. It restarts by itself " +
      "when it is done, then you can unplug it.", "ok");
  } catch (e) {
    say(`Error: ${e.message}`);
    setStatus("install-status",
      "Sending failed. Unplug the player, enter USB boot mode again and retry.", "error");
    try { await dev.close(); } catch { /* ignore */ }
  } finally {
    btn.disabled = false;
  }
}

// ---------------------------------------------------------------- card

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

// ---------------------------------------------------------------- rescue

async function rescueBoot() {
  const dev = await pickDevice();
  if (!dev) return;

  const say = logger("rescue-log");
  const btn = $("rescue-button");
  btn.disabled = true;
  try {
    // with no package, the installer is the bootloader with the full
    // recovery menu (the one in flash keeps only the essentials)
    const file = $("bl-file").files[0];
    const data = file
      ? new Uint8Array(await file.arrayBuffer())
      : await fetchBytes(manifest.installer || manifest.bootloader);
    const boot = parseBootloader(data);
    say(`Bootloader ${boot.version}${file ? ` (${file.name})` : ""}`);

    await usbBoot(dev, boot, say);
    say("Started.");
    setStatus("rescue-status", "The recovery menu should now be on the player's screen.", "ok");
  } catch (e) {
    say(`Error: ${e.message}`);
    setStatus("rescue-status",
      "Loading failed. Unplug the player, enter USB boot mode again and retry.", "error");
    try { await dev.close(); } catch { /* ignore */ }
  } finally {
    btn.disabled = false;
  }
}

// ---------------------------------------------------------------- init

$("browser-warning").hidden = hasUsb;
$("install-button").addEventListener("click", installPlayer);
$("sd-button").addEventListener("click", prepareCard);
$("rescue-button").addEventListener("click", rescueBoot);
loadManifest();
