import { spawn } from "node:child_process";
import { readFile } from "node:fs/promises";
import path from "node:path";
import process from "node:process";
import { fileURLToPath, pathToFileURL } from "node:url";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const workspaceDir = path.resolve(scriptDir, "../workspace");
const bufPath = process.argv[2] ?? process.env.BUF ?? "buf";
const servicePath = path.join(workspaceDir, "acme/v1/service.proto");
const typesPath = path.join(workspaceDir, "acme/v1/types.proto");
const serviceUri = pathToFileURL(servicePath).href;
const typesUri = pathToFileURL(typesPath).href;

const child = spawn(bufPath, ["lsp", "serve"], {
  cwd: workspaceDir,
  stdio: ["pipe", "pipe", "pipe"],
});

let receiveBuffer = Buffer.alloc(0);
let nextRequestId = 1;
let stderr = "";
const pendingRequests = new Map();
const notifications = [];
const notificationWaiters = [];

child.stderr.setEncoding("utf8");
child.stderr.on("data", (chunk) => {
  stderr += chunk;
});

child.stdout.on("data", (chunk) => {
  receiveBuffer = Buffer.concat([receiveBuffer, chunk]);
  drainMessages();
});

function drainMessages() {
  while (true) {
    const headerEnd = receiveBuffer.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
      return;
    }
    const headers = receiveBuffer.subarray(0, headerEnd).toString("ascii");
    const lengthMatch = /^Content-Length:\s*(\d+)$/im.exec(headers);
    if (!lengthMatch) {
      throw new Error(`missing Content-Length header: ${headers}`);
    }
    const bodyLength = Number(lengthMatch[1]);
    const bodyStart = headerEnd + 4;
    if (receiveBuffer.length < bodyStart + bodyLength) {
      return;
    }
    const body = receiveBuffer
      .subarray(bodyStart, bodyStart + bodyLength)
      .toString("utf8");
    receiveBuffer = receiveBuffer.subarray(bodyStart + bodyLength);
    dispatch(JSON.parse(body));
  }
}

function dispatch(message) {
  if (Object.hasOwn(message, "id")) {
    const waiter = pendingRequests.get(message.id);
    if (!waiter) {
      return;
    }
    pendingRequests.delete(message.id);
    if (message.error) {
      waiter.reject(new Error(JSON.stringify(message.error)));
    } else {
      waiter.resolve(message.result);
    }
    return;
  }

  for (let index = 0; index < notificationWaiters.length; index += 1) {
    const waiter = notificationWaiters[index];
    if (waiter.method === message.method && waiter.predicate(message.params)) {
      notificationWaiters.splice(index, 1);
      clearTimeout(waiter.timer);
      waiter.resolve(message.params);
      return;
    }
  }
  notifications.push(message);
}

function send(message) {
  const body = JSON.stringify({ jsonrpc: "2.0", ...message });
  child.stdin.write(`Content-Length: ${Buffer.byteLength(body)}\r\n\r\n${body}`);
}

function request(method, params, timeoutMs = 10_000) {
  const id = nextRequestId;
  nextRequestId += 1;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      pendingRequests.delete(id);
      reject(new Error(`timeout waiting for ${method}`));
    }, timeoutMs);
    pendingRequests.set(id, {
      resolve: (value) => {
        clearTimeout(timer);
        resolve(value);
      },
      reject: (error) => {
        clearTimeout(timer);
        reject(error);
      },
    });
    send({ id, method, params });
  });
}

function notify(method, params) {
  send({ method, params });
}

function waitForNotification(method, predicate, timeoutMs = 10_000) {
  const queuedIndex = notifications.findIndex(
    (message) => message.method === method && predicate(message.params),
  );
  if (queuedIndex >= 0) {
    const [message] = notifications.splice(queuedIndex, 1);
    return Promise.resolve(message.params);
  }

  return new Promise((resolve, reject) => {
    const waiter = {
      method,
      predicate,
      resolve,
      timer: setTimeout(() => {
        const index = notificationWaiters.indexOf(waiter);
        if (index >= 0) {
          notificationWaiters.splice(index, 1);
        }
        reject(new Error(`timeout waiting for ${method}`));
      }, timeoutMs),
    };
    notificationWaiters.push(waiter);
  });
}

function capabilityNames(capabilities) {
  return Object.entries(capabilities)
    .filter(([, value]) => value !== false && value !== null)
    .map(([name]) => name)
    .sort();
}

function conciseDiagnostics(params) {
  return params.diagnostics.map((diagnostic) => ({
    message: diagnostic.message,
    source: diagnostic.source ?? null,
    code: diagnostic.code ?? null,
    severity: diagnostic.severity ?? null,
    range: diagnostic.range,
  }));
}

async function main() {
  const rootUri = pathToFileURL(`${workspaceDir}${path.sep}`).href;
  const initializeResult = await request("initialize", {
    processId: process.pid,
    clientInfo: { name: "recallfs-protobuf-lsp-probe", version: "1.0.0" },
    rootUri,
    workspaceFolders: [{ uri: rootUri, name: "protobuf-lsp-demo" }],
    capabilities: {
      workspace: { workspaceFolders: true },
      textDocument: {
        completion: {},
        definition: {},
        hover: {},
        publishDiagnostics: { relatedInformation: true },
      },
    },
  });
  notify("initialized", {});

  const [typesText, serviceText] = await Promise.all([
    readFile(typesPath, "utf8"),
    readFile(servicePath, "utf8"),
  ]);

  notify("textDocument/didOpen", {
    textDocument: {
      uri: typesUri,
      languageId: "proto",
      version: 1,
      text: typesText,
    },
  });
  notify("textDocument/didOpen", {
    textDocument: {
      uri: serviceUri,
      languageId: "proto",
      version: 1,
      text: serviceText,
    },
  });

  const initialDiagnostics = await waitForNotification(
    "textDocument/publishDiagnostics",
    (params) => params.uri === serviceUri,
  );
  const definition = await request("textDocument/definition", {
    textDocument: { uri: serviceUri },
    position: { line: 11, character: 4 },
  });

  const invalidText = serviceText.replace(
    "Customer customer = 1;",
    "repeated repeated Customer customer = 1;",
  );
  const invalidStarted = process.hrtime.bigint();
  notify("textDocument/didChange", {
    textDocument: { uri: serviceUri, version: 2 },
    contentChanges: [{ text: invalidText }],
  });
  const invalidDiagnostics = await waitForNotification(
    "textDocument/publishDiagnostics",
    (params) => params.uri === serviceUri && params.diagnostics.length > 0,
  );
  const invalidLatencyMs =
    Number(process.hrtime.bigint() - invalidStarted) / 1_000_000;

  const repairStarted = process.hrtime.bigint();
  notify("textDocument/didChange", {
    textDocument: { uri: serviceUri, version: 3 },
    contentChanges: [{ text: serviceText }],
  });
  const repairedDiagnostics = await waitForNotification(
    "textDocument/publishDiagnostics",
    (params) => params.uri === serviceUri && params.diagnostics.length === 0,
  );
  const repairLatencyMs =
    Number(process.hrtime.bigint() - repairStarted) / 1_000_000;

  const result = {
    serverInfo: initializeResult.serverInfo,
    advertisedCapabilities: capabilityNames(initializeResult.capabilities),
    initialDiagnosticCount: initialDiagnostics.diagnostics.length,
    definition,
    unsavedInvalidEdit: {
      diagnosticLatencyMs: Number(invalidLatencyMs.toFixed(3)),
      diagnostics: conciseDiagnostics(invalidDiagnostics),
    },
    unsavedRepair: {
      diagnosticLatencyMs: Number(repairLatencyMs.toFixed(3)),
      diagnosticCount: repairedDiagnostics.diagnostics.length,
    },
  };
  process.stdout.write(`${JSON.stringify(result, null, 2)}\n`);
  child.stdin.end();
  setTimeout(() => child.kill("SIGKILL"), 100).unref();
  await new Promise((resolve) => child.once("close", resolve));
}

main().catch((error) => {
  child.kill("SIGTERM");
  process.stderr.write(`${error.stack ?? error}\n${stderr}`);
  process.exitCode = 1;
});
