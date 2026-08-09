const tests = [];

globalThis.Deno = Object.freeze({
  test(name, operation) {
    tests.push({ name, operation });
  }
});

await import("./soil_api_test.ts");

for (const { name, operation } of tests) {
  await operation();
  console.log(`ok - ${name}`);
}

console.log(`${tests.length} backend tests passed`);
