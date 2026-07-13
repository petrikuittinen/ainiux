export type Result<T> = { ok: true; value: T } | { ok: false; error: Error };

export const success = <T>(value: T): Result<T> => ({ ok: true, value });
