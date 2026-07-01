/* ESPD device console — HTTPS + WSS /ws (monitor + Pd + folder sync). */
'use strict'

const STATUS_RE = /^\+OK STATUS sdcard=(yes|no) internal=(yes|no)$/
const PUT_DONE_RE = /^\+OK PUT done ([0-9a-fA-F]{8})$/
const PUT_ACK_RE = /^\+OK PUT ack (\d+)$/
const PUT_READY_RE = /^\+OK PUT ready window=(\d+)$/
const LIST_DONE_RE = /^\+OK LIST done (\d+)$/
const DEBOUNCE_MS = 350
const LOG_MAX_LINES = 1000
const CONNECT_OPEN_MS = 8000
const RECONNECT_OPEN_MS = 4000
const RECONNECT_DELAY_MS = 600
const RECONNECT_BOOT_WAIT_MS = 2500
const RECONNECT_ATTEMPTS = 24

const sleep = ms => new Promise(r => setTimeout(r, ms))

const $ = id => document.getElementById(id)
const logEl = $('log')
const statusEl = $('status')

let client = null
let connectPromise = null
let suppressDisconnectReconnect = false
let syncDirHandle = null
let syncMtimes = new Map()
let syncObserver = null
let syncDebounceTimer = null
let watchWanted = false
let watchPaused = false
let busy = false

function requireBrowserApis() {
  const missing = []
  if (!window.showDirectoryPicker) missing.push('showDirectoryPicker')
  if (!('FileSystemObserver' in window)) missing.push('FileSystemObserver')
  if (missing.length) {
    throw new Error(`Unsupported browser — need Chrome or Edge (${missing.join(', ')})`)
  }
}

function log(msg, cls = '') {
  const line = document.createElement('div')
  if (cls) line.className = cls
  line.textContent = msg
  logEl.appendChild(line)
  while (logEl.children.length > LOG_MAX_LINES) logEl.removeChild(logEl.firstChild)
  logEl.scrollTop = logEl.scrollHeight
}

const FOLDER_SYNC_OK =
  window.isSecureContext && typeof window.showDirectoryPicker === 'function'
const FOLDER_WATCH_OK =
  FOLDER_SYNC_OK && 'FileSystemObserver' in window

function setConnected(on) {
  const live = on && client && client._ioAlive()
  $('connect-btn').disabled = live || busy || !!connectPromise
  $('disconnect-btn').disabled = !live || busy
  $('pd-input').disabled = !live || busy
  $('pd-btn').disabled = !live || busy
  $('reload-btn').disabled = !live || busy
  $('reset-btn').disabled = !live || busy
  $('sync-btn').disabled = !live || busy || !syncDirHandle
  /* Folder pick does not require an open socket (runSync connects). */
  $('folder-btn').disabled = busy || !FOLDER_SYNC_OK
}

function applyConnectedState(info) {
  if (!client || !client._ioAlive()) {
    setConnected(false)
    return
  }
  setConnected(true)
  if (info) {
    statusEl.textContent = `Connected · sync target ${syncStorePath(info)}`
  } else if (watchWanted && syncDirHandle) {
    setWatchingStatus()
  } else {
    statusEl.textContent = 'Connected'
  }
}

function wsUrl() {
  return `wss://${location.host}/ws`
}

function devPathOk(rel) {
  if (!rel || rel.startsWith('/') || rel.includes('\\')) return false
  if (rel.split('/').some(p => !p || p.startsWith('.') || p.includes('..'))) return false
  if (rel.length >= 384) return false
  return /^[\x20-\x7e]+$/.test(rel)
}

function crc32Bytes(data) {
  let crc = 0xffffffff
  for (let i = 0; i < data.length; i++) {
    crc ^= data[i]
    for (let j = 0; j < 8; j++) crc = (crc >>> 1) ^ (crc & 1 ? 0xedb88320 : 0)
  }
  return (crc ^ 0xffffffff) >>> 0
}

function parseStatus(line) {
  const m = String(line).trim().match(STATUS_RE)
  if (!m) throw new Error(`unexpected STATUS: ${line}`)
  return { sdcard: m[1], internal: m[2] }
}

function syncStorePath(info) {
  return info.sdcard === 'yes' ? '/sdcard' : '/storage'
}

function matchDevReply(verb, line) {
  switch (verb) {
    case 'STATUS':
      return line.startsWith('+OK STATUS')
    case 'MSG':
      return line.startsWith('+OK MSG') || line.startsWith('-ERR MSG')
    case 'RELOAD':
      return line.startsWith('+OK RELOAD') || line.startsWith('-ERR')
    case 'RESET':
      return line.startsWith('+OK RESET')
    case 'RM':
      return line.startsWith('+OK RM') || line.startsWith('-ERR')
    case 'PUT':
      return line.startsWith('+OK PUT') || line.startsWith('-ERR')
    default:
      return line.startsWith('+OK') || line.startsWith('-ERR')
  }
}

function syncIsSnapshot(src) {
  return src && src.type === 'snapshot'
}

async function collectSyncFiles(dirHandle, prefix = '') {
  if (syncIsSnapshot(dirHandle)) {
    return [...dirHandle.files.keys()].filter(rel => {
      if (prefix && !rel.startsWith(prefix)) return false
      return devPathOk(rel)
    })
  }
  const out = []
  for await (const [name, handle] of dirHandle.entries()) {
    if (name.startsWith('.')) continue
    const rel = prefix + name
    if (handle.kind === 'file') out.push(rel)
    else if (handle.kind === 'directory') out.push(...await collectSyncFiles(handle, rel + '/'))
  }
  return out
}

async function readFileBytes(dirHandle, rel) {
  if (syncIsSnapshot(dirHandle)) {
    const file = dirHandle.files.get(rel)
    if (!file) throw new Error(`missing file ${rel}`)
    return new Uint8Array(await file.arrayBuffer())
  }
  const parts = rel.split('/')
  let h = dirHandle
  for (let i = 0; i < parts.length - 1; i++) h = await h.getDirectoryHandle(parts[i])
  const file = await (await h.getFileHandle(parts[parts.length - 1])).getFile()
  return new Uint8Array(await file.arrayBuffer())
}

async function collectSyncMtimes(dirHandle, prefix = '') {
  if (syncIsSnapshot(dirHandle)) {
    const out = new Map()
    for (const [rel, file] of dirHandle.files) {
      if (prefix && !rel.startsWith(prefix)) continue
      out.set(rel, file.lastModified)
    }
    return out
  }
  const out = new Map()
  for await (const [name, handle] of dirHandle.entries()) {
    if (name.startsWith('.')) continue
    const rel = prefix + name
    if (handle.kind === 'file') {
      const file = await handle.getFile()
      out.set(rel, file.lastModified)
    } else if (handle.kind === 'directory') {
      for (const [k, v] of await collectSyncMtimes(handle, rel + '/')) out.set(k, v)
    }
  }
  return out
}


function stopSyncWatch() {
  if (syncObserver) {
    try { syncObserver.disconnect() } catch (_) {}
    syncObserver = null
  }
  if (syncDebounceTimer) {
    clearTimeout(syncDebounceTimer)
    syncDebounceTimer = null
  }
}

async function refreshSyncMtimes() {
  if (!syncDirHandle) return
  syncMtimes = await collectSyncMtimes(syncDirHandle)
}

async function collectChangedRels() {
  if (!syncDirHandle) return []
  const current = await collectSyncMtimes(syncDirHandle)
  const changed = []
  for (const [rel, mtime] of current) {
    if (syncMtimes.get(rel) !== mtime) changed.push(rel)
  }
  return changed
}

function setWatchingStatus() {
  if (!client?._ioAlive() || !syncDirHandle || !watchWanted) return
  const n = syncMtimes.size
  statusEl.textContent = `Connected · watching ${n} file${n === 1 ? '' : 's'}`
}

class EspdWsClient {
  constructor(callbacks = {}) {
    this.onLine = callbacks.onLine || (() => {})
    this.onLog = callbacks.onLog || (() => {})
    this.onDisconnect = callbacks.onDisconnect || (() => {})
    this.ws = null
    this.stopped = false
    this.disconnected = false
    this.putActive = false
    this.listActive = false
    this.listPaths = []
    this.pendingReply = null
    this._replyAccept = null
    this._pendingVerb = null
    this.lastStatus = null
    this._buf = new Uint8Array(0)
  }

  _appendBytes(chunk) {
    const out = new Uint8Array(this._buf.length + chunk.length)
    out.set(this._buf)
    out.set(chunk, this._buf.length)
    this._buf = out
  }

  _markDisconnected() { this.disconnected = true }

  _ioAlive() {
    return !this.disconnected && !this.stopped && this.ws && this.ws.readyState === WebSocket.OPEN
  }

  _resolveReply(line) {
    if (!this.pendingReply) return
    if (this._replyAccept && !this._replyAccept(line)) {
      this.onLine(line, line.startsWith('+') || line.startsWith('-ERR') ? 'dev' : 'device')
      return
    }
    const fn = this.pendingReply
    this.pendingReply = null
    this._replyAccept = null
    this._pendingVerb = null
    fn(line)
  }

  _processLines() {
    const dec = new TextDecoder()
    let text = dec.decode(this._buf)
    let idx
    while ((idx = text.indexOf('\n')) !== -1) {
      const raw = text.slice(0, idx)
      text = text.slice(idx + 1)
      const line = raw.replace(/\r$/, '').trim()
      if (!line) continue
      if (this.putActive) {
        if (line.startsWith('+OK PUT ack')) this._resolveReply(line)
        else if (line.startsWith('+OK PUT done') || line.startsWith('-ERR')) {
          this.onLine(line, 'dev')
          this._resolveReply(line)
        }
        continue
      }
      if (this.listActive) {
        if (line.startsWith('+FILE ')) {
          const rel = line.slice(6)
          if (devPathOk(rel)) this.listPaths.push(rel)
          this.onLine(line, 'dev')
          continue
        }
        if (line.startsWith('+OK LIST done') || line.startsWith('-ERR')) {
          this.onLine(line, 'dev')
          this._resolveReply(line)
        } else if (line.startsWith('+OK LIST')) {
          this.onLine(line, 'dev')
        }
        continue
      }
      if (line.startsWith('+') || line.startsWith('-ERR')) {
        this.onLine(line, 'dev')
        if (this.pendingReply) this._resolveReply(line)
      } else {
        this.onLine(line, 'device')
      }
    }
    this._buf = new TextEncoder().encode(text)
  }

  _onMessage(ev) {
    const chunk = ev.data instanceof ArrayBuffer
      ? new Uint8Array(ev.data)
      : new TextEncoder().encode(String(ev.data))
    this._appendBytes(chunk)
    this._processLines()
  }

  open(connectTimeoutMs = CONNECT_OPEN_MS) {
    return new Promise((resolve, reject) => {
      this.stopped = false
      this.disconnected = false
      const ws = new WebSocket(wsUrl())
      ws.binaryType = 'arraybuffer'
      this.ws = ws
      let settled = false
      const finish = (fn, arg) => {
        if (settled) return
        settled = true
        clearTimeout(t)
        fn(arg)
      }
      const t = setTimeout(() => {
        try { ws.close() } catch (_) {}
        finish(reject, new Error('WebSocket connect timeout'))
      }, connectTimeoutMs)
      ws.onopen = () => {
        this._buf = new Uint8Array(0)
        finish(resolve)
      }
      ws.onerror = () => finish(reject, new Error(
        'WebSocket error — accept the certificate warning, reload, and try again',
      ))
      ws.onclose = () => {
        if (!this.stopped) {
          this._markDisconnected()
          this.onDisconnect()
        }
      }
      ws.onmessage = ev => this._onMessage(ev)
    })
  }

  async close() {
    this.stopped = true
    this.disconnected = true
    this.pendingReply = null
    this._replyAccept = null
    this._pendingVerb = null
    if (this.ws) {
      try { this.ws.close() } catch (_) {}
      this.ws = null
    }
  }

  _waitReply(timeoutMs) {
    return new Promise((resolve, reject) => {
      const t = setTimeout(() => {
        this.pendingReply = null
        this._replyAccept = null
        this._pendingVerb = null
        reject(new Error(this._ioAlive() ? 'device reply timeout' : 'disconnected'))
      }, timeoutMs)
      this.pendingReply = line => {
        clearTimeout(t)
        resolve(line)
      }
    })
  }

  async _writeBytes(data) {
    if (!this._ioAlive()) throw new Error('disconnected')
    this.ws.send(data)
  }

  async _sendAndWait(data, timeoutMs) {
    const replyPromise = this._waitReply(timeoutMs)
    await this._writeBytes(data)
    return replyPromise
  }

  async command(text, timeoutMs = 10000) {
    if (!this._ioAlive()) throw new Error('disconnected')
    this._buf = new Uint8Array(0)
    const verb = text.trim().split(/\s+/)[0]
    this._pendingVerb = verb
    this._replyAccept = line => matchDevReply(verb, line)
    this.onLog(`→ ${text.trim()}`, 'sync')
    const payload = new TextEncoder().encode(text.endsWith('\n') ? text : `${text}\n`)
    try {
      return await this._sendAndWait(payload, timeoutMs)
    } finally {
      this._replyAccept = null
      this._pendingVerb = null
    }
  }

  async status(timeoutMs = 10000) {
    const line = await this.command('STATUS', timeoutMs)
    if (line.startsWith('+OK STATUS')) {
      this.lastStatus = line
      return parseStatus(line)
    }
    throw new Error(line)
  }

  async reload() { return this.command('RELOAD', 30000) }

  async resetDevice() {
    try { await this.command('RESET', 2000) } catch (_) {}
    await this.close()
  }

  async sendPd(message) {
    const msg = message.trim()
    if (!msg) throw new Error('empty Pd message')
    const line = await this.command(`MSG ${msg}`, 5000)
    if (line.startsWith('-ERR')) throw new Error(line)
    return line
  }

  async listFiles(timeoutMs = 120000) {
    if (!this._ioAlive()) throw new Error('disconnected')
    this._buf = new Uint8Array(0)
    this.listPaths = []
    this.listActive = true
    try {
      this.onLog('→ LIST', 'sync')
      const line = await this._sendAndWait(
        new TextEncoder().encode('LIST\n'),
        timeoutMs,
      )
      if (line.startsWith('-ERR')) throw new Error(line)
      if (!LIST_DONE_RE.test(line.trim())) throw new Error(`unexpected LIST: ${line}`)
      return [...this.listPaths]
    } finally {
      this.listActive = false
    }
  }

  async rmFile(relPath, timeoutMs = 10000) {
    const line = await this.command(`RM ${relPath}`, timeoutMs)
    if (line.startsWith('-ERR')) throw new Error(line)
  }

  async putFile(relPath, data) {
    const crc = crc32Bytes(data)
    const nbytes = data.length
    const probeTimeout = Math.max(30000, nbytes / 40)
    const doneTimeout = Math.max(60000, nbytes / 8)
    const line = await this.command(
      `PUT ${relPath} ${nbytes} ${crc.toString(16).padStart(8, '0')}`,
      probeTimeout,
    )
    if (line.startsWith('+OK PUT skip')) return false
    const m = line.trim().match(PUT_READY_RE)
    if (!m) {
      if (line.startsWith('-ERR')) throw new Error(line)
      throw new Error(`unexpected PUT: ${line}`)
    }
    const putWindow = parseInt(m[1], 10)
    this.onLog(`sending ${nbytes} bytes for ${relPath}`, 'sync')
    this.putActive = true
    const ackTimeout = Math.max(30000, nbytes / 40)
    try {
      this.pendingReply = null
      for (let off = 0; off < data.length; off += putWindow) {
        const part = data.subarray(off, off + putWindow)
        const ackLine = await this._sendAndWait(part, ackTimeout)
        const am = ackLine.trim().match(PUT_ACK_RE)
        if (!am) {
          if (ackLine.startsWith('-ERR')) throw new Error(ackLine)
          throw new Error(`unexpected PUT ack: ${ackLine}`)
        }
        const acked = parseInt(am[1], 10)
        if (acked !== off + part.length) {
          throw new Error(`PUT ack mismatch: expected ${off + part.length}, got ${acked}`)
        }
      }
      const doneLine = await this._waitReply(doneTimeout)
      const dm = doneLine.trim().match(PUT_DONE_RE)
      if (dm && parseInt(dm[1], 16) === crc) return true
      if (doneLine.startsWith('-ERR')) throw new Error(doneLine)
      throw new Error(`unexpected PUT done: ${doneLine}`)
    } finally {
      this.putActive = false
    }
  }
}

async function connectClient({ openTimeoutMs = CONNECT_OPEN_MS } = {}) {
  if (client?._ioAlive()) return client
  if (connectPromise) return connectPromise

  const work = (async () => {
    statusEl.textContent = 'Connecting…'
    setConnected(false)

    const c = new EspdWsClient({
      onLine: (line, kind) => log(kind === 'dev' ? `← ${line}` : line, kind === 'dev' ? 'dev' : ''),
      onLog: (msg, cls) => log(msg, cls || 'sync'),
      onDisconnect: () => {
        if (client !== c) return
        setConnected(false)
        if (busy || suppressDisconnectReconnect) {
          statusEl.textContent = 'Connection lost — recovering…'
          return
        }
        client = null
        if (watchWanted) {
          statusEl.textContent = 'Disconnected — reconnecting…'
          reconnect()
            .then(() => {
              applyConnectedState(
                client?.lastStatus ? parseStatus(client.lastStatus) : null,
              )
              if (watchWanted && syncDirHandle && !busy) startSyncWatch()
            })
            .catch(() => {
              statusEl.textContent = 'Disconnected'
            })
        } else {
          statusEl.textContent = 'Disconnected'
        }
      },
    })

    await c.open(openTimeoutMs)
    client = c
    return await c.status()
  })()

  connectPromise = work.finally(() => {
    connectPromise = null
  })

  try {
    const info = await connectPromise
    applyConnectedState(info)
    return client
  } catch (e) {
    client = null
    setConnected(false)
    throw e
  }
}

async function reconnect({ afterReset = false } = {}) {
  suppressDisconnectReconnect = true
  try {
    if (client) {
      client.stopped = true
      await client.close().catch(() => {})
    }
    client = null
    setConnected(false)
    if (afterReset) {
      statusEl.textContent = 'Waiting for device reboot…'
      await sleep(RECONNECT_BOOT_WAIT_MS)
    }
    statusEl.textContent = 'Reconnecting…'
    for (let i = 0; i < RECONNECT_ATTEMPTS; i++) {
      try {
        return await connectClient({ openTimeoutMs: RECONNECT_OPEN_MS })
      } catch (_) {
        if ((i + 1) % 6 === 0) {
          statusEl.textContent = afterReset
            ? `Reconnecting after reset… (attempt ${i + 1})`
            : `Reconnecting… (attempt ${i + 1})`
        }
        await sleep(RECONNECT_DELAY_MS)
      }
    }
    throw new Error('reconnect failed')
  } finally {
    suppressDisconnectReconnect = false
  }
}

async function ensureClient() {
  if (client?._ioAlive()) return client
  return connectClient()
}

async function runSync(label, onlyRels = null, allowRetry = true) {
  if (!syncDirHandle || busy) return
  await ensureClient()
  if (!client) return
  busy = true
  watchPaused = true
  stopSyncWatch()
  setConnected(!!client)
  statusEl.textContent = 'Syncing…'
  try {
    let rels = onlyRels
    if (!rels) rels = await collectSyncFiles(syncDirHandle)
    if (!rels.length) {
      if (label !== 'watch') log(`${label}: nothing to send`, 'sync')
      return
    }
    log(`${label} (${rels.length} file${rels.length === 1 ? '' : 's'})`, 'sync')
    if (!onlyRels && $('mirror-cb').checked) {
      const onDevice = new Set(await client.listFiles())
      const keep = new Set(rels)
      for (const rel of [...onDevice].sort()) {
        if (keep.has(rel) || !devPathOk(rel)) continue
        log(`remove ${rel}`, 'sync')
        await client.rmFile(rel)
      }
    }
    let uploaded = 0
    let skipped = 0
    let resetNeeded = false
    let reloadNeeded = false
    for (const rel of rels) {
      const data = await readFileBytes(syncDirHandle, rel)
      const sent = await client.putFile(rel, data)
      if (sent) {
        uploaded++
        if (rel === 'config.txt') resetNeeded = true
        else reloadNeeded = true
      } else {
        skipped++
      }
    }
    if (resetNeeded) {
      log('RESET (config.txt)', 'sync')
      await client.resetDevice()
      client = await reconnect({ afterReset: true })
      applyConnectedState(await client.status().catch(() => null))
    } else if (reloadNeeded) {
      log('RELOAD', 'sync')
      await client.reload()
    }
    log(`sync done (${uploaded} uploaded, ${skipped} unchanged)`, 'sync')
    await refreshSyncMtimes()
    if (client?._ioAlive()) {
      await client.status().catch(() => {})
      applyConnectedState(client.lastStatus ? parseStatus(client.lastStatus) : null)
    }
  } catch (e) {
    const msg = String(e.message || e)
    log(`sync error: ${msg}`, 'sync')
    const retriable = /disconnected|reply timeout/i.test(msg)
    if (allowRetry && retriable) {
      try {
        statusEl.textContent = 'Reconnecting after sync drop…'
        client = await reconnect()
        applyConnectedState(await client.status().catch(() => null))
        busy = false
        watchPaused = false
        return runSync(label, onlyRels, false)
      } catch (_) {
        client = null
        setConnected(false)
        statusEl.textContent = 'Disconnected'
        return
      }
    }
    try {
      client = await reconnect()
      applyConnectedState(await client.status().catch(() => null))
    } catch (_) {
      client = null
      setConnected(false)
      statusEl.textContent = 'Disconnected'
    }
  } finally {
    if (busy) {
      busy = false
      watchPaused = false
      applyConnectedState(client?.lastStatus ? parseStatus(client.lastStatus) : null)
      if (watchWanted && client?._ioAlive()) await startSyncWatch()
    }
  }
}

async function startSyncWatch() {
  if (!syncDirHandle || syncIsSnapshot(syncDirHandle) || !FOLDER_WATCH_OK) return
  await refreshSyncMtimes()
  if (syncObserver) return
  syncObserver = new FileSystemObserver(async () => {
    if (watchPaused || busy || !watchWanted) return
    if (syncDebounceTimer) clearTimeout(syncDebounceTimer)
    syncDebounceTimer = setTimeout(async () => {
      const changed = await collectChangedRels()
      if (!changed.length || busy) return
      await runSync('watch', changed)
    }, DEBOUNCE_MS)
  })
  await syncObserver.observe(syncDirHandle, { recursive: true })
  $('watch-hint').textContent = 'Watching folder for saves.'
  setWatchingStatus()
}

async function pickFolder() {
  stopSyncWatch()
  if (!FOLDER_SYNC_OK) {
    throw new Error('Folder pick needs Chrome/Edge after accepting the certificate warning')
  }
  syncDirHandle = await window.showDirectoryPicker({ mode: 'read' })
  $('folder-name').textContent = syncDirHandle.name
  watchWanted = true
  if (FOLDER_WATCH_OK) {
    $('watch-hint').textContent = 'Watching folder for saves.'
  }
  await runSync('initial sync')
}

$('connect-btn').addEventListener('click', async () => {
  try {
    await connectClient()
  } catch (e) {
    log(`connect failed: ${e.message || e}`, 'sync')
    statusEl.textContent = 'Not connected'
    setConnected(false)
  }
})
$('disconnect-btn').addEventListener('click', async () => {
  watchWanted = false
  suppressDisconnectReconnect = true
  stopSyncWatch()
  if (client) {
    client.stopped = true
    await client.close().catch(() => {})
  }
  client = null
  connectPromise = null
  suppressDisconnectReconnect = false
  syncDirHandle = null
  syncMtimes.clear()
  $('folder-name').textContent = ''
  $('watch-hint').textContent = ''
  statusEl.textContent = 'Not connected'
  setConnected(false)
})

$('folder-btn').addEventListener('click', async () => {
  try {
    await pickFolder()
  } catch (e) {
    if (e.name !== 'AbortError') log(`folder pick failed: ${e.message || e}`, 'sync')
  }
})
$('sync-btn').addEventListener('click', () => runSync('sync now'))

$('pd-form').addEventListener('submit', async ev => {
  ev.preventDefault()
  const msg = $('pd-input').value
  if (!client?._ioAlive() || !msg.trim() || busy) return
  busy = true
  setConnected(true)
  try {
    await client.sendPd(msg)
    $('pd-input').value = ''
  } catch (e) {
    log(`send failed: ${e.message || e}`, 'sync')
    if (!client?._ioAlive()) {
      try {
        await reconnect()
        applyConnectedState(await client.status().catch(() => null))
      } catch (_) {
        client = null
        setConnected(false)
        statusEl.textContent = 'Disconnected'
      }
    }
  } finally {
    busy = false
    applyConnectedState(client?.lastStatus ? parseStatus(client.lastStatus) : null)
  }
})

$('reload-btn').addEventListener('click', async () => {
  if (!client) return
  try {
    await client.reload()
  } catch (e) {
    log(`reload failed: ${e.message || e}`, 'sync')
  }
})

$('reset-btn').addEventListener('click', async () => {
  if (!client?._ioAlive()) return
  busy = true
  try {
    statusEl.textContent = 'Resetting…'
    await client.resetDevice()
    client = await reconnect({ afterReset: true })
    applyConnectedState(await client.status().catch(() => null))
  } catch (e) {
    log(`reset failed: ${e.message || e}`, 'sync')
    client = null
    setConnected(false)
    statusEl.textContent = 'Disconnected'
  } finally {
    busy = false
    if (client?._ioAlive()) {
      applyConnectedState(client.lastStatus ? parseStatus(client.lastStatus) : null)
    }
  }
})

setConnected(false)

window.addEventListener('pageshow', ev => {
  if (!ev.persisted) return
  if (client?._ioAlive()) return
  if (!watchWanted && !busy) return
  reconnect()
    .then(() => applyConnectedState(client?.lastStatus ? parseStatus(client.lastStatus) : null))
    .catch(() => {
      statusEl.textContent = 'Disconnected'
      setConnected(false)
    })
})

try {
  if (FOLDER_WATCH_OK) {
    requireBrowserApis()
  } else if (!FOLDER_SYNC_OK) {
    $('folder-btn').disabled = true
    $('watch-hint').textContent =
      'Folder pick needs Chrome/Edge (accept the certificate warning first).'
  }
  setTimeout(() => {
    connectClient().catch(e => {
      log(`connect failed: ${e.message || e}`, 'sync')
      statusEl.textContent = 'Not connected'
      setConnected(false)
    })
  }, 400)
} catch (e) {
  log(e.message, 'sync')
  statusEl.textContent = e.message
}
