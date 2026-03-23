import fs from "fs";

const file = process.argv[2];
if (!file) {
  console.error("Usage: bun bf.js <source.b>");
  process.exit(1);
}

const src = fs.readFileSync(file, "utf-8");

const brackets = {};
const stack = [];
for (let i = 0; i < src.length; i++) {
  if (src[i] === "[") {
    stack.push(i);
  } else if (src[i] === "]") {
    const j = stack.pop();
    brackets[j] = i;
    brackets[i] = j;
  }
}

const mem = new Uint8Array(65536);
let dp = 0;
let ip = 0;

while (ip < src.length) {
  switch (src[ip]) {
    case ">":
      dp++;
      break;
    case "<":
      dp--;
      break;
    case "+":
      mem[dp]++;
      break;
    case "-":
      mem[dp]--;
      break;
    case ".":
      process.stdout.write(String.fromCharCode(mem[dp]));
      break;
    case ",": {
      const buf = Buffer.alloc(1);
      fs.readSync(0, buf, 0, 1);
      mem[dp] = buf[0];
      break;
    }
    case "[":
      if (mem[dp] === 0) {
        ip = brackets[ip];
      }
      break;
    case "]":
      if (mem[dp] !== 0) {
        ip = brackets[ip];
      }
      break;
  }
  ip++;
}
