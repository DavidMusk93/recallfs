import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";

const skillRoot = path.resolve(import.meta.dirname, "..");
const skill = fs.readFileSync(path.join(skillRoot, "SKILL.md"), "utf8");
const proof = fs.readFileSync(
  path.join(skillRoot, "references/proof-contract.md"),
  "utf8",
);

const high32 = (left, right) => (left * right) >> 32n;

{
  const x = 3724842645n;
  const divisor = 7n;
  const reciprocal = ((1n << 32n) + divisor - 1n) / divisor;
  assert.equal(reciprocal, 613566757n);
  assert.equal(high32(x, reciprocal), 532120378n);
  assert.equal(x / divisor, 532120377n);
}

{
  const q15Min = -32768n;
  const q15Max = 32767n;
  const products = [
    q15Min * q15Min,
    q15Min * q15Max,
    q15Max * q15Max,
  ];
  const i32Min = -(1n << 31n);
  const i32Max = (1n << 31n) - 1n;
  assert(products.every((value) => value >= i32Min && value <= i32Max));

  const q31Min = -(1n << 31n);
  const q31Product = q31Min * q31Min;
  assert(q31Product > i32Max);
  assert(q31Product <= (1n << 63n) - 1n);
}

{
  const truncQuotient = -1n / 7n;
  const truncRemainder = -1n % 7n;
  assert.equal(truncQuotient, 0n);
  assert.equal(truncRemainder, -1n);

  const floorQuotient = -1n;
  const floorRemainder = -1n - floorQuotient * 7n;
  assert.equal(floorRemainder, 6n);
}

{
  const shiftedWithBias = (1n + 2n) >> 2n;
  const roundedAfterBias = 1n;
  assert.equal(shiftedWithBias, 0n);
  assert.equal(roundedAfterBias, 1n);
}

{
  const validateRange = (range, wordBits) => {
    const max = (1n << BigInt(wordBits)) - 1n;
    if (range < 1n || range > max) throw new RangeError("invalid range");
  };
  assert.throws(() => validateRange(0n, 8), RangeError);
  assert.doesNotThrow(() => validateRange(1n, 8));
  assert.doesNotThrow(() => validateRange(255n, 8));
  assert.throws(() => validateRange(256n, 8), RangeError);
  assert.equal((255n * 255n) >> 8n, 254n);
}

{
  const parseCentsTiesAway = (text) => {
    const match = /^([+-]?)(\d+)\.(\d+)$/.exec(text);
    assert(match);
    const sign = match[1] === "-" ? -1n : 1n;
    const whole = BigInt(match[2]);
    const fraction = `${match[3]}000`;
    let cents = whole * 100n + BigInt(fraction.slice(0, 2));
    if (fraction[2] >= "5") cents += 1n;
    return sign * cents;
  };

  assert(1.005 * 100 < 100.5);
  assert.equal(parseCentsTiesAway("1.005"), 101n);
}

assert(!proof.includes("widen(a) << F"));
assert(proof.includes("widen(a) * 2^F"));
assert(proof.includes("1 <= range <= 2^w - 1"));
assert(proof.includes("parse_decimal_to_scaled_integer"));
assert(skill.includes("modular mapping"));
assert(skill.includes("decimal integers"));

console.log("fixed-point numeric contracts passed");
