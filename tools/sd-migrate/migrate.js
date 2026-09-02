/* Casper Data Migration Tool — same BookPathId contract as firmware (lib/FsHelpers/BookPathId.*) */
(function () {
  "use strict";

  const MARKER = "crosspoint_migrate_v2.done";
  const CFG = [
    "settings.json", "wifi.json", "recent.json", "state.json", "opds.json", "koreader.json",
    "button_map.txt", "global_stats.bin", "global_stats.bin.bak", "sleep_frame.bin",
    "koreader_profiles.json", "achievements.json", "reading_stats.json", "reading_stats.json.bak",
    "crossink-settings.json",
  ];
  const BOOK_EXT = /\.(epub|xtc|xtch|txt|md)$/i;
  const SKIP_TOP = new Set([
    ".casper", "casper", ".crosspoint", "crosspoint", ".metadata", "metadata",
    ".system", "system volume information", ".dictionaries", ".fonts", ".sleep",
    ".casper-logs", ".casper-stats-backup",
  ]);

  if (typeof showDirectoryPicker !== "function") {
    document.body.classList.add("no-fsapi");
  }

  let root = null;
  let dst = null;
  let cancel = false;
  let plan = null;

  const $ = (id) => document.getElementById(id);
  const logEl = $("log");

  function log(m, c) {
    const d = document.createElement("div");
    if (c) d.className = c;
    d.textContent = m;
    logEl.appendChild(d);
    logEl.scrollTop = logEl.scrollHeight;
  }
  function clearLog() {
    logEl.textContent = "";
  }
  function setBar(p, t) {
    $("bar").style.width = Math.max(0, Math.min(100, p)) + "%";
    $("prog").textContent = t || "";
  }
  function esc(s) {
    return String(s)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;");
  }

  // --- BookPathId (must match firmware) ---
  function normalizePath(path) {
    if (!path) return "";
    let s = String(path).replace(/\\/g, "/");
    if (s.length >= 2 && /[A-Za-z]/.test(s[0]) && s[1] === ":") s = s.slice(2);
    while (s.startsWith("//")) s = s.slice(1);
    while (s.startsWith("/")) s = s.slice(1);
    while (s.endsWith("/") && s.length > 1) s = s.slice(0, -1);
    const parts = [];
    for (const p of s.split("/")) {
      if (!p || p === ".") continue;
      if (p === "..") {
        if (parts.length) parts.pop();
        continue;
      }
      parts.push(p);
    }
    if (!parts.length) return "/";
    return "/" + parts.join("/");
  }

  function fnv1a64(str) {
    let h = 14695981039346656037n;
    const prime = 1099511628211n;
    const mask = 0xffffffffffffffffn;
    const bytes = new TextEncoder().encode(str);
    for (const b of bytes) {
      h ^= BigInt(b);
      h = (h * prime) & mask;
    }
    return h;
  }

  function idHex(path) {
    return fnv1a64(normalizePath(path)).toString(16).padStart(16, "0");
  }
  function bookDirName(path) {
    return "book_" + idHex(path);
  }
  function packageRel(path) {
    return bookDirName(path) + "/package";
  }

  async function fileExists(dir, name) {
    try {
      await dir.getFileHandle(name);
      return true;
    } catch {
      return false;
    }
  }
  async function dirExists(dir, name) {
    try {
      await dir.getDirectoryHandle(name);
      return true;
    } catch {
      return false;
    }
  }
  async function ensureDir(parent, name) {
    return parent.getDirectoryHandle(name, { create: true });
  }
  async function writeBytes(dir, name, buf) {
    const fh = await dir.getFileHandle(name, { create: true });
    const w = await fh.createWritable();
    await w.write(buf);
    await w.close();
  }
  async function writeText(dir, name, text) {
    await writeBytes(dir, name, new TextEncoder().encode(text));
  }
  async function readText(dir, name) {
    try {
      const fh = await dir.getFileHandle(name);
      return await (await fh.getFile()).text();
    } catch {
      return null;
    }
  }
  async function getNested(rootDir, rel, create) {
    const parts = String(rel)
      .split("/")
      .filter(Boolean);
    let cur = rootDir;
    for (const p of parts) cur = await cur.getDirectoryHandle(p, { create: !!create });
    return cur;
  }
  async function destFileExists(rel) {
    const parts = String(rel)
      .split("/")
      .filter(Boolean);
    try {
      let cur = dst;
      for (let i = 0; i < parts.length - 1; i++) cur = await cur.getDirectoryHandle(parts[i]);
      await cur.getFileHandle(parts[parts.length - 1]);
      return true;
    } catch {
      return false;
    }
  }

  async function findBooks(dir, prefixParts, out, depth) {
    if (depth > 10 || out.length >= 800) return;
    for await (const [name, handle] of dir.entries()) {
      const low = name.toLowerCase();
      if (depth === 0 && SKIP_TOP.has(low)) continue;
      if (name === "." || name === "..") continue;
      if (handle.kind === "file") {
        if (BOOK_EXT.test(name)) {
          const parts = prefixParts.concat([name]);
          const devicePath = normalizePath("/" + parts.join("/"));
          out.push({ relParts: parts, devicePath, name });
        }
      } else if (handle.kind === "directory") {
        if (low === "system volume information") continue;
        await findBooks(handle, prefixParts.concat([name]), out, depth + 1);
      }
    }
  }

  function extractEpubFolder(p) {
    if (!p) return null;
    const s = String(p).replace(/\\/g, "/");
    let m = s.match(/epub_\d+/i);
    if (m) return m[0];
    m = s.match(/\/epub\/(\d+)\//i);
    if (m) return "epub_" + m[1];
    return null;
  }

  $("btnRoot").onclick = async () => {
    try {
      root = await showDirectoryPicker({ id: "casper-sd-v2", mode: "readwrite" });
      clearLog();
      log("SD root selected", "ok");
      if (await dirExists(root, ".casper")) {
        dst = await root.getDirectoryHandle(".casper");
        $("dstPath").textContent = ".casper (found · merge-only)";
      } else {
        dst = await ensureDir(root, ".casper");
        $("dstPath").textContent = ".casper (created)";
        log("Created .casper", "ok");
      }
      $("dstPath").classList.remove("empty");
      $("rootPath").textContent = root.name || "(SD root)";
      $("rootPath").classList.remove("empty");

      const chips = $("chips");
      chips.textContent = "";
      for (const n of [".crosspoint", "crosspoint", ".metadata"]) {
        if (await dirExists(root, n)) {
          const c = document.createElement("span");
          c.className = "chip ok";
          c.textContent = n;
          chips.appendChild(c);
        }
      }
      if (!chips.children.length) {
        const c = document.createElement("span");
        c.className = "chip";
        c.textContent = "no foreign roots (books-only scan is fine)";
        chips.appendChild(c);
      }
      $("btnScan").disabled = false;
      $("btnGo").disabled = true;
      plan = null;
      $("previewBody").style.display = "none";
      $("previewEmpty").style.display = "block";
      const st = $("status");
      st.style.display = "block";
      st.className = "callout ok";
      st.innerHTML = "Ready — run <strong>Scan &amp; preview</strong>.";
    } catch (e) {
      if (e.name !== "AbortError") log(String(e.message || e), "err");
    }
  };

  $("btnScan").onclick = async () => {
    if (!root || !dst) return;
    clearLog();
    cancel = false;
    setBar(0, "Scanning…");
    try {
      const books = [];
      await findBooks(root, [], books, 0);
      log("Found " + books.length + " book file(s)", "ok");

      // Cover path → legacy epub_* from recent.json
      const coverMap = {};
      for (const srcName of [".casper", ".crosspoint"]) {
        if (!(await dirExists(root, srcName))) continue;
        const d = await root.getDirectoryHandle(srcName);
        const t = await readText(d, "recent.json");
        if (!t) continue;
        try {
          const doc = JSON.parse(t);
          const arr = doc.books || doc.recent || (Array.isArray(doc) ? doc : []);
          for (const b of arr) {
            const ep = extractEpubFolder(b.coverBmpPath || b.cover || "");
            const p = normalizePath(b.path || "");
            if (ep && p) coverMap[p] = ep;
          }
        } catch (_) {}
      }

      const fileJobs = [];
      let skip = 0;

      // Settings from foreign roots
      for (const srcName of [".crosspoint", ".metadata"]) {
        if (!(await dirExists(root, srcName))) continue;
        const sdir = await root.getDirectoryHandle(srcName);
        for (const name of CFG) {
          if (!(await fileExists(sdir, name))) continue;
          if (await fileExists(dst, name)) {
            skip++;
            continue;
          }
          fileJobs.push({ type: "simple", srcDir: sdir, srcName: name, destRel: name });
        }
        for (const lib of ["clippings", "bookmarks", "synced_stats", "bookdata"]) {
          if (!(await dirExists(sdir, lib))) continue;
          const libDir = await sdir.getDirectoryHandle(lib);
          async function walk(d, prefix) {
            for await (const [n, h] of d.entries()) {
              const rel = prefix + "/" + n;
              if (h.kind === "file") {
                const destRel = rel.replace(/^\//, "");
                if (await destFileExists(destRel)) skip++;
                else fileJobs.push({ type: "simple", srcDir: d, srcName: n, destRel });
              } else {
                await walk(h, rel);
              }
            }
          }
          await walk(libDir, "/" + lib);
        }
      }

      const bookRows = [];
      for (const b of books) {
        const id = idHex(b.devicePath);
        const bdir = bookDirName(b.devicePath);
        const pkg = packageRel(b.devicePath);
        const legacy = coverMap[b.devicePath] || null;
        bookRows.push({ devicePath: b.devicePath, id, bdir, pkg, legacy });

        if (legacy) {
          for (const rootName of [".casper", ".crosspoint"]) {
            if (!(await dirExists(root, rootName))) continue;
            const rd = await root.getDirectoryHandle(rootName);
            if (!(await dirExists(rd, legacy))) continue;
            const ed = await rd.getDirectoryHandle(legacy);
            for (const fname of [
              "book.bin",
              "cover.bmp",
              "thumb.bmp",
              "progress.bin",
              "stats_v6.bin",
              "stats.bin",
              "statistics.bin",
            ]) {
              if (!(await fileExists(ed, fname))) continue;
              let destRel;
              if (fname === "progress.bin" || fname.startsWith("stats") || fname === "statistics.bin") {
                const dn =
                  fname === "progress.bin"
                    ? "progress.bin"
                    : fname === "stats_v6.bin"
                      ? "stats_v6.bin"
                      : "stats_v6.bin";
                destRel = bdir + "/" + dn;
              } else {
                destRel = pkg + "/" + fname;
              }
              if (await destFileExists(destRel)) {
                skip++;
                continue;
              }
              fileJobs.push({ type: "simple", srcDir: ed, srcName: fname, destRel });
            }
            break;
          }
        }
      }

      plan = { books: bookRows, fileJobs, skip };
      $("previewEmpty").style.display = "none";
      $("previewBody").style.display = "block";
      $("stBooks").textContent = String(bookRows.length);
      $("stCopy").textContent = String(fileJobs.length);
      $("stSkip").textContent = String(skip);
      const tb = $("tbody");
      tb.textContent = "";
      for (const r of bookRows.slice(0, 250)) {
        const tr = document.createElement("tr");
        tr.innerHTML =
          `<td><code>${esc(r.devicePath)}</code></td>` +
          `<td><code>${esc(r.id)}</code></td>` +
          `<td>${r.legacy ? "create + import " + esc(r.legacy) : "create dirs (package builds on device if empty)"}</td>`;
        tb.appendChild(tr);
      }
      $("btnGo").disabled = bookRows.length === 0 && fileJobs.length === 0;
      setBar(0, "Scan complete");
      log(`Scan: ${bookRows.length} books, ${fileJobs.length} copy ops, ${skip} skipped`, "ok");
    } catch (e) {
      log("Scan failed: " + (e.message || e), "err");
      setBar(0, "failed");
    }
  };

  $("btnCancel").onclick = () => {
    cancel = true;
    log("Cancel…", "warn");
  };

  $("btnGo").onclick = async () => {
    if (!plan || !dst) return;
    cancel = false;
    $("btnGo").disabled = true;
    $("btnCancel").disabled = false;
    $("btnScan").disabled = true;
    clearLog();
    log("Migration started (copy only, path-id layout)", "ok");
    let copied = 0;
    let errors = 0;
    const total = plan.books.length + plan.fileJobs.length + 2;
    let done = 0;
    const tick = (t) => {
      done++;
      setBar(total ? (done / total) * 100 : 100, t);
    };

    try {
      const perm = await dst.requestPermission({ mode: "readwrite" });
      if (perm !== "granted") throw new Error("Write permission denied");

      for (const b of plan.books) {
        if (cancel) throw new Error("Cancelled");
        let cur = dst;
        for (const p of b.bdir.split("/")) cur = await ensureDir(cur, p);
        await ensureDir(cur, "package");
        await ensureDir(cur, "rivulet");
        if (!(await fileExists(cur, "meta.txt"))) {
          await writeText(cur, "meta.txt", `id=${b.id}\npath=${b.devicePath}\n`);
          copied++;
        }
        tick(b.id);
      }

      for (const job of plan.fileJobs) {
        if (cancel) throw new Error("Cancelled");
        try {
          if (await destFileExists(job.destRel)) {
            tick("skip");
            continue;
          }
          const fh = await job.srcDir.getFileHandle(job.srcName);
          const buf = await (await fh.getFile()).arrayBuffer();
          const parts = job.destRel.split("/").filter(Boolean);
          const base = parts.pop();
          const parent = parts.length ? await getNested(dst, parts.join("/"), true) : dst;
          await writeBytes(parent, base, buf);
          copied++;
          if (copied <= 50 || copied % 40 === 0) log("copy " + job.destRel, "dim");
          tick(job.destRel);
        } catch (e) {
          errors++;
          log("ERR " + job.destRel + ": " + e.message, "err");
          tick("err");
        }
      }

      // ledger.tsv
      let ledger = (await readText(dst, "ledger.tsv")) || "";
      const have = new Set(
        ledger
          .split(/\r?\n/)
          .filter(Boolean)
          .map((l) => l.split("\t")[0])
      );
      const lines = [];
      for (const b of plan.books) {
        if (have.has(b.id)) continue;
        lines.push(b.id + "\t" + b.devicePath + "\t");
        have.add(b.id);
      }
      if (lines.length) {
        if (ledger && !ledger.endsWith("\n")) ledger += "\n";
        await writeText(dst, "ledger.tsv", ledger + lines.join("\n") + "\n");
        log("ledger +" + lines.length, "ok");
      }
      tick("ledger");

      await writeText(
        dst,
        MARKER,
        `v2 path-id host-html ${new Date().toISOString()} books=${plan.books.length} copied=${copied} errors=${errors}\n`
      );
      log("Wrote " + MARKER, "ok");
      tick("marker");

      setBar(100, "Done");
      const res = $("result");
      res.style.display = "block";
      res.className = "callout " + (errors ? "warn" : "ok");
      res.innerHTML = `<strong>Done.</strong> books=${plan.books.length}, copied≈${copied}, errors=${errors}. Eject SD and boot Casper.`;
      log("Finished", errors ? "warn" : "ok");
    } catch (e) {
      log(String(e.message || e), "err");
      const res = $("result");
      res.style.display = "block";
      res.className = "callout err";
      res.textContent = String(e.message || e);
    } finally {
      $("btnGo").disabled = false;
      $("btnCancel").disabled = true;
      $("btnScan").disabled = false;
    }
  };
})();
