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
    this.__orcaNetdDest = host + ':' + port
    return orig.apply(this, rewrite(args, parsed))
  }
  net.Socket.prototype.__orcaNetdPatched = true
}

module.exports = { isCgnat, parsedConnect, rewrite }
