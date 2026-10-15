'use strict'
// Offline checks: CGNAT rewrite, and one SOCKS5 bridge against a fake proxy.

const assert = require('assert')
const fs = require('fs')
const net = require('net')
const os = require('os')
const path = require('path')
const { spawn } = require('child_process')

const root = path.join(__dirname, '..')
const preload = require(path.join(root, 'socks-cgnat-preload.js'))

function checkCgnat() {
  assert.equal(preload.isCgnat('100.64.0.1'), true)
  assert.equal(preload.isCgnat('100.127.255.255'), true)
  assert.equal(preload.isCgnat('100.63.0.1'), false)
  assert.equal(preload.isCgnat('100.128.0.1'), false)
  assert.equal(preload.isCgnat('10.0.0.1'), false)
  assert.equal(preload.isCgnat('100.64.0.256'), false)
  assert.equal(preload.isCgnat('grokbox'), false)
}

function checkRewrite() {
  const seen = []
  const orig = net.Socket.prototype.connect
  net.Socket.prototype.connect = function stub(...args) {
    seen.push(args)
    return this
  }
  const stub = net.Socket.prototype.connect
  net.Socket.prototype.connect = function (...args) {
    const parsed = preload.parsedConnect(args)
    const host = parsed && (parsed.options.host || parsed.options.hostname)
    const port = parsed && Number(parsed.options.port)
    if (!parsed || !preload.isCgnat(host) || !port) return stub.apply(this, args)
    this.__orcaNetdDest = host + ':' + port
    return stub.apply(this, preload.rewrite(args, parsed))
  }

  const optionsSocket = new net.Socket()
  optionsSocket.connect({ host: '100.64.8.8', port: 6768 })
  assert.equal(optionsSocket.__orcaNetdDest, '100.64.8.8:6768')
  assert.equal(seen[0][0].host, '127.0.0.1')
  assert.equal(seen[0][0].port, Number(process.env.ORCA_NETD_FORWARD_PORT || 16768))

  const plain = new net.Socket()
  plain.connect({ host: '1.2.3.4', port: 80 })
  assert.equal(plain.__orcaNetdDest, undefined)
  assert.equal(seen[1][0].host, '1.2.3.4')

  const symbol = Symbol('normalizedArgs')
  const normalized = [{ host: '100.64.1.2', port: 443 }, null]
  normalized[symbol] = true
  const normalizedSocket = new net.Socket()
  normalizedSocket.connect(normalized)
  assert.equal(normalizedSocket.__orcaNetdDest, '100.64.1.2:443')
  assert.equal(seen[2][0][0].host, '127.0.0.1')
  assert.equal(seen[2][0][symbol], true)

  const byPort = new net.Socket()
  byPort.connect(6768, '100.127.0.9')
  assert.equal(byPort.__orcaNetdDest, '100.127.0.9:6768')
  assert.equal(seen[3][0], Number(process.env.ORCA_NETD_FORWARD_PORT || 16768))
  assert.equal(seen[3][1], '127.0.0.1')

  net.Socket.prototype.connect = orig
}

function listen(handler) {
  return new Promise((resolve) => {
    const server = net.createServer(handler)
    server.listen(0, '127.0.0.1', () => resolve(server))
  })
}

function fakeSocks(expectedHost, expectedPort) {
  return listen((socket) => {
    let stage = 0
    let buf = Buffer.alloc(0)
    socket.on('data', (chunk) => {
      buf = Buffer.concat([buf, chunk])
      if (stage === 0) {
        if (buf.length < 3) return
        socket.write(Buffer.from([0x05, 0x00]))
        buf = buf.subarray(3)
        stage = 1
      }
      if (stage === 1) {
        if (buf.length < 10) return
        assert.equal(buf[0], 0x05)
        assert.equal(buf[1], 0x01)
        assert.equal(buf[3], 0x01)
        const host = [buf[4], buf[5], buf[6], buf[7]].join('.')
        const port = (buf[8] << 8) | buf[9]
        assert.equal(host, expectedHost)
        assert.equal(port, expectedPort)
        const extra = buf.subarray(10)
        buf = extra
        stage = 2
        socket.write(Buffer.from([0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0]))
      }
      if (stage === 2 && buf.includes('ping')) {
        socket.end('pong')
      }
    })
  })
}

function waitForPort(port) {
  const deadline = Date.now() + 4000
  return new Promise((resolve, reject) => {
    const attempt = () => {
      const socket = net.connect(port, '127.0.0.1')
      socket.on('connect', () => {
        socket.end()
        resolve()
      })
      socket.on('error', () => {
        socket.destroy()
        if (Date.now() > deadline) reject(new Error('forwarder did not listen'))
        else setTimeout(attempt, 40)
      })
    }
    attempt()
  })
}

async function checkBridge() {
  const destHost = '10.1.2.3'
  const destPort = 6768
  const socks = await fakeSocks(destHost, destPort)
  const probe = await listen(() => {})
  const port = probe.address().port
  await new Promise((resolve) => probe.close(resolve))
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'orca-netd-'))
  const child = spawn(process.execPath, [path.join(root, 'forward.js')], {
    env: {
      ...process.env,
      ORCA_NETD_STATE_DIR: dir,
      ORCA_NETD_CONFIG: path.join(dir, 'missing.env'),
      ORCA_NETD_FORWARD_PORT: String(port),
      ORCA_NETD_SOCKS_HOST: '127.0.0.1',
      ORCA_NETD_SOCKS_PORT: String(socks.address().port),
      ORCA_NETD_DEST_HOST: destHost,
      ORCA_NETD_DEST_PORT: String(destPort)
    },
    stdio: ['ignore', 'ignore', 'pipe']
  })
  let stderr = ''
  child.stderr.on('data', (chunk) => {
    stderr += chunk
  })
  try {
    await waitForPort(port)
    const reply = await new Promise((resolve, reject) => {
      const socket = net.connect(port, '127.0.0.1', () => socket.write('ping'))
      const chunks = []
      socket.on('data', (chunk) => chunks.push(chunk))
      socket.on('end', () => resolve(Buffer.concat(chunks).toString()))
      socket.on('error', reject)
      setTimeout(() => reject(new Error('bridge timeout ' + stderr)), 3000)
    })
    assert.equal(reply, 'pong')
  } finally {
    child.kill('SIGTERM')
    await new Promise((resolve) => socks.close(resolve))
  }
}

function checkHeader() {
  const sock = new net.Socket()
  const chunks = []
  sock.write = (chunk) => {
    chunks.push(chunk)
    return true
  }
  sock.end = () => {}
  preload.armDestHeader(sock, '100.82.237.65:6768')
  sock.write('GET / HTTP/1.1\r\n')
  const first = Buffer.isBuffer(chunks[0]) ? chunks[0] : Buffer.from(chunks[0])
  assert.equal(first.toString(), 'ORCA1 100.82.237.65:6768\nGET / HTTP/1.1\r\n')
  sock.write('more')
  assert.equal(chunks[1], 'more')
}

// Node's http client writes after Socket.connect replaces `write`.
function checkHttpHeader() {
  return new Promise((resolve, reject) => {
    const server = net.createServer((sock) => {
      let buf = Buffer.alloc(0)
      sock.on('data', (chunk) => {
        buf = Buffer.concat([buf, chunk])
        if (!buf.includes('\r\n\r\n') && buf.length < 180) return
        sock.end()
        server.close()
        const text = buf.toString('utf8')
        try {
          assert.ok(text.startsWith('ORCA1 100.82.237.65:6768\nGET /web-index.html '), text.slice(0, 80))
          assert.equal(text.indexOf('ORCA1 '), text.lastIndexOf('ORCA1 '))
          resolve()
        } catch (err) {
          reject(err)
        }
      })
    })
    server.listen(0, '127.0.0.1', () => {
      const port = server.address().port
      const child = spawn(process.execPath, ['-e', "require('http').get('http://100.82.237.65:6768/web-index.html', (res) => res.resume()).on('error', () => {})"], {
        env: {
          ...process.env,
          ORCA_NETD_FORWARD_PORT: String(port),
          ORCA_NETD_PRELOAD_KEEP: '1',
          NODE_OPTIONS: '--require ' + path.join(root, 'socks-cgnat-preload.js')
        },
        stdio: 'ignore'
      })
      const timer = setTimeout(() => {
        child.kill('SIGTERM')
        server.close()
        reject(new Error('http header timeout'))
      }, 4000)
      server.on('close', () => {
        clearTimeout(timer)
        child.kill('SIGTERM')
      })
    })
  })
}

async function checkHeaderBridge() {
  const hinted = '100.82.237.65'
  const socks = await fakeSocks(hinted, 6768)
  const probe = await listen(() => {})
  const port = probe.address().port
  await new Promise((resolve) => probe.close(resolve))
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'orca-netd-hdr-'))
  const child = spawn(process.execPath, [path.join(root, 'forward.js')], {
    env: {
      ...process.env,
      ORCA_NETD_STATE_DIR: dir,
      ORCA_NETD_CONFIG: path.join(dir, 'missing.env'),
      ORCA_NETD_FORWARD_PORT: String(port),
      ORCA_NETD_SOCKS_HOST: '127.0.0.1',
      ORCA_NETD_SOCKS_PORT: String(socks.address().port),
      ORCA_NETD_DEST_HOST: '10.1.2.3',
      ORCA_NETD_DEST_PORT: '6768'
    },
    stdio: ['ignore', 'ignore', 'pipe']
  })
  let stderr = ''
  child.stderr.on('data', (chunk) => {
    stderr += chunk
  })
  try {
    await waitForPort(port)
    const reply = await new Promise((resolve, reject) => {
      const socket = net.connect(port, '127.0.0.1', () => {
        socket.write('ORCA1 ' + hinted + ':6768\nping')
      })
      const chunks = []
      socket.on('data', (chunk) => chunks.push(chunk))
      socket.on('end', () => resolve(Buffer.concat(chunks).toString()))
      socket.on('error', reject)
      setTimeout(() => reject(new Error('header bridge timeout ' + stderr)), 3000)
    })
    assert.equal(reply, 'pong')
  } finally {
    child.kill('SIGTERM')
    await new Promise((resolve) => socks.close(resolve))
  }
}

async function main() {
  checkCgnat()
  checkRewrite()
  checkHeader()
  await checkHttpHeader()
  await checkBridge()
  await checkHeaderBridge()
  console.log('orca-netd self-check ok')
}

main().catch((err) => {
  console.error(err)
  process.exit(1)
})
