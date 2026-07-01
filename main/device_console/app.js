/* ESPD device console — HTTPS + WSS /ws (monitor + Pd + folder sync). */
'use strict'

const STATUS_RE = /^\+OK STATUS sdcard=(yes|no) internal=(yes|no)$/
const PUT_DONE_RE = /^\+OK PUT done ([0-9a-fA-F]{8})$/
const PUT_ACK_RE = /^\+OK PUT ack (\d+)$/
const PUT_READY_RE = /^\+OK PUT ready window=(\d+)$/
const LIST_DONE_RE = /^\+OK LIST done (\d+)$/
const DEBOUNCE_MS = 350

const $ = id => document.getElementById(id)
const logEl = $('log')
const statusEl = $('status')

let client = null
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
  while (logEl.children.length > 500) logEl.removeChild(logEl.firstChild)
  logEl.scrollTop = logEl.scrollHeight
}

const FOLDER_SYNC_OK =
  window.isSecureContext && typeof window.showDirectoryPicker === 'function'
const FOLDER_WATCH_OK =
  FOLDER_SYNC_OK && 'FileSystemObserver' in window

function setConnected(on) {
  $('connect-btn').disabled = on || busy
  $('disconnect-btn').disabled = !on || busy
  $('pd-input').disabled = !on || busy
  $('pd-btn').disabled = !on || busy
  $('reload-btn').disabled = !on || busy
  $('reset-btn').disabled = !on || busy
  $('sync-btn').disabled = !on || busy || !syncDirHandle
  $('folder-btn').disabled = busy || !FOLDER_SYNC_OK
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
  if (!client || !syncDirHandle || !watchWanted) return
  const n = syncMtimes.size
  statusEl.textContent = `Watching ${n} file${n === 1 ? '' : 's'}`
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
    if (this.pendingReply) {
      this.pendingReply(line)
      this.pendingReply = null
    }
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
        this._resolveReply(line)
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

  open() {
    return new Promise((resolve, reject) => {
      this.stopped = false
      this.disconnected = false
      const ws = new WebSocket(wsUrl())
      ws.binaryType = 'arraybuffer'
      this.ws = ws
      const t = setTimeout(() => reject(new Error('WebSocket connect timeout')), 8000)
      ws.onopen = () => { clearTimeout(t); resolve() }
      ws.onerror = () => {
        clearTimeout(t)
        reject(new Error(
          'WebSocket error — accept the certificate warning, reload, and try again'
        ))
      }
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
    if (this.ws) {
      try { this.ws.close() } catch (_) {}
      this.ws = null
    }
  }

  _waitReply(timeoutMs) {
    return new Promise((resolve, reject) => {
      const t = setTimeout(() => {
        this.pendingReply = null
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

  async command(text, timeoutMs = 10000) {
    if (!this._ioAlive()) throw new Error('disconnected')
    this.pendingReply = null
    this.onLog(`→ ${text.trim()}`, 'sync')
    await this._writeBytes(new TextEncoder().encode(text.endsWith('\n') ? text : `${text}\n`))
    return this._waitReply(timeoutMs)
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
    this.pendingReply = null
    this.listPaths = []
    this.listActive = true
    try {
      this.onLog('→ LIST', 'sync')
      await this._writeBytes(new TextEncoder().encode('LIST\n'))
      const line = await this._waitReply(timeoutMs)
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
        await this._writeBytes(part)
        const ackLine = await this._waitReply(ackTimeout)
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

async function connectClient() {
  const c = new EspdWsClient({
    onLine: (line, kind) => log(kind === 'dev' ? `← ${line}` : line, kind === 'dev' ? 'dev' : ''),
    onLog: (msg, cls) => log(msg, cls || 'sync'),
    onDisconnect: () => {
      client = null
      setConnected(false)
      if (watchWanted) {
        statusEl.textContent = 'Disconnected — reconnecting…'
        ensureClient().then(() => setWatchingStatus()).catch(() => {
          statusEl.textContent = 'Disconnected'
        })
      } else {
        statusEl.textContent = 'Disconnected'
      }
    },
  })
  await c.open()
  const info = await c.status()
  client = c
  setConnected(true)
  statusEl.textContent = `Connected · sync target ${syncStorePath(info)}`
  return c
}

async function reconnect() {
  await client?.close().catch(() => {})
  client = null
  for (let i = 0; i < 30; i++) {
    try {
      return await connectClient()
    } catch (_) {
      await new Promise(r => setTimeout(r, 500))
    }
  }
  throw new Error('reconnect failed')
}

async function ensureClient() {
  if (client && client._ioAlive()) return client
  statusEl.textContent = 'Connecting…'
  return reconnect()
}

async function runSync(label, onlyRels = null) {
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
      client = await reconnect()
    } else if (reloadNeeded) {
      log('RELOAD', 'sync')
      await client.reload()
    }
    log(`sync done (${uploaded} uploaded, ${skipped} unchanged)`, 'sync')
    await refreshSyncMtimes()
    await client.status().catch(() => {})
    setWatchingStatus()
  } catch (e) {
    log(`sync error: ${e.message || e}`, 'sync')
    try { client = await reconnect() } catch (_) { client = null }
    setWatchingStatus()
  } finally {
    busy = false
    watchPaused = false
    setConnected(!!client)
    if (watchWanted && client) await startSyncWatch()
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
  setConnected(!!client)
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
  stopSyncWatch()
  await client?.close().catch(() => {})
  client = null
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
  if (!client || !msg.trim()) return
  try {
    const line = await client.sendPd(msg)
    log(`← ${line}`, 'dev')
    $('pd-input').value = ''
  } catch (e) {
    log(`send failed: ${e.message || e}`, 'sync')
  }
})

$('reload-btn').addEventListener('click', async () => {
  if (!client) return
  try {
    const line = await client.reload()
    log(`← ${line}`, 'dev')
  } catch (e) {
    log(`reload failed: ${e.message || e}`, 'sync')
  }
})

$('reset-btn').addEventListener('click', async () => {
  if (!client) return
  try {
    await client.resetDevice()
    client = await reconnect()
  } catch (e) {
    log(`reset failed: ${e.message || e}`, 'sync')
  }
})

setConnected(false)
try {
  if (FOLDER_WATCH_OK) {
    requireBrowserApis()
  } else if (!FOLDER_SYNC_OK) {
    $('folder-btn').disabled = true
    $('watch-hint').textContent =
      'Folder pick needs Chrome/Edge (accept the certificate warning first).'
  }
  setTimeout(() => $('connect-btn').click(), 400)
} catch (e) {
  log(e.message, 'sync')
  statusEl.textContent = e.message
}
