const state = {
  items: [
    {
      id: "ringoven-01",
      title: "De ringoven",
      type: "show",
      source: "items/ringoven-01/",
      audience: "jeugd",
      notes: "Voeg audio.mp3 en tijdgestempelde dia's toe.",
      story: "Schrijf hier de verteltekst die later met TTS wordt ingesproken."
    }
  ]
};

const converter = {
  ffmpeg: null,
  fetchFile: null,
  classWorkerURL: null,
  loading: false,
  loaded: false,
  result: null
};

const localSd = {
  handle: null
};

const SD_DIRECTORIES = ["assets", "audio", "cards", "qr", "shows", "texts", "videos"];

const $ = (id) => document.getElementById(id);

function project() {
  return {
    schema: "qr-lamp-project-v1",
    name: $("projectName").value.trim(),
    organization: $("organization").value.trim(),
    language: $("language").value.trim() || "nl",
    description: $("description").value.trim(),
    sdFolderName: $("sdFolderName").value.trim() || "sdcard-gss",
    theme: { primary: $("primary").value },
    items: state.items.map((item) => {
      const content = { source: item.source };
      if (item.type === "video" && item.fps) content.fps = Number(item.fps);
      return {
        id: item.id,
        title: item.title,
        type: item.type,
        audience: item.audience,
        notes: item.notes,
        story: item.story || "",
        content
      };
    })
  };
}

function mediaPath(item) {
  const extension = extensionOf(item.source);
  if (item.type === "show") return `shows/${item.id}/show.csv`;
  if (item.type === "image") return `cards/${item.id}${extension || ".jpg"}`;
  if (item.type === "audio") return `audio/${item.id}${extension || ".mp3"}`;
  if (item.type === "video") return `videos/${item.id}${extension || ".mjpeg"}`;
  return "";
}

function extensionOf(path) {
  const match = path.toLowerCase().match(/(\.[a-z0-9]+)$/);
  return match ? match[1] : "";
}

function mediaMap() {
  const lines = ["# qr-content;relative-media-path;title shown on the display;optional mjpeg fps"];
  for (const item of state.items) {
    const fpsSuffix = item.type === "video" && item.fps ? `;${item.fps}` : "";
    lines.push(`${item.id};${mediaPath(item)};${item.title}${fpsSuffix}`);
  }
  return `${lines.join("\n")}\n`;
}

function validate() {
  const messages = [];
  const ids = new Set();
  if (!project().name) messages.push(["danger", "Projectnaam ontbreekt."]);
  for (const item of state.items) {
    if (!/^[a-z0-9][a-z0-9_-]*$/.test(item.id)) {
      messages.push(["danger", `${item.id || "(geen id)"}: QR-ID mag alleen kleine letters, cijfers, _ en - bevatten.`]);
    }
    if (ids.has(item.id)) messages.push(["danger", `${item.id}: dubbele QR-ID.`]);
    ids.add(item.id);
    if (!item.title) messages.push(["danger", `${item.id}: titel ontbreekt.`]);
    if (!item.source) messages.push(["danger", `${item.id}: bronbestand of bronmap ontbreekt.`]);
    if (item.type === "video" && ![".mjpeg", ".mjpg", ".avi"].includes(extensionOf(item.source))) {
      messages.push(["danger", `${item.id}: video moet voorbereid zijn als .mjpeg of .avi.`]);
    }
    if (item.type !== "video" && item.fps) {
      messages.push(["danger", `${item.id}: FPS hoort alleen bij video-items.`]);
    }
    if (item.fps && !["10", "15", "20", "25"].includes(String(item.fps))) {
      messages.push(["danger", `${item.id}: FPS moet 10, 15, 20 of 25 zijn.`]);
    }
    if (item.type === "audio" && ![".mp3", ".wav"].includes(extensionOf(item.source))) {
      messages.push(["danger", `${item.id}: audio moet .mp3 of .wav zijn.`]);
    }
    if (item.type === "show" && !item.source.endsWith("/")) {
      messages.push(["danger", `${item.id}: show-bron is normaal een map, bijvoorbeeld items/${item.id}/.`]);
    }
    if ((item.type === "show" || item.type === "image") && !item.story) {
      messages.push(["warn", `${item.id}: geen TTS-tekst ingevuld.`]);
    }
  }
  if (!messages.length) messages.push(["ok", "Project ziet er goed uit. Controleer lokaal nog of alle mediabestanden bestaan."]);
  return messages;
}

function renderItems() {
  $("itemsBody").innerHTML = "";
  for (const item of state.items) {
    const row = document.createElement("tr");
    row.innerHTML = `
      <td>${escapeHtml(item.id)}</td>
      <td>${escapeHtml(item.title)}</td>
      <td>${escapeHtml(item.type)}</td>
      <td><code>${escapeHtml(mediaPath(item))}</code></td>
      <td>${item.story ? "ja" : ""}</td>
      <td>${item.type === "video" && item.fps ? `${escapeHtml(item.fps)} fps` : ""}</td>
      <td><button class="secondary" data-delete="${escapeHtml(item.id)}" type="button">Verwijder</button></td>
    `;
    $("itemsBody").appendChild(row);
  }
  document.querySelectorAll("[data-delete]").forEach((button) => {
    button.addEventListener("click", () => {
      state.items = state.items.filter((item) => item.id !== button.dataset.delete);
      render();
    });
  });
}

function renderValidation() {
  $("validation").innerHTML = "";
  for (const [kind, text] of validate()) {
    const li = document.createElement("li");
    li.className = kind;
    li.textContent = text;
    $("validation").appendChild(li);
  }
}

function renderQr() {
  const holder = $("qrPreview");
  holder.innerHTML = "";
  for (const item of state.items) {
    const qr = qrcode(0, "M");
    qr.addData(item.id);
    qr.make();
    const card = document.createElement("div");
    card.className = "qr-card";
    card.innerHTML = `<strong>${escapeHtml(item.title)}</strong><p>${escapeHtml(item.id)}</p>${qr.createSvgTag(4, 2)}`;
    holder.appendChild(card);
  }
}

function renderCommands() {
  $("commands").textContent = [
    "python3 -m pip install -r tools/lampstudio/requirements.txt",
    "python3 tools/lampstudio/lampstudio.py validate projects/mijn-project",
    "python3 tools/lampstudio/lampstudio.py export projects/mijn-project --output sdcard-gss --overwrite",
    "VIDEO_FPS=10 ./tools/convert-video.sh bronvideo.mp4 sdcard-gss/videos/video-01"
  ].join("\n");
}

function render() {
  renderItems();
  renderValidation();
  $("mediaMap").textContent = mediaMap();
  renderQr();
  renderCommands();
  renderConverterCommand();
}

function addItem() {
  const id = $("itemId").value.trim();
  const title = $("itemTitle").value.trim();
  const type = $("itemType").value;
  const source = $("itemSource").value.trim() || (type === "show" ? `items/${id}/` : "");
  const fps = type === "video" ? $("itemFps").value : "";
  const audience = $("audience").value.trim();
  const notes = $("itemNotes").value.trim();
  upsertItem({ id, title, type, source, fps, audience, notes, story: "" });
  $("itemId").value = "";
  $("itemTitle").value = "";
  $("itemSource").value = "";
  $("itemFps").value = "";
  $("audience").value = "";
  $("itemNotes").value = "";
  render();
}

function defaultItemFolder(id) {
  return id ? `items/${id}/` : "";
}

function upsertItem(item) {
  const existing = state.items.find((candidate) => candidate.id === item.id);
  if (existing) Object.assign(existing, item);
  else state.items.push(item);
}

function addShowItem() {
  const id = cleanQrId($("showId").value);
  const title = $("showTitle").value.trim();
  const audience = $("showAudience").value.trim();
  const sourceValue = $("showSource").value.trim() || defaultItemFolder(id);
  const source = sourceValue.endsWith("/") ? sourceValue : `${sourceValue}/`;
  const story = $("showStory").value.trim();
  upsertItem({
    id,
    title,
    type: "show",
    source,
    fps: "",
    audience,
    notes: "Show: voeg audio.mp3 en tijdgestempelde dia's toe.",
    story
  });
  $("showId").value = "";
  $("showTitle").value = "";
  $("showAudience").value = "";
  $("showSource").value = "";
  $("showStory").value = "";
  render();
}

function addCardItem() {
  const id = cleanQrId($("cardId").value);
  const title = $("cardTitle").value.trim();
  const source = $("cardSource").value.trim() || `${id}.jpg`;
  const audience = $("cardAudience").value.trim();
  const story = $("cardStory").value.trim();
  upsertItem({
    id,
    title,
    type: "image",
    source,
    fps: "",
    audience,
    notes: "Kaart/dia: plaats beeld in cards/ en optionele MP3 in audio/.",
    story
  });
  $("cardId").value = "";
  $("cardTitle").value = "";
  $("cardSource").value = "";
  $("cardAudience").value = "";
  $("cardStory").value = "";
  render();
}

function download(filename, text, type = "text/plain") {
  const blob = new Blob([text], { type });
  const url = URL.createObjectURL(blob);
  const link = document.createElement("a");
  link.href = url;
  link.download = filename;
  document.body.appendChild(link);
  link.click();
  link.remove();
  URL.revokeObjectURL(url);
}

function downloadBlob(filename, blob) {
  const url = URL.createObjectURL(blob);
  const link = document.createElement("a");
  link.href = url;
  link.download = filename;
  document.body.appendChild(link);
  link.click();
  link.remove();
  URL.revokeObjectURL(url);
}

async function downloadZip() {
  const zip = new JSZip();
  const data = project();
  zip.file("project.json", JSON.stringify({
    schema: data.schema,
    name: data.name,
    organization: data.organization,
    language: data.language,
    description: data.description,
    sdFolderName: data.sdFolderName,
    theme: data.theme
  }, null, 2));
  for (const item of data.items) {
    zip.file(`items/${item.id}/item.json`, JSON.stringify(item, null, 2));
    if (item.story) {
      zip.file(`items/${item.id}/story.md`, `# ${item.title}\n\n${item.story}\n`);
      zip.file(`texts/${item.id}.txt`, `${item.story}\n`);
    } else if (item.type === "show") {
      zip.file(`items/${item.id}/story.md`, `# ${item.title}\n\nSchrijf hier de verteltekst.\n`);
    }
  }
  zip.file("README.txt", `Voeg mediabestanden lokaal toe en exporteer naar ${data.sdFolderName || "sdcard-gss"}.\n`);
  const blob = await zip.generateAsync({ type: "blob" });
  const url = URL.createObjectURL(blob);
  const link = document.createElement("a");
  link.href = url;
  link.download = "lampstudio-project.zip";
  document.body.appendChild(link);
  link.click();
  link.remove();
  URL.revokeObjectURL(url);
}

function importProject(file) {
  const reader = new FileReader();
  reader.onload = () => {
    const data = JSON.parse(String(reader.result));
    $("projectName").value = data.name || "";
    $("organization").value = data.organization || "";
    $("language").value = data.language || "nl";
    $("description").value = data.description || "";
    $("sdFolderName").value = data.sdFolderName || "sdcard-gss";
    $("primary").value = data.theme?.primary || "#103c6b";
    state.items = (data.items || []).map((item) => ({
      id: item.id || "",
      title: item.title || "",
      type: item.type || "show",
      source: item.content?.source || "",
      fps: item.type === "video" && item.content?.fps ? String(item.content.fps) : "",
      audience: item.audience || "",
      notes: item.notes || "",
      story: item.story || ""
    }));
    render();
  };
  reader.readAsText(file);
}

function cleanQrId(value) {
  return String(value)
    .trim()
    .toLowerCase()
    .replace(/\.[a-z0-9]+$/, "")
    .replace(/[^a-z0-9_-]+/g, "-")
    .replace(/^-+|-+$/g, "")
    .replace(/--+/g, "-");
}

function formatBytes(bytes) {
  if (!bytes && bytes !== 0) return "";
  const units = ["B", "KB", "MB", "GB"];
  let size = bytes;
  let index = 0;
  while (size >= 1024 && index < units.length - 1) {
    size /= 1024;
    index += 1;
  }
  return `${size.toFixed(index ? 1 : 0)} ${units[index]}`;
}

function setConverterStatus(message, kind = "") {
  const element = $("converterStatus");
  element.textContent = message;
  element.className = `status-line ${kind}`.trim();
}

function setSdStatus(message, kind = "") {
  const element = $("sdStatus");
  element.textContent = message;
  element.className = `status-line ${kind}`.trim();
}

function updateSdControls() {
  const hasHandle = Boolean(localSd.handle);
  $("prepareSdFolder").disabled = !hasHandle;
  $("writeMapToSd").disabled = !hasHandle;
  $("writeConvertedToSd").disabled = !hasHandle || !converter.result;
  $("sdFolderBadge").textContent = hasHandle ? `Gekozen: ${localSd.handle.name}` : "Geen lokale map gekozen";
  $("sdFolderBadge").className = hasHandle ? "badge" : "badge muted";
  $("selectedSdFolder").value = hasHandle ? localSd.handle.name : "Nog geen map gekozen";
}

function appendConverterLog(message) {
  const log = $("converterLog");
  const lines = log.textContent ? log.textContent.split("\n").filter(Boolean) : [];
  lines.push(message);
  $("converterLog").textContent = lines.slice(-40).join("\n");
}

function clearConverterResult() {
  converter.result = null;
  $("downloadConvertedZip").disabled = true;
  $("addConvertedItem").disabled = true;
  $("writeConvertedToSd").disabled = true;
  $("videoOutputSummary").hidden = true;
  $("videoOutputSummary").textContent = "";
}

function videoFilter(fps) {
  return `fps=${fps},scale=480:272:force_original_aspect_ratio=decrease,pad=480:272:(ow-iw)/2:(oh-ih)/2:color=black,format=yuvj420p`;
}

function renderConverterCommand() {
  const fps = $("videoFps")?.value || "10";
  const id = cleanQrId($("videoId")?.value || "video-01") || "video-01";
  $("converterCommand").textContent = [
    `ffmpeg -y -i bronvideo.mp4 -an -vf "${videoFilter(fps)}" -q:v 7 -f mjpeg ${id}.mjpeg`,
    `ffmpeg -y -i bronvideo.mp4 -vn -ac 1 -ar 44100 -b:a 96k ${id}.mp3`
  ].join("\n");
}

async function buildFfmpegClassWorkerURL() {
  if (converter.classWorkerURL) return converter.classWorkerURL;
  const packageBaseURL = "https://cdn.jsdelivr.net/npm/@ffmpeg/ffmpeg@0.12.10/dist/esm";
  const response = await fetch(`${packageBaseURL}/worker.js`);
  if (!response.ok) {
    throw new Error(`Kan ffmpeg worker niet laden (${response.status}).`);
  }
  const source = (await response.text())
    .replaceAll('from "./const.js"', `from "${packageBaseURL}/const.js"`)
    .replaceAll('from "./errors.js"', `from "${packageBaseURL}/errors.js"`);
  converter.classWorkerURL = URL.createObjectURL(new Blob([source], { type: "text/javascript" }));
  return converter.classWorkerURL;
}

async function ensureFfmpeg() {
  if (converter.loaded) return;
  if (converter.loading) {
    while (converter.loading) {
      await new Promise((resolve) => setTimeout(resolve, 150));
    }
    return;
  }

  converter.loading = true;
  setConverterStatus("ffmpeg.wasm laden… dit gebeurt alleen bij de eerste conversie.");
  appendConverterLog("Laad ffmpeg.wasm core via CDN.");

  const [{ FFmpeg }, { fetchFile, toBlobURL }] = await Promise.all([
    import("https://cdn.jsdelivr.net/npm/@ffmpeg/ffmpeg@0.12.10/dist/esm/index.js"),
    import("https://cdn.jsdelivr.net/npm/@ffmpeg/util@0.12.1/dist/esm/index.js")
  ]);

  const ffmpeg = new FFmpeg();
  ffmpeg.on("log", ({ message }) => {
    if (message) appendConverterLog(message);
  });
  ffmpeg.on("progress", ({ progress }) => {
    if (Number.isFinite(progress) && progress > 0) {
      setConverterStatus(`Converteren… ${Math.min(99, Math.round(progress * 100))}%`);
    }
  });

  const classWorkerURL = await buildFfmpegClassWorkerURL();
  const baseURL = "https://cdn.jsdelivr.net/npm/@ffmpeg/core@0.12.10/dist/esm";
  await ffmpeg.load({
    classWorkerURL,
    coreURL: await toBlobURL(`${baseURL}/ffmpeg-core.js`, "text/javascript"),
    wasmURL: await toBlobURL(`${baseURL}/ffmpeg-core.wasm`, "application/wasm")
  });

  converter.ffmpeg = ffmpeg;
  converter.fetchFile = fetchFile;
  converter.loaded = true;
  converter.loading = false;
}

async function convertVideo() {
  const file = $("videoFile").files[0];
  const id = cleanQrId($("videoId").value || file?.name || "");
  const title = $("videoTitle").value.trim() || id;
  const fps = $("videoFps").value;

  clearConverterResult();
  $("converterLog").textContent = "";

  if (!file) {
    setConverterStatus("Kies eerst een videobestand.", "danger");
    return;
  }
  if (!/^[a-z0-9][a-z0-9_-]*$/.test(id)) {
    setConverterStatus("Vul een geldige QR-ID in: kleine letters, cijfers, _ of -.", "danger");
    return;
  }

  $("videoId").value = id;
  if (!$("videoTitle").value.trim()) $("videoTitle").value = title;

  try {
    await ensureFfmpeg();
    const ffmpeg = converter.ffmpeg;
    const inputName = `input-${Date.now()}${extensionOf(file.name) || ".mp4"}`;
    const videoName = `${id}.mjpeg`;
    const audioName = `${id}.mp3`;

    setConverterStatus("Bronvideo voorbereiden…");
    await ffmpeg.writeFile(inputName, await converter.fetchFile(file));

    setConverterStatus(`MJPEG maken op ${fps} fps…`);
    await ffmpeg.exec([
      "-y",
      "-i", inputName,
      "-an",
      "-vf", videoFilter(fps),
      "-q:v", "7",
      "-f", "mjpeg",
      videoName
    ]);
    const videoData = await ffmpeg.readFile(videoName);

    let audioData = null;
    let audioWarning = "";
    try {
      setConverterStatus("Audio als MP3 maken…");
      await ffmpeg.exec([
        "-y",
        "-i", inputName,
        "-vn",
        "-ac", "1",
        "-ar", "44100",
        "-b:a", "96k",
        audioName
      ]);
      audioData = await ffmpeg.readFile(audioName);
    } catch (error) {
      audioWarning = "Geen audio gevonden of MP3-conversie is mislukt; de ZIP bevat alleen MJPEG.";
      appendConverterLog(audioWarning);
    }

    converter.result = { id, title, fps, videoData, audioData, audioWarning };
    $("downloadConvertedZip").disabled = false;
    $("addConvertedItem").disabled = false;
    updateSdControls();
    $("videoOutputSummary").hidden = false;
    $("videoOutputSummary").innerHTML = [
      `<strong>Klaar:</strong> videos/${escapeHtml(videoName)} (${formatBytes(videoData.length)})`,
      audioData ? `videos/${escapeHtml(audioName)} (${formatBytes(audioData.length)})` : escapeHtml(audioWarning)
    ].join("<br>");
    setConverterStatus(`Conversie klaar: ${id} op ${fps} fps.`, "ok");

    await safeDeleteFfmpegFile(inputName);
    await safeDeleteFfmpegFile(videoName);
    if (audioData) await safeDeleteFfmpegFile(audioName);
  } catch (error) {
    converter.loading = false;
    setConverterStatus(`Conversie mislukt: ${error.message || error}`, "danger");
    appendConverterLog(String(error.stack || error));
  }
}

async function safeDeleteFfmpegFile(name) {
  try {
    await converter.ffmpeg.deleteFile(name);
  } catch {
    // Best-effort cleanup only.
  }
}

function qrSvg(id) {
  const qr = qrcode(0, "M");
  qr.addData(id);
  qr.make();
  return qr.createSvgTag(8, 2);
}

async function downloadConvertedZip() {
  if (!converter.result) return;
  const { id, title, fps, videoData, audioData, audioWarning } = converter.result;
  const zip = new JSZip();
  zip.file(`videos/${id}.mjpeg`, videoData);
  if (audioData) zip.file(`videos/${id}.mp3`, audioData);
  zip.file(`qr/${id}.svg`, qrSvg(id));
  zip.file("media-map.csv", [
    "# qr-content;relative-media-path;title shown on the display;optional mjpeg fps",
    `${id};videos/${id}.mjpeg;${title};${fps}`,
    ""
  ].join("\n"));
  zip.file("README.txt", [
    `QR-lamp video-export voor: ${title}`,
    `QR-ID: ${id}`,
    `Video: videos/${id}.mjpeg`,
    audioData ? `Audio: videos/${id}.mp3` : audioWarning,
    `Preset: 480x272 @ ${fps} fps, MJPEG video, MP3 mono 44.1 kHz`,
    "",
    "Kopieer de bestanden uit videos/ naar de videos-map op de SD-kaart.",
    "Voeg de media-map.csv-regel toe aan de media-map van je project of SD-export."
  ].join("\n"));
  const blob = await zip.generateAsync({ type: "blob" });
  downloadBlob(`${id}-qr-lamp-video.zip`, blob);
}

async function getDirectory(rootHandle, path, create = true) {
  let directory = rootHandle;
  for (const part of path.split("/").filter(Boolean)) {
    directory = await directory.getDirectoryHandle(part, { create });
  }
  return directory;
}

async function writeTextFile(rootHandle, path, text) {
  const parts = path.split("/").filter(Boolean);
  const filename = parts.pop();
  const directory = parts.length ? await getDirectory(rootHandle, parts.join("/")) : rootHandle;
  const file = await directory.getFileHandle(filename, { create: true });
  const writable = await file.createWritable();
  await writable.write(text);
  await writable.close();
}

async function readTextFile(rootHandle, path) {
  const parts = path.split("/").filter(Boolean);
  const filename = parts.pop();
  const directory = parts.length ? await getDirectory(rootHandle, parts.join("/"), false) : rootHandle;
  const file = await directory.getFileHandle(filename, { create: false });
  return await (await file.getFile()).text();
}

async function writeBinaryFile(rootHandle, path, data) {
  const parts = path.split("/").filter(Boolean);
  const filename = parts.pop();
  const directory = parts.length ? await getDirectory(rootHandle, parts.join("/")) : rootHandle;
  const file = await directory.getFileHandle(filename, { create: true });
  const writable = await file.createWritable();
  await writable.write(data);
  await writable.close();
}

async function prepareSdFolder() {
  if (!localSd.handle) return;
  for (const directory of SD_DIRECTORIES) {
    await localSd.handle.getDirectoryHandle(directory, { create: true });
  }
}

async function pickSdFolder() {
  if (!("showDirectoryPicker" in window)) {
    setSdStatus("Deze browser ondersteunt direct schrijven naar lokale mappen niet. Gebruik Chrome/Edge of download ZIP-bestanden.", "danger");
    return;
  }
  try {
    localSd.handle = await window.showDirectoryPicker({
      id: "qr-lamp-sd",
      mode: "readwrite",
      startIn: "documents"
    });
    $("sdFolderName").value = localSd.handle.name || $("sdFolderName").value;
    await prepareSdFolder();
    setSdStatus(`Lokale SD-map gekozen: ${localSd.handle.name}. Mapstructuur is aangemaakt/gecontroleerd.`, "ok");
  } catch (error) {
    if (error.name !== "AbortError") {
      setSdStatus(`Kan lokale SD-map niet openen: ${error.message || error}`, "danger");
    }
  } finally {
    updateSdControls();
  }
}

async function writeMapAndQrToSd() {
  if (!localSd.handle) return;
  try {
    await prepareSdFolder();
    await writeTextFile(localSd.handle, "media-map.csv", mediaMap());
    for (const item of state.items) {
      await writeTextFile(localSd.handle, `qr/${item.id}.svg`, qrSvg(item.id));
      if (item.story) {
        await writeTextFile(localSd.handle, `texts/${item.id}.txt`, `${item.story}\n`);
      }
    }
    setSdStatus(`media-map.csv, ${state.items.length} QR-label(s) en TTS-teksten geschreven naar ${localSd.handle.name}.`, "ok");
  } catch (error) {
    setSdStatus(`Schrijven naar lokale SD-map mislukt: ${error.message || error}`, "danger");
  }
}

async function writeConvertedToSd() {
  if (!localSd.handle || !converter.result) return;
  const { id, title, fps, videoData, audioData } = converter.result;
  try {
    await prepareSdFolder();
    await writeBinaryFile(localSd.handle, `videos/${id}.mjpeg`, videoData);
    if (audioData) await writeBinaryFile(localSd.handle, `videos/${id}.mp3`, audioData);
    await writeTextFile(localSd.handle, `qr/${id}.svg`, qrSvg(id));
    const row = `${id};videos/${id}.mjpeg;${title};${fps}`;
    let map = "";
    try {
      map = await readTextFile(localSd.handle, "media-map.csv");
    } catch {
      map = "# qr-content;relative-media-path;title shown on the display;optional mjpeg fps\n";
    }
    const lines = map.split(/\r?\n/).filter((line) => line.trim());
    const withoutOld = lines.filter((line) => line.startsWith("#") || !line.startsWith(`${id};`));
    withoutOld.push(row);
    await writeTextFile(localSd.handle, "media-map.csv", `${withoutOld.join("\n")}\n`);
    setSdStatus(`Video, QR en media-mapregel geschreven naar ${localSd.handle.name}: videos/${id}.mjpeg${audioData ? " + MP3" : ""}.`, "ok");
  } catch (error) {
    setSdStatus(`Video schrijven mislukt: ${error.message || error}`, "danger");
  }
}

function addConvertedItem() {
  if (!converter.result) return;
  const { id, title, fps } = converter.result;
  const existing = state.items.find((item) => item.id === id);
  const item = {
    id,
    title,
    type: "video",
    source: `${id}.mjpeg`,
    fps,
    audience: "",
    notes: `Geconverteerd in Lamp Studio op 480x272 @ ${fps} fps.`
  };
  if (existing) Object.assign(existing, item);
  else state.items.push(item);
  render();
}

function escapeHtml(value) {
  return String(value).replace(/[&<>"']/g, (character) => ({
    "&": "&amp;",
    "<": "&lt;",
    ">": "&gt;",
    "\"": "&quot;",
    "'": "&#039;"
  }[character]));
}

document.addEventListener("DOMContentLoaded", () => {
  $("addItem").addEventListener("click", addItem);
  $("addShowItem").addEventListener("click", addShowItem);
  $("addCardItem").addEventListener("click", addCardItem);
  $("pickSdFolder").addEventListener("click", pickSdFolder);
  $("prepareSdFolder").addEventListener("click", async () => {
    try {
      await prepareSdFolder();
      setSdStatus(`Mapstructuur gecontroleerd in ${localSd.handle.name}.`, "ok");
    } catch (error) {
      setSdStatus(`Mapstructuur maken mislukt: ${error.message || error}`, "danger");
    }
  });
  $("writeMapToSd").addEventListener("click", writeMapAndQrToSd);
  $("convertVideo").addEventListener("click", convertVideo);
  $("downloadConvertedZip").addEventListener("click", downloadConvertedZip);
  $("writeConvertedToSd").addEventListener("click", writeConvertedToSd);
  $("addConvertedItem").addEventListener("click", addConvertedItem);
  $("videoFile").addEventListener("change", (event) => {
    const file = event.target.files[0];
    clearConverterResult();
    if (file) {
      if (!$("videoId").value.trim()) $("videoId").value = cleanQrId(file.name) || "video-01";
      if (!$("videoTitle").value.trim()) $("videoTitle").value = file.name.replace(/\.[^.]+$/, "");
      setConverterStatus(`${file.name} gekozen (${formatBytes(file.size)}).`);
    } else {
      setConverterStatus("Nog geen video gekozen.");
    }
    renderConverterCommand();
  });
  ["videoId", "videoTitle", "videoFps"].forEach((id) => {
    $(id).addEventListener("input", () => {
      clearConverterResult();
      renderConverterCommand();
    });
    $(id).addEventListener("change", renderConverterCommand);
  });
  $("downloadProject").addEventListener("click", () => download("lampstudio-project.json", JSON.stringify(project(), null, 2), "application/json"));
  $("downloadMap").addEventListener("click", () => download("media-map.csv", mediaMap(), "text/csv"));
  $("downloadZip").addEventListener("click", downloadZip);
  $("importFile").addEventListener("change", (event) => {
    const file = event.target.files[0];
    if (file) importProject(file);
  });
  ["projectName", "organization", "language", "primary", "description", "sdFolderName"].forEach((id) => {
    $(id).addEventListener("input", render);
  });
  $("showId").addEventListener("input", () => {
    const id = cleanQrId($("showId").value);
    if (!$("showSource").value.trim()) $("showSource").placeholder = defaultItemFolder(id || "ringoven-jeugd-01");
  });
  $("cardId").addEventListener("input", () => {
    const id = cleanQrId($("cardId").value);
    if (!$("cardSource").value.trim()) $("cardSource").placeholder = `${id || "gss-001"}.jpg`;
  });
  render();
  updateSdControls();
});
