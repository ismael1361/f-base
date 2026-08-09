import { globSync } from "node:fs";
import { spawn } from "node:child_process";

(async () => {
  const files = globSync("src/**/*.test.ts");

  for (const file of files) {
    console.log(``);
    console.log("=".repeat(100));
    console.log(` 🧪 Executando teste: ${file}`);
    console.log("=".repeat(100), "\n");
    await import(`../../${file}`);
  }
})();
