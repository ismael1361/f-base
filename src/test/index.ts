import { globSync } from "node:fs";
import { spawn } from "node:child_process";

(async () => {
  const files = globSync("src/**/*.test.ts");

  for (const file of files) {
    await import(`../../${file}`);
  }
})();
