interface User {
  readonly id: number;
  name?: string;
}

const label = (user: User): string => user.name ?? `User ${user.id}`;
