const state = {
  items: [
    {
      id: "ringoven-01",
      title: "De ringoven",
      type: "show",
      source: "items/ringoven-01/",
      audience: "jeugd",
      notes: "Voeg audio.mp3 en tijdgestempelde dia's toe."
    }
  ]
};

const $ = (id) => document.getElementById(id);

function project() {
  return {
    schema: "qr-lamp-project-v1",
    name: $("projectName").value.trim(),
    organization: $("organization").value.trim(),
    language: $("language").value.trim() || "nl",
    description: $("description").value.trim(),
    theme: { primary: $("primary").value },
    items: state.items.map((item) => ({
      id: item.id,
      title: item.title,
      type: item.type,
      audience: item.audience,
      notes: item.notes,
      content: { source: item.source }
    }))
  };
}

function mediaPath(item) {
  const extension = extensionOf(item.source);
  if (item.type === "show") return `shows/${item.id}/show.csv`;
  if (item.type === "image") return `info/${item.id}${extension || ".jpg"}`;
  if (item.type === "audio") return `audio/${item.id}${extension || ".mp3"}`;
  if (item.type === "video") return `mjpeg/${item.id}${extension || ".mjpeg"}`;
  return "";
}

function extensionOf(path) {
  const match = path.toLowerCase().match(/(\.[a-z0-9]+)$/);
  return match ? match[1] : "";
}

function mediaMap() {
  const lines = ["# qr-content;relative-media-path;title shown on the display"];
  for (const item of state.items) {
    lines.push(`${item.id};${mediaPath(item)};${item.title}`);
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
    if (item.type === "audio" && ![".mp3", ".wav"].includes(extensionOf(item.source))) {
      messages.push(["danger", `${item.id}: audio moet .mp3 of .wav zijn.`]);
    }
    if (item.type === "show" && !item.source.endsWith("/")) {
      messages.push(["danger", `${item.id}: show-bron is normaal een map, bijvoorbeeld items/${item.id}/.`]);
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
    "python3 tools/lampstudio/lampstudio.py export projects/mijn-project --output sd-export --overwrite",
    "VIDEO_FPS=25 ./tools/convert-video.sh bronvideo.mp4 projects/mijn-project/items/video-01/video"
  ].join("\n");
}

function render() {
  renderItems();
  renderValidation();
  $("mediaMap").textContent = mediaMap();
  renderQr();
  renderCommands();
}

function addItem() {
  const id = $("itemId").value.trim();
  const title = $("itemTitle").value.trim();
  const type = $("itemType").value;
  const source = $("itemSource").value.trim() || (type === "show" ? `items/${id}/` : "");
  const audience = $("audience").value.trim();
  const notes = $("itemNotes").value.trim();
  state.items.push({ id, title, type, source, audience, notes });
  $("itemId").value = "";
  $("itemTitle").value = "";
  $("itemSource").value = "";
  $("audience").value = "";
  $("itemNotes").value = "";
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

async function downloadZip() {
  const zip = new JSZip();
  const data = project();
  zip.file("project.json", JSON.stringify({
    schema: data.schema,
    name: data.name,
    organization: data.organization,
    language: data.language,
    description: data.description,
    theme: data.theme
  }, null, 2));
  for (const item of data.items) {
    zip.file(`items/${item.id}/item.json`, JSON.stringify(item, null, 2));
    if (item.type === "show") {
      zip.file(`items/${item.id}/story.md`, `# ${item.title}\n\nSchrijf hier de verteltekst.\n`);
    }
  }
  zip.file("README.txt", "Voeg mediabestanden lokaal toe en draai tools/lampstudio/lampstudio.py validate/export.\n");
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
    $("primary").value = data.theme?.primary || "#103c6b";
    state.items = (data.items || []).map((item) => ({
      id: item.id || "",
      title: item.title || "",
      type: item.type || "show",
      source: item.content?.source || "",
      audience: item.audience || "",
      notes: item.notes || ""
    }));
    render();
  };
  reader.readAsText(file);
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
  $("downloadProject").addEventListener("click", () => download("lampstudio-project.json", JSON.stringify(project(), null, 2), "application/json"));
  $("downloadMap").addEventListener("click", () => download("media-map.csv", mediaMap(), "text/csv"));
  $("downloadZip").addEventListener("click", downloadZip);
  $("importFile").addEventListener("change", (event) => {
    const file = event.target.files[0];
    if (file) importProject(file);
  });
  ["projectName", "organization", "language", "primary", "description"].forEach((id) => {
    $(id).addEventListener("input", render);
  });
  render();
});

