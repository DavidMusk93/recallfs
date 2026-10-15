'use strict'
// TCP forwarder: 127.0.0.1:16768 -> netd SOCKS5 -> the remote Orca endpoint.
// netd is userspace (--tun=userspace-networking); the kernel has no 100.64.0.0/10 route.
// This process is supervised by Tunnel Manager (service orca-netd), not a LaunchAgent.

const net = require('net')
const fs = require('fs')
const path = require('path')

function stateDir() {
  return process.env.ORCA_NETD_STATE_DIR || path.join(process.env.HOME, '.local/share/orca-netd')
}

function loadEnvFile(file) {
  let text
  try {
    text = fs.readFileSync(file, 'utf8')
  } catch (err) {
    if (err.code === 'ENOENT') return
    throw err
  }
  for (const line of text.split('\n')) {
    const trimmed = line.trim()
    if (!trimmed || trimmed.startsWith('#')) continue
    const eq = trimmed.indexOf('=')
    if (eq < 1) continue
    const key = trimmed.slice(0, eq).trim()
    const value = trimmed.slice(eq + 1).trim()
    if (process.env[key] === undefined) process.env[key] = value
  }
}

function loadConfig() {
  const dir = stateDir()
  loadEnvFile(process.env.ORCA_NETD_CONFIG || path.join(dir, 'config.env'))
  return {
    dir,
    listenHost: '127.0.0.1',
    listenPort: Number(process.env.ORCA_NETD_FORWARD_PORT || 16768),
    socksHost: process.env.ORCA_NETD_SOCKS_HOST || '127.0.0.1',
    socksPort: Number(process.env.ORCA_NETD_SOCKS_PORT || 1055),
    destHost: process.env.ORCA_NETD_DEST_HOST || '',
    destPort: Number(process.env.ORCA_NETD_DEST_PORT || 6768)
  }
}

function ipv4Parts(host) {
  if (typeof host !== 'string') return null
  const match = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(host)
  if (!match) return null
  const parts = match.slice(1).map(Number)
  if (parts.some((n) => n > 255)) return null
  return parts
}

function isCgnat(host) {
  const parts = ipv4Parts(host)
  return !!parts && parts[0] === 100 && parts[1] >= 64 && parts[1] <= 127
}

// First client bytes may be "ORCA1 <cgnat>:6768\n" so one listener can reach
// more than one runtime. Anything else is the real stream and uses DEST_*.
function takeDest(cfg, client, cb) {
  const magic = 'ORCA1 '
  let buf = Buffer.alloc(0)
  let finished = false
  function finish(host, port, rest) {
    if (finished) return
    finished = true
    clearTimeout(timer)
    client.removeListener('readable', pull)
    client.removeListener('error', onErr)
    if (rest.length) client.unshift(rest)
    cb(null, host, port)
  }
  function onErr(err) {
    if (finished) return
    finished = true
    clearTimeout(timer)
    client.removeListener('readable', pull)
    cb(err)
  }
  function pull() {
    if (finished) return
    let chunk
    while ((chunk = client.read()) !== null) buf = Buffer.concat([buf, chunk])
    if (!buf.length) return
    const prefix = buf.subarray(0, Math.min(buf.length, magic.length)).toString('utf8')
    if (!magic.startsWith(prefix)) {
      finish(cfg.destHost, cfg.destPort, buf)
      return
    }
    const nl = buf.indexOf(0x0a)
    if (nl < 0) {
      if (buf.length > 64) finish(cfg.destHost, cfg.destPort, buf)
      return
    }
    const line = buf.subarray(0, nl).toString('utf8').replace(/\r$/, '')
    const match = /^ORCA1 (\d{1,3}(?:\.\d{1,3}){3}):(\d{1,5})$/.exec(line)
    const port = match && Number(match[2])
    if (!match || !isCgnat(match[1]) || port !== 6768) {
      finish(cfg.destHost, cfg.destPort, buf)
      return
    }
    finish(match[1], port, buf.subarray(nl + 1))
  }
  const timer = setTimeout(() => finish(cfg.destHost, cfg.destPort, buf), 3000)
  client.on('error', onErr)
  client.on('readable', pull)
  pull()
}

function log(cfg, msg) {
  const line = new Date().toISOString() + ' ' + msg + '\n'
  try {
    fs.mkdirSync(cfg.dir, { recursive: true })
    fs.appendFileSync(path.join(cfg.dir, 'forward.log'), line)
  } catch (_) {}
}

function socksDial(cfg, host, port, cb) {
  const ip = ipv4Parts(host)
  if (!ip) {
    cb(new Error('dest_not_ipv4'))
    return
  }
  const socket = net.connect(cfg.socksPort, cfg.socksHost)
  let stage = 0
  let buf = Buffer.alloc(0)
  socket.once('error', (err) => cb(err))
  socket.on('data', onData)
  socket.once('connect', () => socket.write(Buffer.from([0x05, 0x01, 0x00])))

  function fail(err) {
    socket.removeListener('data', onData)
    socket.destroy()
    cb(err)
  }
  function onData(chunk) {
    buf = Buffer.concat([buf, chunk])
    if (stage === 0) {
      if (buf.length < 2) return
      if (buf[0] !== 0x05 || buf[1] !== 0x00) return fail(new Error('socks_auth'))
      buf = buf.subarray(2)
      stage = 1
      socket.write(Buffer.from([
        0x05, 0x01, 0x00, 0x01,
        ip[0], ip[1], ip[2], ip[3],
        (port >> 8) & 0xff, port & 0xff
      ]))
    }
    if (stage === 1) {
      if (buf.length < 4) return
      if (buf[1] !== 0x00) return fail(new Error('socks_rep_' + buf[1]))
      const atyp = buf[3]
      const need = atyp === 0x01 ? 10 : atyp === 0x04 ? 22 : 0
      if (!need || buf.length < need) {
        if (!need) return fail(new Error('socks_atyp'))
        return
      }
      const extra = buf.subarray(need)
      socket.removeListener('data', onData)
      if (extra.length) socket.unshift(extra)
      cb(null, socket)
    }
  }
}

function start(cfg) {
  if (!cfg.destHost || !ipv4Parts(cfg.destHost)) {
    log(cfg, 'missing ORCA_NETD_DEST_HOST (ipv4)')
    process.exit(1)
  }
  const server = net.createServer((client) => {
    takeDest(cfg, client, (err, host, port) => {
      if (err) {
        log(cfg, 'dest ' + err.message)
        client.destroy()
        return
      }
      socksDial(cfg, host, port, (dialErr, upstream) => {
        if (dialErr || !upstream) {
          log(cfg, 'dial fail ' + host + ':' + port + ' ' + (dialErr && dialErr.message))
          client.destroy()
          return
        }
        log(cfg, 'bridged ' + host + ':' + port)
        const drop = (e) => {
          if (e) log(cfg, 'pipe ' + e.message)
          client.destroy()
          upstream.destroy()
        }
        client.on('error', drop)
        upstream.on('error', drop)
        client.pipe(upstream)
        upstream.pipe(client)
      })
    })
  })
  server.on('error', (err) => {
    log(cfg, 'listen error ' + err.message)
    process.exit(1)
  })
  server.listen(cfg.listenPort, cfg.listenHost, () => {
    log(cfg, 'listening ' + cfg.listenHost + ':' + cfg.listenPort + ' -> ' + cfg.destHost + ':' + cfg.destPort)
  })
  return server
}

module.exports = { loadConfig, ipv4Parts, isCgnat, takeDest, socksDial, start }

if (require.main === module) {
  start(loadConfig())
}
