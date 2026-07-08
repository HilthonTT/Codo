"use strict";

// Small helpers -------------------------------------------------------------

const $ = (id) => document.getElementById(id);

// Update the shared "CORS header" readout from a fetch Response. The server's
// CORS middleware only emits Access-Control-Allow-Origin when the request
// carried an Origin header, so same-origin GETs (which browsers send without an
// Origin) will show nothing — POSTs and cross-origin calls populate it.
function showCors(res) {
  const value = res.headers.get("access-control-allow-origin");
  const span = $("cors-line").querySelector("span");
  if (value) {
    span.textContent = value;
    span.style.color = "";
  } else {
    span.textContent = "(no Origin on this request)";
    span.style.color = "var(--muted)";
  }
}

function pretty(obj) {
  return JSON.stringify(obj, null, 2);
}

// Status / stats ------------------------------------------------------------

async function refreshStatus() {
  const out = $("status-out");
  try {
    const [statusRes, statsRes] = await Promise.all([
      fetch("/api/status"),
      fetch("/api/stats"),
    ]);
    showCors(statusRes);
    const status = await statusRes.json();
    const stats = await statsRes.json();
    out.textContent = pretty({ status, stats });
  } catch (err) {
    out.textContent = "Error: " + err;
  }
}

// Hello / echo --------------------------------------------------------------

async function doHello() {
  const out = $("hello-out");
  try {
    const res = await fetch("/api/hello");
    showCors(res);
    out.textContent = pretty(await res.json());
  } catch (err) {
    out.textContent = "Error: " + err;
  }
}

async function doEcho() {
  const out = $("hello-out");
  const text = $("echo-in").value;
  try {
    // A POST carries an Origin header even same-origin, so this response is the
    // reliable way to see the CORS header populate.
    const res = await fetch("/api/echo", {
      method: "POST",
      headers: { "Content-Type": "text/plain" },
      body: text,
    });
    showCors(res);
    out.textContent = "echoed: " + (await res.text());
  } catch (err) {
    out.textContent = "Error: " + err;
  }
}

// Todos ---------------------------------------------------------------------

async function refreshTodos() {
  const list = $("todo-list");
  list.innerHTML = "";
  try {
    const res = await fetch("/api/todos");
    showCors(res);
    const todos = await res.json();
    if (!Array.isArray(todos) || todos.length === 0) {
      const li = document.createElement("li");
      li.className = "empty";
      li.textContent = "No todos yet.";
      list.appendChild(li);
      return;
    }
    for (const todo of todos) {
      list.appendChild(renderTodo(todo));
    }
  } catch (err) {
    const li = document.createElement("li");
    li.className = "empty";
    li.textContent = "Error: " + err;
    list.appendChild(li);
  }
}

function renderTodo(todo) {
  const li = document.createElement("li");
  if (todo.completed) {
    li.classList.add("done");
  }

  const toggle = document.createElement("input");
  toggle.type = "checkbox";
  toggle.checked = !!todo.completed;
  toggle.addEventListener("change", () => toggleTodo(todo));

  const title = document.createElement("span");
  title.className = "title";
  title.textContent = todo.title + "  #" + todo.id;

  const del = document.createElement("button");
  del.className = "del";
  del.textContent = "Delete";
  del.addEventListener("click", () => deleteTodo(todo.id));

  li.append(toggle, title, del);
  return li;
}

async function addTodo() {
  const input = $("todo-title");
  const title = input.value.trim();
  if (!title) {
    return;
  }
  try {
    const res = await fetch("/api/todos", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ title, completed: false }),
    });
    showCors(res);
    input.value = "";
    await refreshTodos();
  } catch (err) {
    console.error(err);
  }
}

async function toggleTodo(todo) {
  try {
    const res = await fetch("/api/todos/" + todo.id, {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ title: todo.title, completed: !todo.completed }),
    });
    showCors(res);
    await refreshTodos();
  } catch (err) {
    console.error(err);
  }
}

async function deleteTodo(id) {
  try {
    const res = await fetch("/api/todos/" + id, { method: "DELETE" });
    showCors(res);
    await refreshTodos();
  } catch (err) {
    console.error(err);
  }
}

// WebSocket echo ------------------------------------------------------------

let socket = null;

function wsLog(line) {
  const log = $("ws-log");
  if (log.textContent === "—") {
    log.textContent = "";
  }
  log.textContent += line + "\n";
  log.scrollTop = log.scrollHeight;
}

function setWsConnected(connected) {
  const badge = $("ws-state");
  badge.textContent = connected ? "connected" : "disconnected";
  badge.className = "badge " + (connected ? "on" : "off");
  $("ws-connect").disabled = connected;
  $("ws-disconnect").disabled = !connected;
  $("ws-in").disabled = !connected;
  $("ws-send").disabled = !connected;
}

function wsConnect() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  const url = proto + "://" + location.host + "/ws/chat";
  wsLog("connecting to " + url + " ...");
  socket = new WebSocket(url);

  socket.addEventListener("open", () => {
    setWsConnected(true);
    wsLog("[open]");
  });
  socket.addEventListener("message", (ev) => {
    wsLog("<= " + ev.data);
  });
  socket.addEventListener("close", (ev) => {
    setWsConnected(false);
    wsLog("[close] code=" + ev.code);
    socket = null;
  });
  socket.addEventListener("error", () => {
    wsLog("[error]");
  });
}

function wsDisconnect() {
  if (socket) {
    socket.close(1000, "bye");
  }
}

function wsSend() {
  const input = $("ws-in");
  const text = input.value;
  if (!socket || socket.readyState !== WebSocket.OPEN || !text) {
    return;
  }
  socket.send(text);
  wsLog("=> " + text);
  input.value = "";
}

// Wiring --------------------------------------------------------------------

window.addEventListener("DOMContentLoaded", () => {
  $("refresh-status").addEventListener("click", refreshStatus);
  $("hello-btn").addEventListener("click", doHello);
  $("echo-btn").addEventListener("click", doEcho);
  $("echo-in").addEventListener("keydown", (e) => {
    if (e.key === "Enter") doEcho();
  });

  $("todo-add").addEventListener("click", addTodo);
  $("todo-refresh").addEventListener("click", refreshTodos);
  $("todo-title").addEventListener("keydown", (e) => {
    if (e.key === "Enter") addTodo();
  });

  $("ws-connect").addEventListener("click", wsConnect);
  $("ws-disconnect").addEventListener("click", wsDisconnect);
  $("ws-send").addEventListener("click", wsSend);
  $("ws-in").addEventListener("keydown", (e) => {
    if (e.key === "Enter") wsSend();
  });

  refreshStatus();
  refreshTodos();
});
