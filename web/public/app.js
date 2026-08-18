// Assemble a wrapped OpenFX plugin in the browser.
//
// This is the whole of the generator's job now that the shell describes itself
// (see the repo's docs/05-any-to-any.md): copy a prebuilt shell binary, copy
// the guest bundle beside it, write an Info.plist. The shell works out what it
// is carrying when the host loads it — which is why no native code has to run
// here, and nothing is uploaded anywhere.
//
// Deliberately no build step and no framework: a page whose entire argument is
// "you don't have to install anything" should be readable in one file.

/**
 * Which shell binary to serve, from the visitor's own platform.
 *
 * The wrapped plugin runs on THEIR machine, so this has to match the host they
 * will load it in — not the machine that built the page. `platform` is the
 * modern replacement for the userAgent sniffing this would otherwise need.
 *
 * `arch` is the subdirectory of Contents/ the binary must sit in, and it is not
 * cosmetic: an OFX host looks in exactly one of these and nowhere else. Only
 * macOS hosts fall back to Contents/MacOS, so a Windows or Linux bundle laid
 * out the macOS way is not "probably fine" — it is invisible.
 */
const SHELLS = {
  macos: {
    url: './shells/ffglofxshell-macos-universal.ofx',
    label: 'macOS (Apple Silicon + Intel)',
    ext: '.ofx.bundle',
    arch: 'MacOS',
    tested: true,
  },
  windows: {
    url: './shells/ffglofxshell-windows-x86_64.ofx',
    label: 'Windows x64',
    ext: '.ofx.bundle',
    arch: 'Win64',
    tested: false,
  },
  linux: {
    url: './shells/ffglofxshell-linux-x86_64.ofx',
    label: 'Linux x64',
    ext: '.ofx.bundle',
    arch: 'Linux-x86-64',
    tested: false,
    noAe: true,
  },
};

function detectPlatform() {
  const p = (navigator.userAgentData?.platform || navigator.platform || '').toLowerCase();
  if (p.includes('win')) return 'windows';
  if (p.includes('linux') || p.includes('android')) return 'linux';
  return 'macos';
}

const PLATFORM = detectPlatform();
const SHELL = SHELLS[PLATFORM];
const SHELL_URL = SHELL.url;

const el = (id) => document.getElementById(id);
const log = (msg, cls = '') => {
  const d = document.createElement('div');
  d.textContent = msg;
  if (cls) d.className = cls;
  el('log').appendChild(d);
};

let guestHandle = null;   // FileSystemDirectoryHandle for the .bundle/.plugin
let guestName = '';       // e.g. "Tinsel.bundle"
let guestKind = 'ffgl';   // 'ffgl' | 'ae'
let outHandle = null;

// State the platform up front. A visitor on Windows should not have to guess
// whether the thing they just built is for their machine.
{
  const note = document.getElementById('platformNote');
  if (note) {
    note.innerHTML = SHELL.tested
      ? `Building for <b>${SHELL.label}</b>.`
      : `Building for <b>${SHELL.label}</b> — <span class="warnInline">this shell compiles but ` +
        `has never been run by its author</span>, so treat it as untested.` +
        (SHELL.noAe ? ' It carries FFGL plugins only: After Effects has no Linux version.' : '');
  }
}

if (!('showDirectoryPicker' in window)) {
  el('unsupported').hidden = false;
  el('unsupported').classList.add('unsupported');
  for (const id of ['pickGuest', 'pickOut', 'build']) el(id).disabled = true;
}

/** Read a text file out of a directory handle, or '' if it isn't there. */
async function readText(dir, ...path) {
  try {
    let h = dir;
    for (const part of path.slice(0, -1)) h = await h.getDirectoryHandle(part);
    const file = await (await h.getFileHandle(path[path.length - 1])).getFile();
    return await file.text();
  } catch {
    return '';
  }
}

/**
 * Which plugin format this is, from static data only.
 *
 * An After Effects plugin declares CFBundlePackageType eFKT in its own
 * Info.plist. That is the same discriminator the shell itself uses at load
 * time, so the page and the plugin cannot disagree about what was wrapped.
 */
export async function sniffKind(dir) {
  const plist = await readText(dir, 'Contents', 'Info.plist');
  return plist.includes('eFKT') ? 'ae' : 'ffgl';
}

/** Copy a directory tree from one handle to another, recursively. */
async function copyTree(src, dstParent, name, onFile) {
  const dst = await dstParent.getDirectoryHandle(name, { create: true });
  for await (const [childName, handle] of src.entries()) {
    if (handle.kind === 'directory') {
      await copyTree(handle, dst, childName, onFile);
    } else {
      const file = await handle.getFile();
      const out = await dst.getFileHandle(childName, { create: true });
      const w = await out.createWritable();
      await w.write(await file.arrayBuffer());
      await w.close();
      if (onFile) onFile(file.size);
    }
  }
  return dst;
}

async function writeFile(dir, name, contents) {
  const h = await dir.getFileHandle(name, { create: true });
  const w = await h.createWritable();
  await w.write(contents);
  await w.close();
}

el('pickGuest').addEventListener('click', async () => {
  try {
    const dir = await window.showDirectoryPicker({ id: 'guest', mode: 'read' });
    // The picker hands back the folder itself, so its name is the bundle name.
    if (!/\.(bundle|plugin|ofx\.bundle)$/i.test(dir.name)) {
      el('guestInfo').innerHTML =
        `<b>${dir.name}</b> doesn't look like a plugin bundle — expected a folder ` +
        `ending .bundle or .plugin.`;
      return;
    }
    guestHandle = dir;
    guestName = dir.name;
    guestKind = await sniffKind(dir);

    const label = guestKind === 'ae' ? 'After Effects plugin' : 'FFGL plugin';
    el('guestInfo').innerHTML =
      `<b>${guestName}</b><span class="badge">${label}</span>`;
    el('step1row').classList.add('done');
    el('pickOut').disabled = false;
  } catch (e) {
    if (e.name !== 'AbortError') el('guestInfo').textContent = `Couldn't read that: ${e.message}`;
  }
});

el('pickOut').addEventListener('click', async () => {
  try {
    outHandle = await window.showDirectoryPicker({ id: 'out', mode: 'readwrite' });
    el('outInfo').innerHTML = `Writing into <b>${outHandle.name}</b>`;
    el('step2row').classList.add('done');
    el('build').disabled = false;
  } catch (e) {
    if (e.name !== 'AbortError') el('outInfo').textContent = `Couldn't use that folder: ${e.message}`;
  }
});

/**
 * Assemble the wrapped bundle. Split out from the button handler so it can be
 * driven against any FileSystemDirectoryHandle — the test in web/test/ runs it
 * against the origin-private filesystem, which is the same interface the
 * picker returns.
 */
export async function assembleBundle(guest, name, kind, outDir, onLog = () => {}, arch = SHELL.arch) {
  const stem = name.replace(/\.(ofx\.bundle|bundle|plugin)$/i, '');
  const safe = stem.replace(/[^A-Za-z0-9]+/g, '_');
  const suffix = kind === 'ae' ? '_AE' : '_FFGL';
  const bundleName = `${safe}${suffix}.ofx.bundle`;
  const binaryName = `${safe}${suffix}.ofx`;
  const identifier =
    `com.stoatworks.${kind === 'ae' ? 'aewrap' : 'ffglwrap'}.` + safe.toLowerCase();

  onLog('Fetching the shell…');
  const shell = await (await fetch(SHELL_URL)).arrayBuffer();

  const bundle = await outDir.getDirectoryHandle(bundleName, { create: true });
  const contents = await bundle.getDirectoryHandle('Contents', { create: true });
  // Named for the shell's platform, not for the one that wrote the page. See
  // SHELLS above for why getting this wrong makes the bundle invisible rather
  // than broken.
  const archDir = await contents.getDirectoryHandle(arch, { create: true });
  const guestDir = await contents.getDirectoryHandle('Guest', { create: true });

  await writeFile(archDir, binaryName, shell);
  onLog(`Shell written (${(shell.byteLength / 1048576).toFixed(1)} MB)`, 'ok');

  let bytes = 0;
  await copyTree(guest, guestDir, name, (n) => { bytes += n; });
  onLog(`Guest copied (${(bytes / 1048576).toFixed(1)} MB)`, 'ok');

  await writeFile(contents, 'Info.plist',
    `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
\t<key>CFBundleExecutable</key>\t<string>${binaryName}</string>
\t<key>CFBundleIdentifier</key>\t<string>${identifier}</string>
\t<key>CFBundlePackageType</key>\t<string>BNDL</string>
</dict>
</plist>
`);
  onLog('Info.plist written', 'ok');

  // No manifest is written. The shell reads the guest's own Info.plist to tell
  // an After Effects plugin from an FFGL one, and asks it for its parameters
  // when the host loads it.
  onLog(`${bundleName} is ready`, 'ok');
  return bundleName;
}

el('build').addEventListener('click', async () => {
  el('build').disabled = true;
  el('log').innerHTML = '';
  el('after').hidden = true;

  try {
    const bundleName = await assembleBundle(guestHandle, guestName, guestKind, outHandle, log);
    const stem = guestName.replace(/\.(ofx\.bundle|bundle|plugin)$/i, '');
    // No path is available from the File System Access API — only the folder's
    // name — so the command shows that and invites the drag, which is how a
    // person gets a real path into Terminal anyway.
    // Quarantine is a macOS idea; the other two just need the bundle moved.
    el('installCmd').textContent =
      PLATFORM === 'macos'
        ? `# tip: type the command, then drag ${bundleName} from Finder onto the window\n` +
          `xattr -dr com.apple.quarantine <${outHandle.name}>/${bundleName}\n` +
          `sudo cp -R <${outHandle.name}>/${bundleName} /Library/OFX/Plugins/`
        : PLATFORM === 'windows'
        ? `move "<${outHandle.name}>\\${bundleName}" ^\n` +
          `  "C:\\Program Files\\Common Files\\OFX\\Plugins\\${bundleName}"`
        : `sudo cp -R <${outHandle.name}>/${bundleName} /usr/OFX/Plugins/`;
    el('after').hidden = false;
    el('step3row').classList.add('done');
  } catch (e) {
    log(`Failed: ${e.message}`, 'err');
  } finally {
    el('build').disabled = false;
  }
});
