type Props = { count: number; onClick(): void };

export const Counter = ({ count, onClick }: Props) => (
  <button onClick={onClick}>Count: {count}</button>
);
