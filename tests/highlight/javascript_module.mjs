export async function loadValue() {
  const result = await Promise.resolve({ value: 17 });
  return result?.value ?? 0;
}
