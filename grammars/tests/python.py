import os
from dataclasses import dataclass

MAX_ITEMS = 10


@dataclass
class Item:
    """A thing with a name."""
    name: str
    count: int = 0

    def label(self) -> str:
        return f"{self.name} x{self.count}"


def main(paths: list[str]) -> None:
    # print every file
    for i, p in enumerate(paths):
        if i >= MAX_ITEMS or not os.path.exists(p):
            break
        print(Item(p, i).label(), None, True, 3.5e-2)
    lambda x: x ** 2


if __name__ == "__main__":
    main(["a.txt", 'b.txt'])
