// Osmium Pod web installer: file formats and the X1000 USB boot protocol.
//
// Everything here is a port of Rockbox code:
//   - USB boot:   utils/jztool/src/x1000.c, utils/jztool/src/usb.c
//   - UCL unpack: firmware/common/ucl_decompress.c (nrv2e)
// No DOM access, so the module also runs under node for testing.

// ---------------------------------------------------------------- tar

// Minimal ustar reader: returns a Map of file name -> Uint8Array.
export function readTar(buf) {
  const files = new Map();
  const dec = new TextDecoder();
  let off = 0;
  while (off + 512 <= buf.length) {
    const hdr = buf.subarray(off, off + 512);
    if (hdr.every((b) => b === 0)) break;
    const field = (start, len) => {
      const raw = hdr.subarray(start, start + len);
      const end = raw.indexOf(0);
      return dec.decode(end < 0 ? raw : raw.subarray(0, end));
    };
    const name = field(0, 100);
    const prefix = field(345, 155);
    const size = parseInt(field(124, 12).trim() || "0", 8);
    const type = String.fromCharCode(hdr[156] || 0x30);
    off += 512;
    if (type === "0" || type === "\0")
      files.set(prefix ? `${prefix}/${name}` : name, buf.slice(off, off + size));
    off += Math.ceil(size / 512) * 512;
  }
  return files;
}

// ---------------------------------------------------------------- UCL

function nrv2eDecompress(src, dst) {
  let bb = 0, ilen = 0, olen = 0, lastMOff = 1;
  const getbit = () => {
    bb = bb & 0x7f ? bb * 2 : src[ilen++] * 2 + 1;
    return (bb >> 8) & 1;
  };
  const check = (cond, what) => {
    if (cond) throw new Error(`UCL: ${what}`);
  };

  for (;;) {
    while (getbit()) {
      check(ilen >= src.length, "input overrun");
      check(olen >= dst.length, "output overrun");
      dst[olen++] = src[ilen++];
    }
    let mOff = 1, mLen;
    for (;;) {
      mOff = mOff * 2 + getbit();
      check(ilen >= src.length, "input overrun");
      check(mOff > 0xffffff + 3, "lookbehind overrun");
      if (getbit()) break;
      mOff = (mOff - 1) * 2 + getbit();
    }
    if (mOff === 2) {
      mOff = lastMOff;
      mLen = getbit();
    } else {
      check(ilen >= src.length, "input overrun");
      mOff = ((mOff - 3) * 256 + src[ilen++]) >>> 0;
      if (mOff === 0xffffffff) break;
      mLen = (~mOff) & 1;
      mOff >>>= 1;
      lastMOff = ++mOff;
    }
    if (mLen) mLen = 1 + getbit();
    else if (getbit()) mLen = 3 + getbit();
    else {
      mLen++;
      do {
        mLen = mLen * 2 + getbit();
        check(ilen >= src.length, "input overrun");
        check(mLen >= dst.length, "output overrun");
      } while (!getbit());
      mLen += 3;
    }
    mLen += mOff > 0x500 ? 1 : 0;
    check(olen + mLen > dst.length, "output overrun");
    check(mOff > olen, "lookbehind overrun");
    let mPos = olen - mOff;
    dst[olen++] = dst[mPos++];
    do dst[olen++] = dst[mPos++]; while (--mLen > 0);
  }
  check(ilen !== src.length, "input not consumed");
  return olen;
}

const be32 = (d, o) => ((d[o] << 24) | (d[o + 1] << 16) | (d[o + 2] << 8) | d[o + 3]) >>> 0;

// Unpack a file made by "uclpack --nrv2e"
export function uclUnpack(src) {
  const magic = [0x00, 0xe9, 0x55, 0x43, 0x4c, 0xff, 0x01, 0x1a];
  if (src.length < 18 || magic.some((b, i) => src[i] !== b))
    throw new Error("UCL: bad magic");
  if (src[12] !== 0x2e) throw new Error("UCL: unsupported method");
  const blockSize = be32(src, 14);
  if (blockSize < 1024 || blockSize > 8 * 1024 * 1024)
    throw new Error("UCL: bad block size");

  const chunks = [];
  let off = 18, total = 0;
  for (;;) {
    if (src.length - off < 4) throw new Error("UCL: truncated");
    const outLen = be32(src, off); off += 4;
    if (outLen === 0) break;
    if (src.length - off < 4) throw new Error("UCL: truncated");
    const inLen = be32(src, off); off += 4;
    if (inLen > blockSize || outLen > blockSize || inLen === 0 || inLen > outLen)
      throw new Error("UCL: corrupted");
    if (src.length - off < inLen) throw new Error("UCL: truncated");

    const block = src.subarray(off, off + inLen);
    let out;
    if (inLen < outLen) {
      out = new Uint8Array(outLen);
      if (nrv2eDecompress(block, out) !== outLen) throw new Error("UCL: bad length");
    } else {
      out = block;
    }
    chunks.push(out);
    total += outLen;
    off += inLen;
  }

  const dst = new Uint8Array(total);
  let pos = 0;
  for (const c of chunks) { dst.set(c, pos); pos += c.length; }
  return dst;
}

// ---------------------------------------------------------------- bootloader file

const X1000_TCSM_BASE = 0xf4000000;
export const X1000_SPL_LOAD_ADDR = X1000_TCSM_BASE + 0x1000;
export const X1000_SPL_EXEC_ADDR = X1000_TCSM_BASE + 0x1800;
export const X1000_STANDARD_DRAM_BASE = 0x80004000;

// Rockbox binary header lookup (search_header() in x1000.c)
export function searchHeader(src, label) {
  const HDR_BEGIN = 128, HDR_LEN = 256;
  const enc = new TextEncoder();
  const eq = (i, s) => enc.encode(s).every((b, k) => src[i + k] === b);
  let searchLen = Math.min(src.length, HDR_BEGIN);
  if (searchLen < 8) return 0;
  searchLen -= 7;
  let i;
  for (i = 8; i < searchLen; i += 4) if (eq(i, "BEGINHDR")) break;
  if (i >= searchLen) return 0;
  i += 8;
  searchLen = Math.min(src.length, i + HDR_LEN) - 7;
  for (; i < searchLen; i += 8) {
    if (eq(i, "ENDH")) break;
    if (eq(i, label)) {
      i += 4;
      return (src[i] | (src[i + 1] << 8) | (src[i + 2] << 16) | (src[i + 3] << 24)) >>> 0;
    }
  }
  return 0;
}

// Parse a bootloader.m3k archive into what the USB boot needs
export function parseBootloader(buf, ext = "m3k") {
  const files = readTar(buf);
  const spl = files.get(`spl.${ext}`);
  if (!spl) throw new Error(`spl.${ext} missing: not a bootloader for this player`);
  const ucl = files.get("bootloader2.ucl") || files.get("bootloader.ucl");
  if (!ucl) throw new Error("no bootloader binary in the archive");
  const info = files.get("bootloader-info.txt");
  const version = info ? new TextDecoder().decode(info).split("\n")[0].trim() : "unknown";
  const bootloader = uclUnpack(ucl);
  const loadAddr = searchHeader(bootloader, "LOAD") || X1000_STANDARD_DRAM_BASE;
  return { spl, bootloader, loadAddr, version };
}

// ---------------------------------------------------------------- USB boot

export const USB_FILTER = { vendorId: 0xa108, productId: 0x1000 };

const VR_GET_CPU_INFO = 0;
const VR_SET_DATA_ADDRESS = 1;
const VR_SET_DATA_LENGTH = 2;
const VR_FLUSH_CACHES = 3;
const VR_PROGRAM_START1 = 4;
const VR_PROGRAM_START2 = 5;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function setup(request, arg) {
  return {
    requestType: "vendor",
    recipient: "device",
    request,
    value: (arg >>> 16) & 0xffff,
    index: arg & 0xffff,
  };
}

async function vendorOut(dev, request, arg) {
  const r = await dev.controlTransferOut(setup(request, arg));
  if (r.status !== "ok") throw new Error(`USB request ${request} failed (${r.status})`);
}

async function send(dev, addr, data) {
  await vendorOut(dev, VR_SET_DATA_ADDRESS, addr);
  await vendorOut(dev, VR_SET_DATA_LENGTH, data.length);
  const r = await dev.transferOut(1, data);
  if (r.status !== "ok" || r.bytesWritten !== data.length)
    throw new Error("USB bulk transfer failed");
}

export async function getCpuInfo(dev) {
  const r = await dev.controlTransferIn(setup(VR_GET_CPU_INFO, 0), 8);
  if (r.status !== "ok") throw new Error("cannot read the CPU info");
  return new TextDecoder().decode(r.data).replace(/\0.*$/s, "");
}

// Load the bootloader into RAM and start it (jz_x1000_boot())
export async function usbBoot(dev, boot, log = () => {}) {
  await dev.open();
  if (dev.configuration === null) await dev.selectConfiguration(1);
  await dev.claimInterface(0);

  const cpu = await getCpuInfo(dev);
  log(`CPU: ${cpu}`);
  if (!cpu.startsWith("X1000"))
    throw new Error(`this is not an X1000 player (${cpu})`);

  log(`Stage 1: SPL, ${boot.spl.length} bytes`);
  await send(dev, X1000_SPL_LOAD_ADDR, boot.spl);
  await vendorOut(dev, VR_PROGRAM_START1, X1000_SPL_EXEC_ADDR);

  // the SPL needs a moment to set up the clocks and the DRAM
  await sleep(500);

  log(`Stage 2: bootloader, ${boot.bootloader.length} bytes at 0x${boot.loadAddr.toString(16)}`);
  await send(dev, boot.loadAddr, boot.bootloader);
  await vendorOut(dev, VR_FLUSH_CACHES, 0);
  await vendorOut(dev, VR_PROGRAM_START2, boot.loadAddr);

  // the player now runs the bootloader and leaves USB boot mode
  try { await dev.close(); } catch { /* already gone */ }
}

// ---------------------------------------------------------------- zip

// Minimal zip reader (stored and deflated entries) using the central
// directory. Returns [{name, size, data: () => Promise<Uint8Array>}].
export function readZip(buf) {
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  let eocd = -1;
  for (let i = buf.length - 22; i >= Math.max(0, buf.length - 65557); i--)
    if (dv.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
  if (eocd < 0) throw new Error("not a zip file");

  const count = dv.getUint16(eocd + 10, true);
  let p = dv.getUint32(eocd + 16, true);
  const dec = new TextDecoder();
  const entries = [];
  for (let n = 0; n < count; n++) {
    if (dv.getUint32(p, true) !== 0x02014b50) throw new Error("zip: bad directory");
    const method = dv.getUint16(p + 10, true);
    const csize = dv.getUint32(p + 20, true);
    const size = dv.getUint32(p + 24, true);
    const nlen = dv.getUint16(p + 28, true);
    const xlen = dv.getUint16(p + 30, true);
    const clen = dv.getUint16(p + 32, true);
    const local = dv.getUint32(p + 42, true);
    const name = dec.decode(buf.subarray(p + 46, p + 46 + nlen));
    p += 46 + nlen + xlen + clen;
    if (name.endsWith("/")) continue;
    if (method !== 0 && method !== 8) throw new Error(`zip: unsupported method for ${name}`);

    entries.push({
      name,
      size,
      async data() {
        const lnlen = dv.getUint16(local + 26, true);
        const lxlen = dv.getUint16(local + 28, true);
        const start = local + 30 + lnlen + lxlen;
        const raw = buf.subarray(start, start + csize);
        if (method === 0) return raw;
        const stream = new Blob([raw]).stream().pipeThrough(new DecompressionStream("deflate-raw"));
        const out = new Uint8Array(await new Response(stream).arrayBuffer());
        if (out.length !== size) throw new Error(`zip: bad size for ${name}`);
        return out;
      },
    });
  }
  return entries;
}
