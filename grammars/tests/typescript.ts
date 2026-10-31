import type { Readable } from "stream";

export interface User {
  id: number;
  name: string;
  tags?: readonly string[];
}

type Handler<T> = (value: T) => Promise<void>;

enum Color { Red = "red", Green = "green" }

export class Store<T extends User> {
  private items = new Map<number, T>();

  constructor(private readonly limit: number = 100) {}

  add(item: T): boolean {
    if (this.items.size >= this.limit) return false;
    this.items.set(item.id, item); // keep it
    return true;
  }
}

const greet = (u: User): string => `hi ${u.name as string}`;
let color: Color = Color.Red;
