'use strict'
// Patches Electron's Node net.Socket. Must stay JavaScript: a Rust helper
// cannot intercept the already-normalized connect() inside this process.
// netd runs with --tun=userspace-networking, so the kernel has no route to
// 100.64.0.0/10. Redirect those TCP connects to the local forwarder.
// Do not leak NODE_OPTIONS to children.

if (!process.env.ORCA_NETD_PRELOAD_KEEP) {
  delete process.env.NODE_OPTIONS
}

const net = require('net')
const FORWARD_HOST = '127.0.0.1'
const FORWARD_PORT = Number(process.env.ORCA_NETD_FORWARD_PORT || 16768)

function isCgnat(host) {
  if (typeof host !== 'string') return false
  const match = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(host)
  if (!match) return false
  const parts = match.slice(1).map(Number)
  if (parts.some((n) => n > 255)) return false
  return parts[0] === 100 && parts[1] >= 64 && parts[1] <= 127
}

function parsedConnect(args) {
  const first = args[0]
  if (Array.isArray(first) && first[0] && typeof first[0] === 'object') {
    return { kind: 'normalized', options: first[0], normalized: first }
  }
  if (first && typeof first === 'object') return { kind: 'options', options: first }
  if (typeof first === 'number') {
    return {
      kind: 'port',
      options: { host: typeof args[1] === 'string' ? args[1] : undefined, port: first }
    }
  }
  return null
}

function asBuffer(chunk, encoding) {
  if (Buffer.isBuffer(chunk)) return chunk
  if (typeof chunk === 'string') {
    return Buffer.from(chunk, typeof encoding === 'string' ? encoding : 'utf8')
  }
  return Buffer.alloc(0)
}

// Node's Socket.connect assigns its own `write` and overwrites anything set
// before it returns. Hook `_write` first (the prototype write always lands
// there) and wrap `write` again after connect returns.
function armDestHeader(sock, dest) {
  if (sock.__orcaNetdHeaderArmed === dest && sock.write && sock.write.__orcaNetd) return
  if (!sock.__orcaNetdTake) {
    const header = Buffer.from('ORCA1 ' + dest + '\n')
    let sent = false
    sock.__orcaNetdTake = function take() {
      if (sent) return null
      sent = true
      return header
    }
  }
  const take = sock.__orcaNetdTake

  const origLow = sock._write
  const origLowv = sock._writev
  if (typeof origLow === 'function' && !origLow.__orcaNetd) {
    sock._write = function patchedLow(data, encoding, cb) {
      const h = take()
      if (!h) return origLow.call(this, data, encoding, cb)
      return origLow.call(this, Buffer.concat([h, asBuffer(data, encoding)]), encoding, cb)
    }
    sock._write.__orcaNetd = true
  }
  if (typeof origLowv === 'function' && !origLowv.__orcaNetd) {
    sock._writev = function patchedLowv(chunks, cb) {
      const h = take()
      if (!h) return origLowv.call(this, chunks, cb)
      return origLowv.call(this, [{ chunk: h, encoding: 'buffer' }].concat(chunks), cb)
    }
    sock._writev.__orcaNetd = true
  }

  const origWrite = sock.write
  const origEnd = sock.end
  function patchedWrite(chunk, encoding, cb) {
    const h = take()
    if (!h) return origWrite.call(this, chunk, encoding, cb)
    if (typeof encoding === 'function') {
      cb = encoding
      encoding = undefined
    }
    return origWrite.call(this, Buffer.concat([h, asBuffer(chunk, encoding)]), cb)
  }
  patchedWrite.__orcaNetd = true
  function patchedEnd(chunk, encoding, cb) {
    if (chunk === undefined || typeof chunk === 'function') {
      return origEnd.apply(this, arguments)
    }
    const h = take()
    if (!h) return origEnd.call(this, chunk, encoding, cb)
    if (typeof encoding === 'function') {
      cb = encoding
      encoding = undefined
    }
    return origEnd.call(this, Buffer.concat([h, asBuffer(chunk, encoding)]), cb)
  }
  patchedEnd.__orcaNetd = true
  sock.write = patchedWrite
  sock.end = patchedEnd
  sock.__orcaNetdHeaderArmed = dest
}

function rewrite(args, parsed) {
  const options = {
    ...parsed.options,
    host: FORWARD_HOST,
    hostname: FORWARD_HOST,
    port: FORWARD_PORT
  }
  if (parsed.kind === 'normalized') {
    const copy = [options, parsed.normalized[1]]
    for (const symbol of Object.getOwnPropertySymbols(parsed.normalized)) {
      copy[symbol] = parsed.normalized[symbol]
    }
    return [copy]
  }
  if (parsed.kind === 'options') {
    const copy = args.slice()
    copy[0] = options
    return copy
  }
  const copy = args.slice()
  copy[0] = FORWARD_PORT
  copy[1] = FORWARD_HOST
  return copy
}

if (!net.Socket.prototype.__orcaNetdPatched) {
  const orig = net.Socket.prototype.connect
  net.Socket.prototype.connect = function patchedConnect(...args) {
    const parsed = parsedConnect(args)
    const host = parsed && (parsed.options.host || parsed.options.hostname)
    const port = parsed && Number(parsed.options.port)
    if (!parsed || !isCgnat(host) || !port) return orig.apply(this, args)
    const dest = host + ':' + port
    this.__orcaNetdDest = dest
    // _write is hooked inside armDestHeader. connect() then replaces `write`,
    // so arm again after it returns and once the socket actually connects.
    armDestHeader(this, dest)
    const ret = orig.apply(this, rewrite(args, parsed))
    armDestHeader(this, dest)
    if (typeof this.once === 'function') this.once('connect', () => armDestHeader(this, dest))
    return ret
  }
  net.Socket.prototype.__orcaNetdPatched = true
}

module.exports = { isCgnat, parsedConnect, rewrite, armDestHeader }
