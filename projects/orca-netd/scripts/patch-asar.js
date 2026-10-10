#!/usr/bin/env node
'use strict'
// Idempotently prepend the CGNAT preload to Orca's packaged main, then
// refresh ElectronAsarIntegrity and ad-hoc sign the app. Quit Orca first.
// Requires @electron/asar (ORCA_ASAR_TOOL or /tmp/orca-asar-tool).

const { execFileSync } = require('child_process')
const crypto = require('crypto')
const fs = require('fs')
const path = require('path')

const app = process.env.ORCA_APP || '/Applications/Orca.app'
const asarPath = path.join(app, 'Contents/Resources/app.asar')
const plistPath = path.join(app, 'Contents/Info.plist')
const preload = process.env.ORCA_NETD_PRELOAD ||
  path.join(process.env.HOME, '.local/share/orca-netd/socks-cgnat-preload.js')
const requireLine = `require(${JSON.stringify(preload)});`
const toolRoot = process.env.ORCA_ASAR_TOOL || '/tmp/orca-asar-tool'

function asarBin() {
  const local = path.join(toolRoot, 'node_modules/@electron/asar/bin/asar.js')
  if (fs.existsSync(local)) return local
  return 'asar'
}

function plistHash(plist, digest) {
  const key = '<key>ElectronAsarIntegrity</key>'
  const at = plist.indexOf(key)
  if (at < 0) throw new Error('ElectronAsarIntegrity missing')
  const slice = plist.slice(at)
  const replaced = slice.replace(
    /(<key>Resources\/app\.asar<\/key>\s*<dict>\s*<key>algorithm<\/key>\s*<string>SHA256<\/string>\s*<key>hash<\/key>\s*<string>)[0-9a-fA-F]+(<\/string>)/,
    `$1${digest}$2`
  )
  if (replaced === slice) throw new Error('could not replace app.asar hash')
  return plist.slice(0, at) + replaced
}

function main() {
  if (!fs.existsSync(preload)) throw new Error('preload missing: ' + preload)
  const raw = fs.readFileSync(asarPath)
  if (raw.includes('orca-netd/socks-cgnat-preload.js')) {
    console.log('asar already requires the preload')
    return
  }
  const work = fs.mkdtempSync(path.join('/tmp', 'orca-asar-'))
  const bin = asarBin()
  const node = process.execPath
  execFileSync(node, [bin, 'extract', asarPath, work], { stdio: 'inherit' })
  const mainJs = path.join(work, 'out/main/index.js')
  const source = fs.readFileSync(mainJs, 'utf8')
  if (!source.startsWith(requireLine)) {
    fs.writeFileSync(mainJs, requireLine + '\n' + source)
  }
  const packed = path.join('/tmp', 'orca-app-patched-' + process.pid + '.asar')
  execFileSync(node, [bin, 'pack', work, packed], { stdio: 'inherit' })
  const digest = crypto.createHash('sha256').update(fs.readFileSync(packed)).digest('hex')
  fs.copyFileSync(packed, asarPath)
  const plist = fs.readFileSync(plistPath, 'utf8')
  fs.writeFileSync(plistPath, plistHash(plist, digest))
  execFileSync('codesign', ['--force', '--sign', '-', app], { stdio: 'inherit' })
  console.log('patched asar sha256', digest)
}

main()
