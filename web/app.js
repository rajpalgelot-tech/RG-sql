/* ==========================================================================
 * RajSQL playground: consumer for the RajSQL WASM module (rajsql.js).
 * No external dependencies; runs fully offline.
 * ========================================================================== */

"use strict";

/* ------------------------------------------------------------------ */
/* DOM helpers                                                         */
/* ------------------------------------------------------------------ */

const $ = (id) => document.getElementById(id);

/** Escape a string for safe interpolation into innerHTML. */
function esc(s) {
  return String(s)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

/* ------------------------------------------------------------------ */
/* Sample dataset: a small university schema                          */
/* ------------------------------------------------------------------ */

const SAMPLE_DROPS = "DROP TABLE enrollments; DROP TABLE students; DROP TABLE courses;";

const SAMPLE_SQL = `
CREATE TABLE students (id INTEGER, name TEXT, major TEXT, gpa REAL, year INTEGER);
INSERT INTO students VALUES (1,  'Aarav Sharma',   'Computer Science', 3.91, 3);
INSERT INTO students VALUES (2,  'Meera Iyer',     'Electrical Eng.',  3.74, 2);
INSERT INTO students VALUES (3,  'Daniel Kim',     'Computer Science', 3.45, 4);
INSERT INTO students VALUES (4,  'Sofia Rossi',    'Mathematics',      3.88, 1);
INSERT INTO students VALUES (5,  'Liam O''Brien',  'Mechanical Eng.',  3.12, 3);
INSERT INTO students VALUES (6,  'Priya Nair',     'Computer Science', 3.97, 2);
INSERT INTO students VALUES (7,  'Noah Fischer',   'Physics',          3.29, 4);
INSERT INTO students VALUES (8,  'Emma Laurent',   'Mathematics',      3.62, 3);
INSERT INTO students VALUES (9,  'Hiroshi Tanaka', 'Electrical Eng.',  3.51, 1);
INSERT INTO students VALUES (10, 'Zara Ahmed',     'Computer Science', 3.35, 4);
INSERT INTO students VALUES (11, 'Elena Petrova',  'Physics',          3.79, 2);
INSERT INTO students VALUES (12, 'Marcus Johnson', 'Mechanical Eng.',  2.98, 2);

CREATE TABLE courses (course_id INTEGER, title TEXT, credits INTEGER);
INSERT INTO courses VALUES (101, 'Database Systems',        4);
INSERT INTO courses VALUES (102, 'Operating Systems',       4);
INSERT INTO courses VALUES (103, 'Linear Algebra',          3);
INSERT INTO courses VALUES (104, 'Data Structures',         3);
INSERT INTO courses VALUES (105, 'Quantum Mechanics',       4);
INSERT INTO courses VALUES (106, 'Control Systems',         3);

CREATE TABLE enrollments (student_id INTEGER, course_id INTEGER, grade TEXT);
INSERT INTO enrollments VALUES (1,  101, 'A');
INSERT INTO enrollments VALUES (1,  104, 'A-');
INSERT INTO enrollments VALUES (2,  102, 'B+');
INSERT INTO enrollments VALUES (3,  101, 'B');
INSERT INTO enrollments VALUES (3,  104, 'B+');
INSERT INTO enrollments VALUES (4,  103, 'A');
INSERT INTO enrollments VALUES (5,  106, 'C+');
INSERT INTO enrollments VALUES (6,  101, 'A');
INSERT INTO enrollments VALUES (6,  102, 'A-');
INSERT INTO enrollments VALUES (7,  105, 'B-');
INSERT INTO enrollments VALUES (8,  103, 'A-');
INSERT INTO enrollments VALUES (9,  102, 'B');
INSERT INTO enrollments VALUES (10, 104, 'B-');
INSERT INTO enrollments VALUES (11, 105, 'B+');
INSERT INTO enrollments VALUES (12, 106, 'C');
`;

/* ------------------------------------------------------------------ */
/* Example queries (clickable)                                         */
/* ------------------------------------------------------------------ */

const EXAMPLES = [
  ["Simple SELECT", "SELECT * FROM students LIMIT 5;"],
  ["WHERE + AND/OR", "SELECT name, major, gpa FROM students\nWHERE (major = 'Computer Science' OR major = 'Mathematics')\n  AND gpa > 3.5;"],
  ["ORDER BY + LIMIT", "SELECT name, gpa FROM students\nORDER BY gpa DESC\nLIMIT 5;"],
  ["Aggregates", "SELECT COUNT(*), AVG(gpa), MIN(gpa), MAX(gpa), SUM(gpa)\nFROM students;"],
  ["INNER JOIN", "SELECT s.name, c.title, e.grade\nFROM students s\nJOIN enrollments e ON s.id = e.student_id\nJOIN courses c ON e.course_id = c.course_id\nORDER BY s.name;"],
  ["INSERT", "INSERT INTO students VALUES (13, 'Grace Hopper', 'Computer Science', 4.0, 1);"],
  ["UPDATE", "UPDATE students SET gpa = 4.0 WHERE id = 12;"],
  ["DELETE", "DELETE FROM enrollments WHERE student_id = 12;"],
  ["Transactions", "BEGIN;\nUPDATE students SET gpa = gpa + 0.5 WHERE major = 'Physics';\nSELECT * FROM students WHERE major = 'Physics';\nROLLBACK;\nSELECT * FROM students WHERE major = 'Physics';"],
  ["Aggregate + JOIN", "SELECT c.title, COUNT(*) AS enrolled\nFROM enrollments e\nJOIN courses c ON e.course_id = c.course_id\nGROUP BY c.title;"],
];

/* ------------------------------------------------------------------ */
/* SQL mini-highlighter                                                */
/* ------------------------------------------------------------------ */

const SQL_KEYWORDS = new Set([
  "SELECT", "FROM", "WHERE", "INSERT", "INTO", "VALUES", "CREATE", "TABLE",
  "DROP", "UPDATE", "SET", "DELETE", "JOIN", "ON", "ORDER", "BY", "ASC",
  "DESC", "LIMIT", "BEGIN", "COMMIT", "ROLLBACK", "AND", "OR", "NOT",
  "COUNT", "SUM", "AVG", "MIN", "MAX", "INTEGER", "REAL", "TEXT", "NULL",
]);

/**
 * Tokenize SQL into a single pass and emit escaped, spanned HTML.
 * Order matters: comments, strings, then words/numbers.
 */
function highlightSQL(sql) {
  const re = /(--[^\n]*)|('(?:[^']|'')*')|([A-Za-z_][A-Za-z0-9_]*)|(\d+(?:\.\d+)?)|(\n)|(.)/g;
  let out = "";
  let m;
  while ((m = re.exec(sql)) !== null) {
    if (m[1] !== undefined) out += `<span class="com">${esc(m[1])}</span>`;
    else if (m[2] !== undefined) out += `<span class="str">${esc(m[2])}</span>`;
    else if (m[3] !== undefined) {
      const word = m[3];
      if (SQL_KEYWORDS.has(word.toUpperCase())) out += `<span class="kw">${esc(word)}</span>`;
      else out += esc(word);
    } else if (m[4] !== undefined) out += `<span class="num">${esc(m[4])}</span>`;
    else out += esc(m[5] !== undefined ? m[5] : m[6]);
  }
  // Trailing newline would collapse in the <pre>, add a spacer to keep heights equal.
  return out + "\n ";
}

/* ------------------------------------------------------------------ */
/* Editor                                                              */
/* ------------------------------------------------------------------ */

const editor = $("editor");
const highlight = $("editor-highlight");
const history = [];
let historyIndex = -1; // position when browsing; -1 = live buffer
let liveBuffer = "";

function renderHighlight() {
  // highlightSQL already appends a trailing spacer line to keep <pre> height
  // equal to the textarea when content ends with a newline.
  highlight.innerHTML = highlightSQL(editor.value);
}

function syncScroll() {
  highlight.scrollTop = editor.scrollTop;
  highlight.scrollLeft = editor.scrollLeft;
}

editor.addEventListener("input", () => {
  renderHighlight();
  syncScroll();
});

editor.addEventListener("scroll", syncScroll);

// Tab inserts two spaces.
editor.addEventListener("keydown", (e) => {
  if (e.key === "Tab") {
    e.preventDefault();
    const start = editor.selectionStart;
    const end = editor.selectionEnd;
    editor.setRangeText("  ", start, end, "end");
    renderHighlight();
  }
});

// Ctrl+Enter / Cmd+Enter runs; Up/Down on first/last line walk history.
editor.addEventListener("keydown", (e) => {
  if ((e.ctrlKey || e.metaKey) && e.key === "Enter") {
    e.preventDefault();
    runSQL();
    return;
  }
  const isFirstLine = editor.value.slice(0, editor.selectionStart).indexOf("\n") === -1;
  const afterCaret = editor.value.slice(editor.selectionEnd);
  const isLastLine = afterCaret.indexOf("\n") === -1;
  if (e.key === "ArrowUp" && isFirstLine && history.length > 0) {
    e.preventDefault();
    if (historyIndex === -1) { liveBuffer = editor.value; historyIndex = history.length; }
    historyIndex = Math.max(0, historyIndex - 1);
    editor.value = history[historyIndex];
    editor.selectionStart = editor.selectionEnd = editor.value.length;
    renderHighlight();
  } else if (e.key === "ArrowDown" && isLastLine && historyIndex !== -1) {
    e.preventDefault();
    historyIndex++;
    if (historyIndex >= history.length) {
      historyIndex = -1;
      editor.value = liveBuffer;
    } else {
      editor.value = history[historyIndex];
    }
    editor.selectionStart = editor.selectionEnd = editor.value.length;
    renderHighlight();
  }
});

function pushHistory(sql) {
  const trimmed = sql.trim();
  if (!trimmed || history[history.length - 1] === trimmed) return;
  history.push(trimmed);
  historyIndex = -1;
}

/* ------------------------------------------------------------------ */
/* Module loading & persistence                                        */
/* ------------------------------------------------------------------ */

let Module = null;
let dbHandle = 0;
let persistDirty = false;

function setStatus(text, cls) {
  const el = $("persist-status");
  el.textContent = text;
  el.className = "persist-status " + (cls || "muted");
}

function markDirty() {
  persistDirty = true;
  setStatus("unsaved changes", "dirty");
}

function markPersisted() {
  persistDirty = false;
  setStatus("DB loaded · persisted in IndexedDB", "ok");
}

/** Flush the Emscripten virtual FS to IndexedDB. */
function persistNow(cb) {
  if (!Module) return;
  try {
    Module.FS.syncfs(false, (err) => {
      if (err) {
        console.error("syncfs failed:", err);
        setStatus("persist failed", "");
      } else {
        markPersisted();
      }
      if (cb) cb(err);
    });
  } catch (e) {
    console.error("syncfs threw:", e);
  }
}

function showLoadError(msg) {
  const card = document.querySelector(".load-card");
  card.classList.add("error");
  $("load-msg").textContent = msg;
  $("load-sub").textContent = "rajsql.js not found. Build it with ./build.sh wasm, then reload this page.";
}

async function boot() {
  try {
    const createModule = (await import("./rajsql.js")).default;
    Module = await createModule();
  } catch (err) {
    console.error(err);
    showLoadError("Failed to load the RajSQL WebAssembly module.");
    return;
  }

  // Persistent, IndexedDB-backed filesystem.
  try {
    Module.FS.mkdir("/db");
  } catch (e) { /* already exists */ }
  Module.FS.mount(Module.FS.filesystems.IDBFS, {}, "/db");

  Module.FS.syncfs(true, (err) => {
    if (err) {
      console.error(err);
      showLoadError("Could not restore the database from IndexedDB.");
      return;
    }
    dbHandle = Module.ccall("rsql_open", "number", ["string"], ["/db/rajsql.db"]);
    if (!dbHandle) {
      showLoadError("rsql_open failed: the module could not open /db/rajsql.db.");
      return;
    }
    markPersisted();
    refreshSchema();
    $("load-overlay").hidden = true;
  });
}

/* ------------------------------------------------------------------ */
/* Exec & rendering                                                    */
/* ------------------------------------------------------------------ */

/** Run SQL through the engine. Returns parsed JSON array of statements. */
function execSQL(sql) {
  const raw = Module.ccall("rsql_exec", "string", ["number", "string"], [dbHandle, sql]);
  let parsed;
  try {
    parsed = JSON.parse(raw);
  } catch (e) {
    throw new Error("Engine returned invalid JSON: " + String(raw).slice(0, 200));
  }
  return Array.isArray(parsed) ? parsed : [parsed];
}

function valueCell(v) {
  if (v === null || v === undefined) return `<td class="null-cell">NULL</td>`;
  if (typeof v === "number") return `<td class="num-cell">${esc(v)}</td>`;
  return `<td>${esc(v)}</td>`;
}

function renderStatement(st) {
  if (!st.ok) {
    return `<div class="result-block">
      <div class="result-status err">✕ ${esc(st.error || "unknown error")}</div>
    </div>`;
  }
  const time = typeof st.ms === "number" ? ` · ${st.ms.toFixed(2)} ms` : "";
  const nRows = st.rows ? st.rows.length : 0;

  if (st.columns && st.rows) {
    const head = st.columns.map((c) => `<th>${esc(c)}</th>`).join("");
    const body = st.rows
      .map((row) => `<tr>${row.map(valueCell).join("")}</tr>`)
      .join("");
    return `<div class="result-block">
      <div class="result-status ok">✓ ok<span class="status-time">${time} · ${nRows} row${nRows === 1 ? "" : "s"}</span></div>
      <div class="result-scroll">
        <table class="result-table">
          <thead><tr>${head}</tr></thead>
          <tbody>${body}</tbody>
        </table>
      </div>
    </div>`;
  }

  // Non-SELECT: message + rowsAffected
  const affected = typeof st.rowsAffected === "number" ? ` · ${st.rowsAffected} row${st.rowsAffected === 1 ? "" : "s"} affected` : "";
  return `<div class="result-block">
    <div class="result-status ok">✓ ok<span class="status-time">${time}${affected}</span></div>
    <div class="result-message">${esc(st.message || "statement executed")}</div>
  </div>`;
}

function renderResults(statements) {
  $("results").innerHTML = statements.map(renderStatement).join("");
}

function runSQL() {
  const sql = editor.value.trim();
  if (!sql) return;
  try {
    const results = execSQL(sql);
    renderResults(results);
    pushHistory(sql);
    hideCrash();
    // Persist if any statement succeeded and might have written.
    if (results.some((r) => r.ok)) {
      markDirty();
      persistNow();
    }
    refreshSchema();
  } catch (err) {
    $("results").innerHTML =
      `<div class="result-status err">✕ ${esc(err.message)}</div>`;
    refreshSchema();
  }
}

/* ------------------------------------------------------------------ */
/* Schema sidebar                                                      */
/* ------------------------------------------------------------------ */

let openTables = new Set();

function refreshSchema() {
  let schema;
  try {
    schema = JSON.parse(Module.ccall("rsql_schema", "string", ["number"], [dbHandle]));
  } catch (e) {
    console.error(e);
    return;
  }
  const container = $("schema-list");
  const tables = schema.tables || [];
  // Keep expansion state only for tables that still exist.
  const names = new Set(tables.map((t) => t.name));
  openTables = new Set([...openTables].filter((n) => names.has(n)));

  if (tables.length === 0) {
    container.innerHTML = `<div class="schema-empty muted">No tables yet. Load the sample database or run a CREATE TABLE.</div>`;
    return;
  }

  container.innerHTML = tables.map((t) => {
    const open = openTables.has(t.name);
    const cols = open
      ? `<div class="schema-columns">${t.columns
          .map((c) => `<div class="schema-col"><span>${esc(c.name)}</span><span class="schema-col-type">${esc(c.type)}</span></div>`)
          .join("")}</div>`
      : "";
    return `<div class="schema-table${open ? " open" : ""}" data-table="${esc(t.name)}">
      <div class="schema-table-head" data-action="toggle">
        <span class="schema-table-name">${esc(t.name)}</span>
        <span class="schema-rowcount">${t.rows} row${t.rows === 1 ? "" : "s"}</span>
      </div>
      ${cols}
    </div>`;
  }).join("");
}

// One delegated listener for expand/collapse (no per-row listeners to leak).
$("schema-list").addEventListener("click", (e) => {
  const head = e.target.closest("[data-action='toggle']");
  if (!head) return;
  const name = head.parentElement.dataset.table;
  if (openTables.has(name)) openTables.delete(name);
  else openTables.add(name);
  refreshSchema();
});

/* ------------------------------------------------------------------ */
/* Crash & recover demo                                                */
/* ------------------------------------------------------------------ */

function hideCrash() {
  $("crash-panel").hidden = true;
}

function runCrashDemo() {
  try {
    const result = JSON.parse(
      Module.ccall("rsql_crash_demo", "string", ["number"], [dbHandle])
    );
    const steps = (result.steps || []).map((s, i) => {
      const isCrash = /crash|power loss/i.test(s);
      return `<div class="crash-step${isCrash ? " crash-event" : ""}">
        <div class="crash-step-num">${i + 1}</div>
        <div>${esc(s)}</div>
      </div>`;
    }).join("");
    $("crash-timeline").innerHTML = steps;

    const recovered = result.recovered === true;
    const match = result.rowsBeforeCrash === result.rowsAfterRecovery;
    const good = recovered && match;
    const summary = $("crash-summary");
    summary.className = "crash-summary " + (good ? "ok" : "bad");
    summary.textContent = good
      ? `Recovered: ${result.rowsAfterRecovery}/${result.rowsBeforeCrash} rows`
      : `Data loss: ${result.rowsAfterRecovery}/${result.rowsBeforeCrash} rows after recovery`;
    $("crash-panel").hidden = false;
    $("crash-panel").scrollIntoView({ behavior: "smooth", block: "nearest" });

    refreshSchema();
    markDirty();
    persistNow();
  } catch (err) {
    $("results").innerHTML =
      `<div class="result-status err">✕ Crash demo failed: ${esc(err.message)}</div>`;
  }
}

/* ------------------------------------------------------------------ */
/* Sample database / reset                                             */
/* ------------------------------------------------------------------ */

function loadSample() {
  // The engine has no IF EXISTS: try dropping first and ignore failures.
  for (const stmt of SAMPLE_DROPS.split(";")) {
    if (stmt.trim()) {
      try { execSQL(stmt + ";"); } catch (e) { /* table may not exist */ }
    }
  }
  try {
    const results = execSQL(SAMPLE_SQL);
    renderResults(results);
    markDirty();
    persistNow();
    openTables = new Set(["students", "courses", "enrollments"]);
    refreshSchema();
  } catch (err) {
    $("results").innerHTML =
      `<div class="result-status err">✕ ${esc(err.message)}</div>`;
  }
}

function resetDatabase() {
  if (!confirm("Drop all tables and reset the database? This cannot be undone.")) return;
  let schema;
  try {
    schema = JSON.parse(Module.ccall("rsql_schema", "string", ["number"], [dbHandle]));
  } catch (e) {
    console.error(e);
    return;
  }
  for (const t of schema.tables || []) {
    try { execSQL(`DROP TABLE ${t.name};`); } catch (e) { /* ignore */ }
  }
  $("results").innerHTML =
    `<div class="result-status ok">✓ ok<span class="status-time"> · database reset</span></div>`;
  hideCrash();
  markDirty();
  persistNow();
  refreshSchema();
}

/* ------------------------------------------------------------------ */
/* Wire up static UI                                                   */
/* ------------------------------------------------------------------ */

$("btn-run").addEventListener("click", runSQL);

$("examples").innerHTML = EXAMPLES.map(([label, sql], i) =>
  `<button class="example-btn" data-example="${i}">${esc(label)}</button>`
).join("");

$("examples").addEventListener("click", (e) => {
  const btn = e.target.closest("[data-example]");
  if (!btn) return;
  const [, sql] = EXAMPLES[Number(btn.dataset.example)];
  editor.value = sql;
  historyIndex = -1;
  renderHighlight();
  editor.focus();
});

$("btn-sample").addEventListener("click", loadSample);
$("btn-crash").addEventListener("click", runCrashDemo);
$("btn-persist").addEventListener("click", () => persistNow());
$("btn-reset").addEventListener("click", resetDatabase);

// Docs modal
const docsModal = $("docs-modal");
$("btn-docs").addEventListener("click", () => { docsModal.hidden = false; });
$("btn-docs-close").addEventListener("click", () => { docsModal.hidden = true; });
docsModal.addEventListener("click", (e) => {
  if (e.target === docsModal) docsModal.hidden = true;
});
document.addEventListener("keydown", (e) => {
  if (e.key === "Escape" && !docsModal.hidden) docsModal.hidden = true;
});

// Best-effort flush on unload (async IndexedDB write may not finish, hence best-effort).
window.addEventListener("beforeunload", () => {
  if (persistDirty) persistNow();
});

/* ------------------------------------------------------------------ */

renderHighlight(); // draw placeholder highlighting before boot
boot();
